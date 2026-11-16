/*
 * mtp_device.h - WinUSB 経由の Magic Trackpad (USB, MI_01) の接続と読み取り
 *
 * 前提: drivers/MtpUsb/MtpUsb.inf で MI_01 に WinUSB を当ててあること（docs/INSTALL-USB.md）。
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
    HANDLE file;
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR pipe;
    UCHAR interface_number;
} MTP_DEVICE;

typedef enum MTP_DEVICE_STATUS {
    MTP_DEVICE_OK = 0,
    MTP_DEVICE_NOT_FOUND = 1,    /* WinUSB のインターフェイスがない（未導入・未接続） */
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

#ifdef __cplusplus
}
#endif

#endif /* MTP_DEVICE_H */
