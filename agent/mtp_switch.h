/*
 * mtp_switch.h - Magic Trackpad の MI_01 のドライバーを、Windows 標準の HID と WinUSB（MTP）の間で切り替える
 *
 * 試験道具（tools/mtpswitch）とサービス（service/mtpswitchsvc.c）が使う。管理者権限（または SYSTEM）が必要。
 * 実機（2026-11-16）: 機器を誰も開いていなければ、どちらの向きも約 8 秒。
 * 常駐アプリが WinUSB のハンドルを開いたままだと、PnP は機器を止められない。新しいドライバーは登録されるが
 * 「再起動が必要」（DN_NEED_RESTART）のまま、機器は前のドライバーで動き続ける。サービス名だけでは見分けられないので、
 * 「再起動が必要」でないことも確かめ、そうなら機器の動かし直しを試し続ける。
 */
#ifndef MTP_SWITCH_H
#define MTP_SWITCH_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MTP_DRIVER_MODE {
    MTP_MODE_UNKNOWN = 0,
    MTP_MODE_HID = 1,     /* Windows 標準の HidUsb（ログオン画面・ロック画面でも普通のマウスとして動く） */
    MTP_MODE_WINUSB = 2   /* WinUSB（MTP の常駐アプリがフル機能で使う） */
} MTP_DRIVER_MODE;

typedef void (*MTP_SWITCH_LOG)(const wchar_t *message);

/* つながっている Magic Trackpad の今のモード。つながっていなければ MTP_MODE_UNKNOWN */
MTP_DRIVER_MODE MtpSwitchCurrentMode(void);

/* 上と同じ。pending には、登録済みのドライバーにまだ切り替わっていない（再起動が必要）なら 1 を入れる */
MTP_DRIVER_MODE MtpSwitchCurrentModeEx(int *pending);

/*
 * mode に切り替える。すでにそのモードなら何もしない。log は NULL 可。
 * 戻り値: 1 = 切り替えた、0 = 切り替える必要がなかった・機器がない、-1 = 失敗
 */
int MtpSwitchTo(MTP_DRIVER_MODE mode, MTP_SWITCH_LOG log);

#ifdef __cplusplus
}
#endif

#endif /* MTP_SWITCH_H */
