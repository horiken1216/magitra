/*
 * test_report.c - mtp_report.c の単体テスト
 *
 * 前半のバイト列は解析コードを使わず手計算で作った（計算過程はコメント参照）。
 * test_usb_real_frames は USB 実機（WinUSB 経由）で記録したフレームの回帰テスト。
 */
#include <stdio.h>
#include <string.h>

#include "mtp_core.h"

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/*
 * 指 A: x=-3678, y=+2586, state=2, major=0x40, minor=0x30, size=0x20, pressure=0x55, id=3, orientation=+1
 *   x: 13bit 2の補数  8192-3678 = 4514 = 0x11A2
 *   y: デバイス値は -2586 → 8192-2586 = 5606 = 0x15E6
 *   w = 0x11A2 | 0x15E6<<13 = 0x02BCD1A2、state=2 → b3 |= 0x80 → bytes A2 D1 BC 82
 *   b8 = id | (orientation+4)<<5 = 0x03 | 0xA0 = 0xA3
 */
static const uint8_t kFingerA[9] = { 0xA2, 0xD1, 0xBC, 0x82, 0x40, 0x30, 0x20, 0x55, 0xA3 };

/*
 * 指 B: x=+3934, y=-2479, state=0, major..pressure=0x10..0x13, id=15, orientation=-4
 *   x = 0x0F5E、y のデバイス値 +2479 = 0x09AF
 *   w = 0x0F5E | 0x09AF<<13 = 0x0135EF5E → bytes 5E EF 35 01
 *   b8 = 0x0F | 0<<5 = 0x0F
 */
static const uint8_t kFingerB[9] = { 0x5E, 0xEF, 0x35, 0x01, 0x10, 0x11, 0x12, 0x13, 0x0F };

static void test_bt_two_fingers(void)
{
    uint8_t r[4 + 18] = { 0x31, 0x01, 0x00, 0x00 };
    MTP_FRAME f;
    memcpy(r + 4, kFingerA, 9);
    memcpy(r + 13, kFingerB, 9);

    CHECK(MtpParseReport(MTP_TRANSPORT_BT, r, sizeof(r), &f) == MTP_OK);
    CHECK(f.button == 1);
    CHECK(f.contact_count == 2);
    CHECK(f.truncated == 0);

    CHECK(f.contacts[0].x == -3678);
    CHECK(f.contacts[0].y == 2586);
    CHECK(f.contacts[0].state == 2);
    CHECK(f.contacts[0].touch_major == 0x40);
    CHECK(f.contacts[0].touch_minor == 0x30);
    CHECK(f.contacts[0].size == 0x20);
    CHECK(f.contacts[0].pressure == 0x55);
    CHECK(f.contacts[0].id == 3);
    CHECK(f.contacts[0].orientation == 1);

    CHECK(f.contacts[1].x == 3934);
    CHECK(f.contacts[1].y == -2479);
    CHECK(f.contacts[1].state == 0);
    CHECK(f.contacts[1].pressure == 0x13);
    CHECK(f.contacts[1].id == 15);
    CHECK(f.contacts[1].orientation == -4);
}

static void test_usb_one_finger(void)
{
    uint8_t r[12 + 9] = { 0x02, 0x00 };
    MTP_FRAME f;
    memcpy(r + 12, kFingerB, 9);

    CHECK(MtpParseReport(MTP_TRANSPORT_USB, r, sizeof(r), &f) == MTP_OK);
    CHECK(f.button == 0);
    CHECK(f.contact_count == 1);
    CHECK(f.contacts[0].x == 3934);
    CHECK(f.contacts[0].y == -2479);

    /* 同じバイト列を BT として解析すると ID 不一致 */
    CHECK(MtpParseReport(MTP_TRANSPORT_BT, r, sizeof(r), &f) == MTP_ERR_REPORT_ID);
}

/*
 * 実機（USB、WinUSB 経由、マルチタッチモード）で記録したフレーム。2026-11-16 の build/usb_frames.txt より。
 * 期待値は記録時の解析結果で、1 本目の X/Y は手計算でも確認した（w=0xB00B5BFC → x=-1028, y=-90）。
 */
