/*
 * mtp_report.c - Magic Trackpad の有効化レポートと入力レポート解析
 *
 * 入力レポートの構造:
 *   [0]          Report ID（BT: 0x31, USB: 0x02）
 *   [1]          ボタン（bit0 = 物理クリック）
 *   [2..H-1]     未使用ヘッダー（H = BT: 4, USB: 12）
 *   [H + 9*i]    指 i のデータ 9 バイト
 *
 * 指データ 9 バイト:
 *   b0..b3  32bit LE ワード w
 *           X     = 符号付き 13bit (w bits 0..12)
 *           Y     = -符号付き 13bit (w bits 13..25)   ※反転後は下（手前）が正。画面座標と同じ向き
 *           state = b3 の上位 2bit
 *   b4      touch_major
 *   b5      touch_minor
 *   b6      size
 *   b7      pressure
 *   b8      bit0..3 = contact id, bit5..7 = orientation + 4
 */
#include "mtp_core.h"

#define MTP_FINGER_SIZE      9
#define MTP_HEADER_SIZE_BT   4
#define MTP_HEADER_SIZE_USB  12
#define MTP_BUTTON_OFFSET    1

static const uint8_t kEnableBt[]  = { 0xF1, 0x02, 0x01 };
static const uint8_t kEnableUsb[] = { 0x02, 0x01 };
/* マウスのモード（入力 0x02）へ戻す。Bluetooth で実機確認済み（2026-11-18）。USB は未確認なので持たない */
static const uint8_t kDisableBt[] = { 0xF1, 0x02, 0x00 };

MTP_STATUS MtpGetDisableReport(MTP_TRANSPORT transport, const uint8_t **report, size_t *length)
{
    if (report == NULL || length == NULL || transport != MTP_TRANSPORT_BT) {
        return MTP_ERR_ARG;
    }
    *report = kDisableBt;
    *length = sizeof(kDisableBt);
    return MTP_OK;
}

static int16_t SignExtend13(uint32_t v)
{
    v &= 0x1FFFu;
    return (int16_t)((v & 0x1000u) ? (int32_t)v - 0x2000 : (int32_t)v);
}

MTP_STATUS MtpGetEnableReport(MTP_TRANSPORT transport, const uint8_t **report, size_t *length)
{
    if (report == NULL || length == NULL) {
        return MTP_ERR_ARG;
    }
    switch (transport) {
    case MTP_TRANSPORT_BT:
        *report = kEnableBt;
        *length = sizeof(kEnableBt);
        return MTP_OK;
    case MTP_TRANSPORT_USB:
        *report = kEnableUsb;
        *length = sizeof(kEnableUsb);
        return MTP_OK;
    default:
        return MTP_ERR_ARG;
    }
}

int MtpIsSupportedProduct(uint16_t product_id)
{
    return product_id == MTP_PID_MT2 || product_id == MTP_PID_MT_USBC;
}

uint8_t MtpInputReportId(MTP_TRANSPORT transport)
{
    return transport == MTP_TRANSPORT_USB ? MTP_REPORT_ID_USB : MTP_REPORT_ID_BT;
}

MTP_STATUS MtpParseReport(MTP_TRANSPORT transport, const uint8_t *report, size_t length, MTP_FRAME *frame)
{
    size_t header;
    size_t count;
    size_t i;

    if (report == NULL || frame == NULL) {
        return MTP_ERR_ARG;
    }
    if (transport == MTP_TRANSPORT_BT) {
        header = MTP_HEADER_SIZE_BT;
    } else if (transport == MTP_TRANSPORT_USB) {
        header = MTP_HEADER_SIZE_USB;
    } else {
        return MTP_ERR_ARG;
    }

    if (length < 1 || report[0] != MtpInputReportId(transport)) {
        return MTP_ERR_REPORT_ID;
    }
    if (length < header || (length - header) % MTP_FINGER_SIZE != 0) {
        return MTP_ERR_LENGTH;
    }

    count = (length - header) / MTP_FINGER_SIZE;
    frame->truncated = 0;
    if (count > MTP_MAX_CONTACTS) {
        count = MTP_MAX_CONTACTS;
        frame->truncated = 1;
    }
    frame->button = (uint8_t)(report[MTP_BUTTON_OFFSET] & 0x01u);
    frame->contact_count = (uint8_t)count;

    for (i = 0; i < count; i++) {
        const uint8_t *f = report + header + i * MTP_FINGER_SIZE;
        MTP_CONTACT *c = &frame->contacts[i];
        uint32_t w = (uint32_t)f[0] | ((uint32_t)f[1] << 8) | ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);

        c->x = SignExtend13(w);
        c->y = (int16_t)-SignExtend13(w >> 13);
        c->state = (uint8_t)(f[3] >> 6);
        c->touch_major = f[4];
        c->touch_minor = f[5];
        c->size = f[6];
        c->pressure = f[7];
        c->id = (uint8_t)(f[8] & 0x0Fu);
        c->orientation = (int8_t)((int)(f[8] >> 5) - 4);
    }
    return MTP_OK;
}
