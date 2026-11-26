/*
 * device.h - Magic Trackpad の探索（sysfs）・hidraw の読み書き・カーネル側 input デバイスの占有
 */
#ifndef MAGITRA_DEVICE_H
#define MAGITRA_DEVICE_H

#include "mtp_core.h"

#define DEVICE_MAX_GRABS 8

typedef enum DEVICE_KIND {
    DEVICE_NONE = 0,
    DEVICE_USB = 1,
    DEVICE_BT = 2
} DEVICE_KIND;

typedef enum DEVICE_READ {
    DEVICE_READ_AGAIN = 0,   /* もう読むものがない */
    DEVICE_READ_FRAME = 1,   /* frame に入れた */
    DEVICE_READ_IGNORED = 2, /* マルチタッチ以外のレポート。続けて読んでよい */
    DEVICE_READ_LOST = -1    /* 切断された */
} DEVICE_READ;

typedef struct MAGITRA_DEVICE {
    DEVICE_KIND kind;
    MTP_TRANSPORT transport;
    int bustype, vendor, product;  /* 接続中のデバイスの HID ID（input の id と照合する） */
    int mt_fd;                     /* マルチタッチの hidraw。未接続は -1 */
    int haptic_fd;                 /* 振動の書き込み先。BT は mt_fd と同じ、USB は MI_02。なければ -1 */
    int grab_fd[DEVICE_MAX_GRABS]; /* EVIOCGRAB した /dev/input/eventN。なければ -1 */
    int grab_num[DEVICE_MAX_GRABS];/* その eventN の N */
    long long last_haptic_ms;
} MAGITRA_DEVICE;

void DeviceInit(MAGITRA_DEVICE *d);

/* 探して開き、マルチタッチを有効にする。成功なら 0、見つからない・開けなければ -1 */
int DeviceOpen(MAGITRA_DEVICE *d);

/* hidraw と占有をすべて閉じる（無効化レポートは送らない） */
void DeviceClose(MAGITRA_DEVICE *d);

DEVICE_READ DeviceRead(MAGITRA_DEVICE *d, MTP_FRAME *frame);

/* 接続中のデバイスの input を占有する（済みのものは飛ばす）。遅れて現れるノードのため定期的に呼ぶ */
void DeviceGrabScan(MAGITRA_DEVICE *d);

/* 占有を全部やめる（一時停止・切断・終了） */
void DeviceUngrab(MAGITRA_DEVICE *d);

/* 占有した input の溜まったイベントを捨てる。ノードが消えていたら閉じる */
void DeviceGrabDrain(MAGITRA_DEVICE *d, int index);

/* 0: 送った、-1: 振動先なし、-2: 間隔が短すぎる、-3: 書き込み失敗 */
int DeviceHaptic(MAGITRA_DEVICE *d, const MTP_HAPTIC *haptic);

#endif /* MAGITRA_DEVICE_H */
