/*
 * mtp_switch.c - MI_01 のドライバーの切り替え（UpdateDriverForPlugAndPlayDevices、画面を出さない）
 */
#include "mtp_switch.h"

#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

/* 対象の機器 ID（mtp_core.h の PID と同じ。0x0324 は仮対応） */
static const wchar_t *kHardwareIds[] = {
    L"USB\\VID_05AC&PID_0265&MI_01",
    L"USB\\VID_05AC&PID_0324&MI_01",
};

static void Log(MTP_SWITCH_LOG log, const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    if (log == NULL) {
        return;
    }
    va_start(ap, fmt);
    vswprintf_s(buf, ARRAYSIZE(buf), fmt, ap);
    va_end(ap);
    log(buf);
}

/*
 * 機器 ID が一致する、つながっている機器を探す。見つかれば、登録されているサービス名（HidUsb / WINUSB）を service に、
 * 「再起動が必要」か（新しいドライバーが登録されただけで、機器はまだ前のドライバーで動いている）を pending に入れる。
 * set と data は、呼び出し側が要るときだけ返す（返したら SetupDiDestroyDeviceInfoList で閉じる）。
 */
static BOOL FindDeviceEx(const wchar_t *hardwareId, wchar_t *service, DWORD serviceLen, BOOL *pending,
    HDEVINFO *outSet, SP_DEVINFO_DATA *outData)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA data;
    DWORD i;
    BOOL found = FALSE;

    if (pending) *pending = FALSE;
    if (outSet) *outSet = INVALID_HANDLE_VALUE;
    if (set == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    data.cbSize = sizeof(data);
    for (i = 0; !found && SetupDiEnumDeviceInfo(set, i, &data); i++) {
        wchar_t ids[2048];
        const wchar_t *p;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof(ids) - sizeof(wchar_t), NULL)) {
            continue;
        }
        ids[ARRAYSIZE(ids) - 1] = L'\0';
        for (p = ids; *p != L'\0'; p += wcslen(p) + 1) {
            if (_wcsicmp(p, hardwareId) == 0) {
                found = TRUE;
                break;
            }
        }
        if (found) {
            service[0] = L'\0';
            if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_SERVICE, NULL, (BYTE *)service, serviceLen * sizeof(wchar_t), NULL)) {
                service[0] = L'\0';
            }
            if (pending) {
                ULONG status = 0, problem = 0;
                if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) == CR_SUCCESS) {
                    *pending = (status & DN_NEED_RESTART) != 0 || problem == CM_PROB_NEED_RESTART;
                }
            }
        }
    }
    if (found && outSet && outData) {
        *outSet = set;
        *outData = data;
        return TRUE;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static BOOL FindDevice(const wchar_t *hardwareId, wchar_t *service, DWORD serviceLen, BOOL *pending)
{
    return FindDeviceEx(hardwareId, service, serviceLen, pending, NULL, NULL);
}

/*
 * 機器を止めて動かし直し、登録済みのドライバーで動かす（devcon restart と同じ）。
 * ほかのプログラムが機器を開いていると止められないので、FALSE が返る。
 */
static BOOL RestartDevice(const wchar_t *hardwareId)
{
    HDEVINFO set;
    SP_DEVINFO_DATA data;
    SP_PROPCHANGE_PARAMS pc;
    SP_DEVINSTALL_PARAMS_W ip;
    wchar_t service[128];
    BOOL ok = FALSE;

    if (!FindDeviceEx(hardwareId, service, ARRAYSIZE(service), NULL, &set, &data)) {
        return FALSE;
    }
    memset(&pc, 0, sizeof(pc));
    pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = DICS_PROPCHANGE;
    pc.Scope = DICS_FLAG_CONFIGSPECIFIC;
    pc.HwProfile = 0;
    if (SetupDiSetClassInstallParamsW(set, &data, &pc.ClassInstallHeader, sizeof(pc)) &&
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &data)) {
        memset(&ip, 0, sizeof(ip));
        ip.cbSize = sizeof(ip);
        ok = TRUE;
        if (SetupDiGetDeviceInstallParamsW(set, &data, &ip) && (ip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0) {
            ok = FALSE; /* 止められなかった（使用中） */
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

static MTP_DRIVER_MODE ModeOf(const wchar_t *service)
{
    if (_wcsicmp(service, L"WINUSB") == 0) return MTP_MODE_WINUSB;
    if (_wcsicmp(service, L"HidUsb") == 0) return MTP_MODE_HID;
    return MTP_MODE_UNKNOWN;
}

/* %windir%\INF\oem*.inf のうち、MAGITRA の WinUSB 用 INF（MtpUsb.DeviceDesc と提供元 MAGITRA を含む）を探す */
static BOOL FindMtpInf(wchar_t *out, DWORD outLen)
{
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL found = FALSE;

    if (GetWindowsDirectoryW(dir, MAX_PATH) == 0) {
        return FALSE;
    }
    swprintf_s(pattern, MAX_PATH, L"%s\\INF\\oem*.inf", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    do {
        wchar_t path[MAX_PATH];
        HANDLE f;
        char buf[8192];
        DWORD n = 0;
        swprintf_s(path, MAX_PATH, L"%s\\INF\\%s", dir, fd.cFileName);
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            continue;
        }
        if (ReadFile(f, buf, sizeof(buf) - 1, &n, NULL)) {
            buf[n] = '\0';
            if (strstr(buf, "MtpUsb.DeviceDesc") != NULL && strstr(buf, "MAGITRA") != NULL) {
                wcscpy_s(out, outLen, path);
                found = TRUE;
            }
        }
        CloseHandle(f);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

MTP_DRIVER_MODE MtpSwitchCurrentModeEx(int *pending)
{
    size_t i;
    if (pending) *pending = 0;
    for (i = 0; i < ARRAYSIZE(kHardwareIds); i++) {
        wchar_t service[128];
        BOOL p = FALSE;
        if (FindDevice(kHardwareIds[i], service, ARRAYSIZE(service), &p)) {
            if (pending) *pending = p ? 1 : 0;
            return ModeOf(service);
        }
    }
    return MTP_MODE_UNKNOWN;
}

MTP_DRIVER_MODE MtpSwitchCurrentMode(void)
{
    return MtpSwitchCurrentModeEx(NULL);
}

int MtpSwitchTo(MTP_DRIVER_MODE mode, MTP_SWITCH_LOG log)
{
    wchar_t inf[MAX_PATH];
    size_t i;
    int result = 0;

    if (mode == MTP_MODE_WINUSB) {
        if (!FindMtpInf(inf, MAX_PATH)) {
            Log(log, L"MAGITRA の WinUSB 用 INF がドライバーストアにありません");
            return -1;
        }
    } else if (mode == MTP_MODE_HID) {
        wchar_t dir[MAX_PATH];
        if (GetWindowsDirectoryW(dir, MAX_PATH) == 0) {
            return -1;
        }
        swprintf_s(inf, MAX_PATH, L"%s\\INF\\input.inf", dir);
    } else {
        return -1;
    }

    for (i = 0; i < ARRAYSIZE(kHardwareIds); i++) {
        wchar_t service[128];
        BOOL reboot = FALSE;
        BOOL pending = FALSE;
        BOOL done = FALSE;
        ULONGLONG start;
        int t;

        if (!FindDevice(kHardwareIds[i], service, ARRAYSIZE(service), &pending)) {
            continue;
        }
        if (ModeOf(service) == mode && !pending) {
            continue;
        }
        start = GetTickCount64();
        if (ModeOf(service) == mode) {
            /* 前回の切り替えで登録だけ済んで、機器が止められなかった。動かし直しだけを試す */
            Log(log, L"%s: %s は登録済みですが、まだ前のドライバーで動いています。動かし直します", kHardwareIds[i], service);
        } else {
            Log(log, L"%s: %s から %s へ切り替えます（%s）", kHardwareIds[i], service[0] ? service : L"(なし)",
                mode == MTP_MODE_WINUSB ? L"WINUSB" : L"HidUsb", inf);
            if (!UpdateDriverForPlugAndPlayDevicesW(NULL, kHardwareIds[i], inf, INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &reboot)) {
                Log(log, L"  切り替えに失敗しました（エラー 0x%08lX）", GetLastError());
                result = -1;
                continue;
            }
        }
        /*
         * 登録したサービス名になり、かつ「再起動が必要」でなくなったら、実際に新しいドライバーで動いている。
         * 機器が使用中（常駐アプリが開いたまま、など）で止められなかったときは、1 秒ごとに動かし直しを試す（最大 15 秒）。
         */
        for (t = 0; t < 150; t++) {
            if (FindDevice(kHardwareIds[i], service, ARRAYSIZE(service), &pending) && ModeOf(service) == mode && !pending) {
                done = TRUE;
                break;
            }
            if (pending && t % 10 == 0) {
                RestartDevice(kHardwareIds[i]);
                continue;
            }
            Sleep(100);
        }
        if (done) {
            Log(log, L"  %s になりました（%.1f 秒）", service, (double)(GetTickCount64() - start) / 1000.0);
            if (result == 0) result = 1;
        } else if (ModeOf(service) == mode) {
            Log(log, L"  %s を登録しましたが、機器が使用中のため動かし直せませんでした（あとで試し直します）", service);
            result = -1;
        } else {
            Log(log, L"  15 秒待っても切り替わりませんでした（今は %s）", service[0] ? service : L"(なし)");
            result = -1;
        }
    }
    return result;
}
