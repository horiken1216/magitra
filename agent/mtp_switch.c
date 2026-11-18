/*
 * mtp_switch.c - トラックパッドのドライバーの切り替え（UpdateDriverForPlugAndPlayDevices、画面を出さない）
 */
#include "mtp_switch.h"

#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/*
 * 切り替える機器。PID は mtp_core.h と同じ（0x0324 は仮対応）。
 * MAGITRA 側の INF がドライバーストアにない機器は扱わない（Bluetooth はテスト署名モードで MtpBth を入れたときだけ）。
 */
typedef struct MTP_SWITCH_TARGET {
    const wchar_t *hardwareId;
    const wchar_t *enumerator;   /* SetupDiGetClassDevs で絞り込む列挙子 */
    const wchar_t *hidService;   /* Windows 標準のドライバーのサービス名 */
    const wchar_t *hidInf;       /* %windir%\INF にある、Windows 標準のドライバーの INF */
    const wchar_t *mtpService;   /* MAGITRA 側のサービス名 */
    const char *mtpInfMarker;    /* MAGITRA 側の INF を見分ける文字列（提供元 MAGITRA と一緒に探す） */
} MTP_SWITCH_TARGET;

static const MTP_SWITCH_TARGET kTargets[] = {
    { L"USB\\VID_05AC&PID_0265&MI_01", L"USB", L"HidUsb", L"input.inf", L"WINUSB", "MtpUsb.DeviceDesc" },
    { L"USB\\VID_05AC&PID_0324&MI_01", L"USB", L"HidUsb", L"input.inf", L"WINUSB", "MtpUsb.DeviceDesc" },
    { L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0265", L"BTHENUM", L"HidBth", L"hidbth.inf",
      L"MtpBth", "MtpBth.DeviceDesc" },
    { L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0324", L"BTHENUM", L"HidBth", L"hidbth.inf",
      L"MtpBth", "MtpBth.DeviceDesc" },
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
 * 機器 ID が一致する、つながっている機器を探す。見つかれば、登録されているサービス名を service に、
 * 「再起動が必要」か（新しいドライバーが登録されただけで、機器はまだ前のドライバーで動いている）を pending に入れる。
 * set と data は、呼び出し側が要るときだけ返す（返したら SetupDiDestroyDeviceInfoList で閉じる）。
 */
static BOOL FindDeviceEx(const MTP_SWITCH_TARGET *target, wchar_t *service, DWORD serviceLen, BOOL *pending,
    HDEVINFO *outSet, SP_DEVINFO_DATA *outData)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, target->enumerator, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
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
            if (_wcsicmp(p, target->hardwareId) == 0) {
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

static BOOL FindDevice(const MTP_SWITCH_TARGET *target, wchar_t *service, DWORD serviceLen, BOOL *pending)
{
    return FindDeviceEx(target, service, serviceLen, pending, NULL, NULL);
}

/*
 * 機器を止めて動かし直し、登録済みのドライバーで動かす（devcon restart と同じ）。
 * ほかのプログラムが機器を開いていると止められないので、FALSE が返る。
 */
static BOOL RestartDevice(const MTP_SWITCH_TARGET *target)
{
    HDEVINFO set;
    SP_DEVINFO_DATA data;
    SP_PROPCHANGE_PARAMS pc;
    SP_DEVINSTALL_PARAMS_W ip;
    wchar_t service[128];
    BOOL ok = FALSE;

    if (!FindDeviceEx(target, service, ARRAYSIZE(service), NULL, &set, &data)) {
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

static MTP_DRIVER_MODE ModeOf(const MTP_SWITCH_TARGET *target, const wchar_t *service)
{
    if (_wcsicmp(service, target->mtpService) == 0) return MTP_MODE_MAGITRA;
    if (_wcsicmp(service, target->hidService) == 0) return MTP_MODE_HID;
    return MTP_MODE_UNKNOWN;
}

/* %windir%\INF\oem*.inf のうち、MAGITRA の INF（marker と提供元 MAGITRA を含む）を探す。out は NULL 可 */
static BOOL FindMtpInf(const char *marker, wchar_t *out, DWORD outLen)
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
            if (strstr(buf, marker) != NULL && strstr(buf, "MAGITRA") != NULL) {
                if (out != NULL) {
                    wcscpy_s(out, outLen, path);
                }
                found = TRUE;
            }
        }
        CloseHandle(f);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/*
 * 切り替えの対象として扱う機器か。つながっていて、標準か MAGITRA のどちらかのドライバーで動いていて、
 * MAGITRA 側の INF がドライバーストアにある。mode と pending には今の状態を入れる。
 */
static BOOL IsManaged(const MTP_SWITCH_TARGET *target, MTP_DRIVER_MODE *mode, BOOL *pending)
{
    wchar_t service[128];
    if (!FindDevice(target, service, ARRAYSIZE(service), pending)) {
        return FALSE;
    }
    *mode = ModeOf(target, service);
    if (*mode == MTP_MODE_UNKNOWN) {
        return FALSE; /* 別のドライバーが当たっている。触らない */
    }
    return FindMtpInf(target->mtpInfMarker, NULL, 0);
}

MTP_DRIVER_MODE MtpSwitchCurrentModeEx(int *pending)
{
    size_t i;
    if (pending) *pending = 0;
    for (i = 0; i < ARRAYSIZE(kTargets); i++) {
        MTP_DRIVER_MODE mode;
        BOOL p = FALSE;
        if (IsManaged(&kTargets[i], &mode, &p)) {
            if (pending) *pending = p ? 1 : 0;
            return mode;
        }
    }
    return MTP_MODE_UNKNOWN;
}

MTP_DRIVER_MODE MtpSwitchCurrentMode(void)
{
    return MtpSwitchCurrentModeEx(NULL);
}

BOOL MtpSwitchIsSettled(MTP_DRIVER_MODE desired)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(kTargets); i++) {
        MTP_DRIVER_MODE mode;
        BOOL pending = FALSE;
        if (!IsManaged(&kTargets[i], &mode, &pending)) {
            continue;
        }
        /*
         * MAGITRA 側が登録済みで「再起動が必要」のときは、常駐アプリがもう開いている見込みが高い
         * （動かし直すと使用中で断られ続ける）。標準に戻すときだけ、動かし直しを待っている状態を食い違いとみなす。
         */
        if (mode != desired || (pending && desired == MTP_MODE_HID)) {
            return FALSE;
        }
    }
    return TRUE;
}

int MtpSwitchTo(MTP_DRIVER_MODE mode, MTP_SWITCH_LOG log)
{
    wchar_t windir[MAX_PATH];
    size_t i;
    int result = 0;

    if (mode != MTP_MODE_HID && mode != MTP_MODE_MAGITRA) {
        return -1;
    }
    if (GetWindowsDirectoryW(windir, MAX_PATH) == 0) {
        return -1;
    }

    for (i = 0; i < ARRAYSIZE(kTargets); i++) {
        const MTP_SWITCH_TARGET *target = &kTargets[i];
        const wchar_t *targetService = mode == MTP_MODE_MAGITRA ? target->mtpService : target->hidService;
        wchar_t inf[MAX_PATH];
        wchar_t service[128];
        MTP_DRIVER_MODE current;
        BOOL reboot = FALSE;
        BOOL pending = FALSE;
        BOOL done = FALSE;
        ULONGLONG start;
        int t;

        if (!IsManaged(target, &current, &pending)) {
            continue;
        }
        if (current == mode && !pending) {
            continue;
        }
        if (mode == MTP_MODE_MAGITRA) {
            if (!FindMtpInf(target->mtpInfMarker, inf, MAX_PATH)) {
                continue; /* IsManaged の後に取り除かれた */
            }
        } else {
            swprintf_s(inf, MAX_PATH, L"%s\\INF\\%s", windir, target->hidInf);
        }
        start = GetTickCount64();
        if (current == mode) {
            /* 前回の切り替えで登録だけ済んで、機器が止められなかった。動かし直しだけを試す */
            Log(log, L"%s: %s は登録済みですが、まだ前のドライバーで動いています。動かし直します", target->hardwareId, targetService);
        } else {
            Log(log, L"%s: %s から %s へ切り替えます（%s）", target->hardwareId,
                current == MTP_MODE_MAGITRA ? target->mtpService : target->hidService, targetService, inf);
            if (!UpdateDriverForPlugAndPlayDevicesW(NULL, target->hardwareId, inf, INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &reboot)) {
                Log(log, L"  切り替えに失敗しました（エラー 0x%08lX）", GetLastError());
                result = -1;
                continue;
            }
        }
        /*
         * 登録したサービス名になり、かつ「再起動が必要」でなくなったら、実際に新しいドライバーで動いている。
         * 機器が使用中（常駐アプリが開いたまま、など）で止められなかったときは、1 秒ごとに動かし直しを試す（最大 15 秒）。
         */
        service[0] = L'\0';
        for (t = 0; t < 150; t++) {
            if (FindDevice(target, service, ARRAYSIZE(service), &pending) && ModeOf(target, service) == mode && !pending) {
                done = TRUE;
                break;
            }
            if (pending && t % 10 == 0) {
                RestartDevice(target);
                continue;
            }
            Sleep(100);
        }
        if (done) {
            Log(log, L"  %s になりました（%.1f 秒）", service, (double)(GetTickCount64() - start) / 1000.0);
            if (result == 0) result = 1;
        } else if (ModeOf(target, service) == mode) {
            Log(log, L"  %s を登録しましたが、機器が使用中のため動かし直せませんでした（あとで試し直します）", service);
            result = -1;
        } else {
            Log(log, L"  15 秒待っても切り替わりませんでした（今は %s）", service[0] ? service : L"(なし)");
            result = -1;
        }
    }
    return result;
}
