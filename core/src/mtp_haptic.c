/*
 * mtp_haptic.c - Magic Trackpad (USB) の Actuator 出力レポートの組み立て
 *
 * 14 バイトのテンプレート（imbushuo の gist、USB PID 0x0265 で動作報告あり）:
 *   idx:  0  1  2  3  4  5  6  7  8  9 10 11 12 13
 *         53 01 s0 6C 02 s1 21 2B 06 01 s2 16 41 13
 * s0/s1/s2 以外は gist の値に固定し、残り（idx 14..63）は 0 で埋める。
 */
#include "mtp_core.h"

#define TEMPLATE_LENGTH 14
#define IDX_STRENGTH    2
#define IDX_SHARPNESS   5
#define IDX_DEPTH       10

static const uint8_t kTemplate[TEMPLATE_LENGTH] = {
    0x53, 0x01, 0x15, 0x6C, 0x02, 0x00, 0x21, 0x2B, 0x06, 0x01, 0x00, 0x16, 0x41, 0x13
};

static uint8_t Clamp(uint8_t value, uint8_t max, int *clamped)
{
    if (value > max) {
        *clamped = 1;
        return max;
    }
    return value;
}

MTP_STATUS MtpBuildActuatorReport(const MTP_HAPTIC *haptic, uint8_t *buffer, size_t length, int *clamped)
{
    size_t i;
    int wasClamped = 0;

    if (haptic == NULL || buffer == NULL || length < MTP_ACTUATOR_REPORT_LENGTH) {
        return MTP_ERR_ARG;
    }
    for (i = 0; i < MTP_ACTUATOR_REPORT_LENGTH; i++) {
        buffer[i] = i < TEMPLATE_LENGTH ? kTemplate[i] : 0;
    }
    buffer[IDX_STRENGTH] = Clamp(haptic->strength, MTP_ACTUATOR_STRENGTH_MAX, &wasClamped);
    buffer[IDX_SHARPNESS] = Clamp(haptic->sharpness, MTP_ACTUATOR_SHARPNESS_MAX, &wasClamped);
    buffer[IDX_DEPTH] = Clamp(haptic->depth, MTP_ACTUATOR_DEPTH_MAX, &wasClamped);

    if (clamped != NULL) {
        *clamped = wasClamped;
    }
    return MTP_OK;
}

/*
 * Bluetooth 版（MagicTrackpad2ForWindows PR #63）:
 *   idx:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14
 *         F2 53 01 s0 78 02 s1 24 30 06 01 s2 18 48 12
 */
static const uint8_t kTemplateBt[MTP_ACTUATOR_BT_REPORT_LENGTH] = {
    0xF2, 0x53, 0x01, 0x15, 0x78, 0x02, 0x00, 0x24, 0x30, 0x06, 0x01, 0x00, 0x18, 0x48, 0x12
};

MTP_STATUS MtpBuildActuatorReportBt(const MTP_HAPTIC *haptic, uint8_t *buffer, size_t length, int *clamped)
{
    size_t i;
    int wasClamped = 0;

    if (haptic == NULL || buffer == NULL || length < MTP_ACTUATOR_BT_REPORT_LENGTH) {
        return MTP_ERR_ARG;
    }
    for (i = 0; i < MTP_ACTUATOR_BT_REPORT_LENGTH; i++) {
        buffer[i] = kTemplateBt[i];
    }
    /* USB 版より Report ID の 1 バイトぶん後ろにずれる */
    buffer[IDX_STRENGTH + 1] = Clamp(haptic->strength, MTP_ACTUATOR_STRENGTH_MAX, &wasClamped);
    buffer[IDX_SHARPNESS + 1] = Clamp(haptic->sharpness, MTP_ACTUATOR_SHARPNESS_MAX, &wasClamped);
    buffer[IDX_DEPTH + 1] = Clamp(haptic->depth, MTP_ACTUATOR_DEPTH_MAX, &wasClamped);

    if (clamped != NULL) {
        *clamped = wasClamped;
    }
    return MTP_OK;
}
