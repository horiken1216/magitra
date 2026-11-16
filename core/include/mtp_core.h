/*
 * mtp_core.h - Magic Trackpad 2 の接続方式に依存しない処理（有効化レポート、入力レポート解析）
 *
 * カーネルモード（KMDF ドライバー）とユーザーモード（テスト・ツール）の両方から使う。
 * そのため動的メモリ確保と CRT 関数を使わない。
 *
 * レポート形式は Linux hid-magicmouse と imbushuo/mac-precision-touchpad の記述から確認した事実に基づく。
 * コードはどちらからも流用していない。
 */
#ifndef MTP_CORE_H
#define MTP_CORE_H

#include <stddef.h>
#if defined(_KERNEL_MODE)
/* カーネルの CRT には stdint.h がないため、必要な型だけ定義する */
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_VID_APPLE_BT   0x004C
#define MTP_VID_APPLE_USB  0x05AC
#define MTP_PID_MT2        0x0265

/* 入力レポート ID（マルチタッチモード時） */
#define MTP_REPORT_ID_BT   0x31
#define MTP_REPORT_ID_USB  0x02

/* 1 フレームに入る指の最大数。デバイスは最大 16 本まで報告しうる */
#define MTP_MAX_CONTACTS   16

/* 生座標の範囲（MT2）。X は右が正、Y は上が正に揃えた後の値 */
#define MTP_X_MIN  (-3678)
#define MTP_X_MAX  3934
#define MTP_Y_MIN  (-2479)
#define MTP_Y_MAX  2586

typedef enum MTP_TRANSPORT {
    MTP_TRANSPORT_BT = 0,
    MTP_TRANSPORT_USB = 1
} MTP_TRANSPORT;

typedef enum MTP_STATUS {
    MTP_OK = 0,
    MTP_ERR_ARG = -1,        /* NULL 引数・未知の接続方式 */
    MTP_ERR_REPORT_ID = -2,  /* マルチタッチ以外のレポート（呼び出し側で無視してよい） */
    MTP_ERR_LENGTH = -3      /* ヘッダー長未満、または指データが 9 バイト単位で割り切れない */
} MTP_STATUS;

typedef struct MTP_CONTACT {
    int16_t x;            /* MTP_X_MIN..MTP_X_MAX */
    int16_t y;            /* MTP_Y_MIN..MTP_Y_MAX（上が正） */
    uint8_t touch_major;  /* 接触楕円の長軸 */
    uint8_t touch_minor;  /* 接触楕円の短軸 */
    uint8_t size;         /* 接触面積 */
    uint8_t pressure;     /* 圧力（Force Touch の判定に使う） */
    uint8_t id;           /* 0..15。指が離れるまで同じ値 */
    int8_t  orientation;  /* -4..3 */
    uint8_t state;        /* 生の状態ビット（byte3 の上位 2 ビット）。実機で 1 = 触れ始め、2 = 接触中、0/3 = 離れる途中を確認 */
} MTP_CONTACT;

typedef struct MTP_FRAME {
    uint8_t     button;         /* 物理クリック（1 = 押下） */
    uint8_t     contact_count;  /* contacts の有効数 */
    uint8_t     truncated;      /* MTP_MAX_CONTACTS を超えて切り捨てたら 1 */
    MTP_CONTACT contacts[MTP_MAX_CONTACTS];
} MTP_FRAME;

/*
 * マルチタッチモードへ切り替えるフィーチャーレポート（先頭バイトが Report ID）。
 * 接続直後と、デバイスが省電力から復帰した後に送る。
 */
MTP_STATUS MtpGetEnableReport(MTP_TRANSPORT transport, const uint8_t **report, size_t *length);

/* 入力レポートの Report ID */
uint8_t MtpInputReportId(MTP_TRANSPORT transport);

/*
 * 入力レポート（先頭バイトが Report ID）を解析する。
 * MTP_OK 以外を返したとき frame の内容は未定義。
 */
MTP_STATUS MtpParseReport(MTP_TRANSPORT transport, const uint8_t *report, size_t length, MTP_FRAME *frame);

#ifdef __cplusplus
}
#endif

#endif /* MTP_CORE_H */
