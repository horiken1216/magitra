/*
 * mtp_haptic_out.c - Actuator（USB MI_02）を HID のまま開いて出力レポートを書く
 */
#include "mtp_haptic_out.h"

#include <cfgmgr32.h>
#include <hidsdi.h>
#include <wchar.h>

/*
 * Actuator（MI_02）のデバイスパスに含まれる文字列（小文字で比較する）
 * - PID 0x0265（Lightning）: 実機で確認済み
 * - PID 0x0324（USB-C、第 4 世代）: 公開情報で MI_02・Usage 0xFF00/0x0D と報告されている。
 *   振動レポートの中身が同じかを含め、実機では未確認
 */
static const wchar_t *kActuatorPathParts[] = {
    L"vid_05ac&pid_0265&mi_02",
    L"vid_05ac&pid_0324&mi_02",
};

static HANDLE OpenActuator(void)
{
    GUID hidGuid;
    ULONG size = 0;
    WCHAR *list = NULL;
    WCHAR *p;
    HANDLE h = INVALID_HANDLE_VALUE;

    HidD_GetHidGuid(&hidGuid);
    if (CM_Get_Device_Interface_List_SizeW(&size, &hidGuid, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        size == 0) {
        return INVALID_HANDLE_VALUE;
    }
    list = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(WCHAR));
    if (list == NULL) {
        return INVALID_HANDLE_VALUE;
    }
    if (CM_Get_Device_Interface_ListW(&hidGuid, NULL, list, size, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS) {
        for (p = list; *p != L'\0'; p += wcslen(p) + 1) {
            WCHAR lower[512];
            size_t i, len = wcslen(p);
            if (len >= ARRAYSIZE(lower)) {
                continue;
            }
            for (i = 0; i <= len; i++) {
                lower[i] = (WCHAR)towlower(p[i]);
            }
            for (i = 0; i < ARRAYSIZE(kActuatorPathParts) && h == INVALID_HANDLE_VALUE; i++) {
                if (wcsstr(lower, kActuatorPathParts[i]) != NULL) {
                    h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                }
            }
            if (h != INVALID_HANDLE_VALUE) {
                break;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, list);
    return h;
}

void MtpHapticOutInit(MTP_HAPTIC_OUT *out)
{
    out->file = INVALID_HANDLE_VALUE;
    out->last_tick = 0;
}

void MtpHapticOutClose(MTP_HAPTIC_OUT *out)
{
    if (out->file != INVALID_HANDLE_VALUE) {
        CloseHandle(out->file);
        out->file = INVALID_HANDLE_VALUE;
    }
}

BOOL MtpHapticOutPlay(MTP_HAPTIC_OUT *out, const MTP_HAPTIC *haptic)
{
    uint8_t report[MTP_ACTUATOR_REPORT_LENGTH];
    ULONGLONG now = GetTickCount64();
    DWORD written = 0;
    int attempt;

    if (out->last_tick != 0 && now - out->last_tick < MTP_ACTUATOR_MIN_INTERVAL_MS) {
        return FALSE;
    }
    if (MtpBuildActuatorReport(haptic, report, sizeof(report), NULL) != MTP_OK) {
        return FALSE;
    }
    /* 抜き差しでハンドルが無効になっていたら 1 回だけ開き直す */
    for (attempt = 0; attempt < 2; attempt++) {
        if (out->file == INVALID_HANDLE_VALUE) {
            out->file = OpenActuator();
            if (out->file == INVALID_HANDLE_VALUE) {
                return FALSE;
            }
        }
        if (WriteFile(out->file, report, sizeof(report), &written, NULL) && written == sizeof(report)) {
            out->last_tick = now;
            return TRUE;
        }
        MtpHapticOutClose(out);
    }
    return FALSE;
}
