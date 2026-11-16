/*
 * mtp_haptic_out.h - USB 接続の Actuator（MI_02、標準 HID のまま）に振動レポートを書く
 *
 * レポートの中身は core の MtpBuildActuatorReport が作る（上限付き）。
 * 速すぎる書き込みは拒否されるという報告があるため、MTP_ACTUATOR_MIN_INTERVAL_MS より短い間隔の要求は捨てる。
 */
#ifndef MTP_HAPTIC_OUT_H
#define MTP_HAPTIC_OUT_H

#include <windows.h>

#include "mtp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MTP_HAPTIC_OUT {
    HANDLE file;           /* MI_02 の HID。開けていなければ INVALID_HANDLE_VALUE */
    ULONGLONG last_tick;   /* 最後に書いた時刻（GetTickCount64） */
} MTP_HAPTIC_OUT;

/* Force Touch の手応えの既定値は mtp_settings.c（MtpSettingsDefaults）にある */

void MtpHapticOutInit(MTP_HAPTIC_OUT *out);
void MtpHapticOutClose(MTP_HAPTIC_OUT *out);

/* 振動させる。必要なら開き直す。間隔が短すぎて捨てた場合も FALSE */
BOOL MtpHapticOutPlay(MTP_HAPTIC_OUT *out, const MTP_HAPTIC *haptic);

#ifdef __cplusplus
}
#endif

#endif /* MTP_HAPTIC_OUT_H */
