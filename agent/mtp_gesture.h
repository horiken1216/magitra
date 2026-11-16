/*
 * mtp_gesture.h - マルチタッチのフレーム列から入力イベント（ポインター移動・ボタン・スクロール）を作る
 *
 * OS に依存しない純粋なロジック。フレームを 1 つ渡すと、そのフレームで発生したイベントを返す。
 * 実機なしで単体テストできるよう、入力注入（SendInput）は呼び出し側で行う。
 *
 * 座標系: MTP_CONTACT の x は右が正、y は下（手前）が正（mtp_core.h）。出力の dy も画面座標（下が正）なので符号はそのまま。
 */
#ifndef MTP_GESTURE_H
#define MTP_GESTURE_H

#include "mtp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MTP_GESTURE_CONFIG {
    float pointer_gain;      /* トラックパッド単位 → 画素の基本倍率 */
    float pointer_accel;     /* 速く動かしたときの追加倍率（0 で加速なし） */
    float scroll_gain;       /* トラックパッド単位 → ホイール単位（120 = 1 ノッチ） */
    int   natural_scroll;    /* 1: 指と同じ向きに内容が動く（Mac の既定） */
    uint8_t min_touch_major; /* これ未満の接触は指として数えない */
} MTP_GESTURE_CONFIG;

void MtpGestureDefaultConfig(MTP_GESTURE_CONFIG *config);

typedef enum MTP_EVENT_TYPE {
    MTP_EVENT_MOVE = 1,    /* dx, dy（画素、画面座標） */
    MTP_EVENT_BUTTON = 2,  /* button, down */
    MTP_EVENT_SCROLL = 3   /* dx = 横ホイール, dy = 縦ホイール（Windows の符号: 縦は正で上、横は正で右） */
} MTP_EVENT_TYPE;

typedef enum MTP_BUTTON {
    MTP_BUTTON_LEFT = 0,
    MTP_BUTTON_RIGHT = 1
} MTP_BUTTON;

typedef struct MTP_EVENT {
    MTP_EVENT_TYPE type;
    int dx;
    int dy;
    MTP_BUTTON button;
    int down;
} MTP_EVENT;

#define MTP_GESTURE_MAX_EVENTS 4

typedef struct MTP_GESTURE {
    MTP_GESTURE_CONFIG config;

    /* 前フレームで指として数えた接触（id ごと） */
    int     prev_valid[MTP_MAX_CONTACTS];
    int16_t prev_x[MTP_MAX_CONTACTS];
    int16_t prev_y[MTP_MAX_CONTACTS];
    int     prev_count;
    uint16_t prev_id_mask;

    int        button_down;
    MTP_BUTTON button_held;

    /* 1 未満の端数を次のフレームへ持ち越す */
    float move_rem_x, move_rem_y;
    float scroll_rem_x, scroll_rem_y;
} MTP_GESTURE;

void MtpGestureInit(MTP_GESTURE *g, const MTP_GESTURE_CONFIG *config);

/*
 * フレームを 1 つ処理し、発生したイベントを out に書く（最大 MTP_GESTURE_MAX_EVENTS 件）。戻り値は件数。
 */
size_t MtpGestureProcess(MTP_GESTURE *g, const MTP_FRAME *frame, MTP_EVENT *out, size_t capacity);

/*
 * 押したままのボタンがあれば離すイベントを返す（終了時・切断時に呼ぶ）。戻り値は件数（0 か 1）。
 */
size_t MtpGestureRelease(MTP_GESTURE *g, MTP_EVENT *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* MTP_GESTURE_H */
