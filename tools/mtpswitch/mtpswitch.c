/*
 * mtpswitch.c - Magic Trackpad の MI_01 のドライバーを、Windows 標準の HID と WinUSB（MAGITRA）の間で手動で切り替える道具
 *
 * 普段はサービス（mtpswitchsvc.exe）がロック・ログオンに合わせて自動で切り替える。これは試験と、困ったときの手動の切り替え用。
 * 切り替えの処理は agent/mtp_switch.c（サービスと同じ）。
 *
 * 使い方（hid と winusb は管理者権限で）:
 *   mtpswitch status   今のドライバーを表示する
 *   mtpswitch hid      Windows 標準の HID に切り替える
 *   mtpswitch winusb   WinUSB（MAGITRA）に戻す
 */
#include <windows.h>
#include <locale.h>
#include <stdio.h>
#include <wchar.h>

#include "mtp_switch.h"

static void PrintLog(const wchar_t *message)
{
    wprintf(L"%s\n", message);
}

static BOOL IsElevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION e;
    DWORD len = 0;
    BOOL elevated = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &len)) {
            elevated = e.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    return elevated;
}

static const wchar_t *ModeName(MTP_DRIVER_MODE m)
{
    return m == MTP_MODE_HID ? L"HidUsb（Windows 標準の HID）" : m == MTP_MODE_WINUSB ? L"WINUSB（MTP）" : L"不明（未接続か、別のドライバー）";
}

int wmain(int argc, wchar_t **argv)
{
    MTP_DRIVER_MODE target;
    int r;

    SetConsoleOutputCP(CP_UTF8);
    _wsetlocale(LC_ALL, L".UTF8");

    if (argc < 2 || _wcsicmp(argv[1], L"status") == 0) {
        int pending = 0;
        MTP_DRIVER_MODE m = MtpSwitchCurrentModeEx(&pending);
        wprintf(L"今のドライバー: %s\n", ModeName(m));
        if (pending) {
            wprintf(L"  ただし登録されただけで、機器はまだ前のドライバーで動いています（再起動が必要）。\n");
        }
        return m == MTP_MODE_UNKNOWN ? 1 : 0;
    }
    if (_wcsicmp(argv[1], L"hid") == 0) {
        target = MTP_MODE_HID;
    } else if (_wcsicmp(argv[1], L"winusb") == 0) {
        target = MTP_MODE_WINUSB;
    } else {
        wprintf(L"使い方: mtpswitch status | hid | winusb\n");
        return 2;
    }
    if (!IsElevated()) {
        wprintf(L"管理者権限で実行してください。\n");
        return 2;
    }
    r = MtpSwitchTo(target, PrintLog);
    if (r == 0) {
        wprintf(L"切り替える必要はありませんでした（今は %s）。\n", ModeName(MtpSwitchCurrentMode()));
    }
    return r < 0 ? 1 : 0;
}