static void test_usb_real_frames(void)
{
    static const uint8_t twoFingers[30] = {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x31, 0x40, 0x13, 0x05,
        0xFC, 0x5B, 0x0B, 0xB0, 0x6F, 0x82, 0x07, 0x00, 0x86,
        0x3B, 0xC0, 0xF3, 0xE3, 0x00, 0x00, 0x00, 0x00, 0x87
    };
    static const uint8_t clicked[21] = {
        0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x31, 0x01, 0xCF, 0x09,
        0x89, 0x19, 0x3B, 0x90, 0x48, 0x5D, 0x16, 0x49, 0x89
    };
    MTP_FRAME f;

    CHECK(MtpParseReport(MTP_TRANSPORT_USB, twoFingers, sizeof(twoFingers), &f) == MTP_OK);
    CHECK(f.button == 0);
    CHECK(f.contact_count == 2);
    CHECK(f.contacts[0].id == 6 && f.contacts[0].x == -1028 && f.contacts[0].y == -90);
    CHECK(f.contacts[0].touch_major == 111 && f.contacts[0].touch_minor == 130 && f.contacts[0].size == 7);
    CHECK(f.contacts[0].pressure == 0 && f.contacts[0].state == 2 && f.contacts[0].orientation == 0);
    /* 離れる直前の指: 接触サイズ・圧力が 0 で state=3 */
    CHECK(f.contacts[1].id == 7 && f.contacts[1].x == 59 && f.contacts[1].y == 98);
    CHECK(f.contacts[1].touch_major == 0 && f.contacts[1].pressure == 0 && f.contacts[1].state == 3);

    CHECK(MtpParseReport(MTP_TRANSPORT_USB, clicked, sizeof(clicked), &f) == MTP_OK);
    CHECK(f.button == 1);
    CHECK(f.contact_count == 1);
    CHECK(f.contacts[0].id == 9 && f.contacts[0].x == -1655 && f.contacts[0].y == -472);
    CHECK(f.contacts[0].pressure == 73 && f.contacts[0].size == 22);

    /* 既定モードのマウス用レポート（同じ ID 0x02 で 8 バイト）はフレームとして扱わない */
    {
        static const uint8_t mouse[8] = { 0x02, 0x00, 0xF6, 0xF6, 0x00, 0x00, 0x00, 0x01 };
        CHECK(MtpParseReport(MTP_TRANSPORT_USB, mouse, sizeof(mouse), &f) == MTP_ERR_LENGTH);
    }
}

static void test_no_fingers(void)
{
    const uint8_t bt[4] = { 0x31, 0x00, 0x00, 0x00 };
    MTP_FRAME f;
    CHECK(MtpParseReport(MTP_TRANSPORT_BT, bt, sizeof(bt), &f) == MTP_OK);
    CHECK(f.contact_count == 0);
    CHECK(f.button == 0);
}

static void test_bad_input(void)
{
    const uint8_t short_bt[3] = { 0x31, 0x00, 0x00 };
    const uint8_t ragged_bt[5] = { 0x31, 0x00, 0x00, 0x00, 0xFF };
    const uint8_t battery[3] = { 0x90, 0x00, 0x64 };
    MTP_FRAME f;

    CHECK(MtpParseReport(MTP_TRANSPORT_BT, short_bt, sizeof(short_bt), &f) == MTP_ERR_LENGTH);
    CHECK(MtpParseReport(MTP_TRANSPORT_BT, ragged_bt, sizeof(ragged_bt), &f) == MTP_ERR_LENGTH);
    CHECK(MtpParseReport(MTP_TRANSPORT_BT, battery, sizeof(battery), &f) == MTP_ERR_REPORT_ID);
    CHECK(MtpParseReport(MTP_TRANSPORT_BT, short_bt, 0, &f) == MTP_ERR_REPORT_ID);
    CHECK(MtpParseReport(MTP_TRANSPORT_BT, NULL, 4, &f) == MTP_ERR_ARG);
    CHECK(MtpParseReport(MTP_TRANSPORT_BT, short_bt, 3, NULL) == MTP_ERR_ARG);
    CHECK(MtpParseReport((MTP_TRANSPORT)7, short_bt, 3, &f) == MTP_ERR_ARG);
}

static void test_truncation(void)
{
    uint8_t r[4 + 9 * 17] = { 0x31 };
    MTP_FRAME f;
    int i;
    for (i = 0; i < 17; i++) {
        memcpy(r + 4 + 9 * i, kFingerA, 9);
    }
    CHECK(MtpParseReport(MTP_TRANSPORT_BT, r, sizeof(r), &f) == MTP_OK);
    CHECK(f.contact_count == MTP_MAX_CONTACTS);
    CHECK(f.truncated == 1);
}

static void test_enable_reports(void)
{
    const uint8_t *p = NULL;
    size_t n = 0;

    CHECK(MtpGetEnableReport(MTP_TRANSPORT_BT, &p, &n) == MTP_OK);
    CHECK(n == 3 && p[0] == 0xF1 && p[1] == 0x02 && p[2] == 0x01);

    CHECK(MtpGetEnableReport(MTP_TRANSPORT_USB, &p, &n) == MTP_OK);
    CHECK(n == 2 && p[0] == 0x02 && p[1] == 0x01);

    CHECK(MtpGetEnableReport(MTP_TRANSPORT_BT, NULL, &n) == MTP_ERR_ARG);
    CHECK(MtpInputReportId(MTP_TRANSPORT_BT) == 0x31);
    CHECK(MtpInputReportId(MTP_TRANSPORT_USB) == 0x02);
}

int main(void)
{
    test_bt_two_fingers();
    test_usb_one_finger();
    test_usb_real_frames();
    test_no_fingers();
    test_bad_input();
    test_truncation();
    test_enable_reports();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
