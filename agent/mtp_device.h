/*
 * mtp_device.h - Magic Trackpad の接続と読み取り
 *
 * - USB: drivers/MtpUsb/MtpUsb.inf で MI_01 に WinUSB を当ててあること（docs/INSTALL.md）
 * - Bluetooth（上級者向け、テスト署名モード）: drivers/MtpBth を入れてあること。ドライバーが L2CAP を張り、
 *   マルチタッチへの切り替えも済ませているので、ここでは生のレポートを ReadFile で読むだけ
 * 両方あれば USB を使う。
 */
#ifndef MTP_DEVICE_H
#define MTP_DEVICE_H

#include <windows.h>
#include <winusb.h>

#include "mtp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_DEVICE_READ_TIMEOUT_MS 200

typedef struct MTP_DEVICE {
    MTP_TRANSPORT transport;
    HANDLE file;                 /* USB: WinUSB のハンドル。Bluetooth: MtpBth のハンドル（どちらも OVERLAPPED） */
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR pipe;
    UCHAR interface_number;
    HANDLE read_event;           /* Bluetooth の読み取りで使う */
} MTP_DEVICE;

typedef enum MTP_DEVICE_STATUS {
    MTP_DEVICE_OK = 0,
    MTP_DEVICE_NOT_FOUND = 1,    /* WinUSB のインターフェイスも MtpBth もない（未導入・未接続） */
    MTP_DEVICE_OPEN_FAILED = 2,  /* 見つかったが開けない・初期化できない */
    MTP_DEVICE_ENABLE_FAILED = 3 /* パイプ取得かマルチタッチ切り替えに失敗 */
} MTP_DEVICE_STATUS;

/* 開いてマルチタッチモードへ切り替える。失敗時は *win32_error に GetLastError() を入れる */
MTP_DEVICE_STATUS MtpDeviceOpen(MTP_DEVICE *dev, DWORD *win32_error);

void MtpDeviceClose(MTP_DEVICE *dev);

typedef enum MTP_READ_RESULT {
    MTP_READ_FRAME = 0,   /* frame にマルチタッチのフレームが入った */
    MTP_READ_OTHER = 1,   /* マルチタッチ以外のレポート（無視してよい） */
    MTP_READ_TIMEOUT = 2, /* MTP_DEVICE_READ_TIMEOUT_MS の間、入力がなかった */
    MTP_READ_ERROR = 3    /* 切断などの失敗。閉じて開き直す */
} MTP_READ_RESULT;

MTP_READ_RESULT MtpDeviceRead(MTP_DEVICE *dev, MTP_FRAME *frame, UCHAR *raw, ULONG raw_capacity, ULONG *raw_length,
    DWORD *win32_error);

/* 別のスレッドから、進行中の読み取りを中断させる（終了時用） */
void MtpDeviceAbortRead(MTP_DEVICE *dev);

#ifdef __cplusplus
}
#endif

#endif /* MTP_DEVICE_H */
