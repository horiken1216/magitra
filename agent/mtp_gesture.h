/*
 * mtp_gesture.h - マルチタッチのフレーム列から入力イベント（ポインター移動・ボタン・スクロール・Force Touch）を作る
 *
 * OS に依存しない純粋なロジック。フレームを 1 つ渡すと、そのフレームで発生したイベントを返す。
 * 実機なしで単体テストできるよう、入力注入（SendInput）と振動は呼び出し側で行う。
 * 時間はフレーム数で数える（実機は約 90 フレーム/秒 = 約 11ms/フレーム）。
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

    /*
     * Force Touch（1 本指で押し込んだときだけ）
     * 押し込んでから force_window_frames の間だけ様子を見て、左ボタンをまだ送らない。
     * その間に圧力 >= force_threshold が force_dwell_frames 続けば Force Touch（左ボタンは送らない）。
     * 続かないまま時間が過ぎるか、drag_start_units 以上動いたら、通常の左ボタンの押下を送る。
     * 時間内に離したら、通常の左クリック（押下と解放）を送る。
     */
    int     force_enabled;
    uint8_t force_threshold;
    int     force_dwell_frames;
    int     force_window_frames;
    float   drag_start_units;
} MTP_GESTURE_CONFIG;

void MtpGestureDefaultConfig(MTP_GESTURE_CONFIG *config);

typedef enum MTP_EVENT_TYPE {
    MTP_EVENT_MOVE = 1,    /* dx, dy（画素、画面座標） */
    MTP_EVENT_BUTTON = 2,  /* button, down */
    MTP_EVENT_SCROLL = 3,  /* dx = 横ホイール, dy = 縦ホイール（Windows の符号: 縦は正で上、横は正で右） */
    MTP_EVENT_FORCE = 4    /* Force Touch が成立した（1 回の押し込みにつき 1 回） */
} MTP_EVENT_TYPE;

typedef enum MTP_BUTTON {
    MTP_BUTTON_LEFT = 0,
    MTP_BUTTON_RIGHT = 1,
    MTP_BUTTON_MIDDLE = 2  /* mtp_gesture は出さない。Force Touch の割り当て先として呼び出し側が使う */
} MTP_BUTTON;

typedef struct MTP_EVENT {
    MTP_EVENT_TYPE type;
    int dx;
    int dy;
    MTP_BUTTON button;
    int down;
} MTP_EVENT;

#define MTP_GESTURE_MAX_EVENTS 4

typedef enum MTP_PRESS_STATE {
    MTP_PRESS_NONE = 0,
    MTP_PRESS_PENDING = 1, /* 1 本指で押し込んだ直後。Force Touch か通常クリックかを見極めている */
    MTP_PRESS_LEFT = 2,    /* 左ボタンの押下を送った */
    MTP_PRESS_RIGHT = 3,   /* 右ボタンの押下を送った */
    MTP_PRESS_FORCE = 4    /* Force Touch が成立した。離すまで何も送らない */
} MTP_PRESS_STATE;

typedef struct MTP_GESTURE {
    MTP_GESTURE_CONFIG config;

    /* 前フレームで指として数えた接触（id ごと） */
    int     prev_valid[MTP_MAX_CONTACTS];
    int16_t prev_x[MTP_MAX_CONTACTS];
    int16_t prev_y[MTP_MAX_CONTACTS];
    int     prev_count;
    uint16_t prev_id_mask;

    MTP_PRESS_STATE press;
    int   pending_frames;
    int   force_frames;
    float press_travel;

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
 * 押したままのボタンがあれば離すイベントを返す（終了時・切断時・一時停止時に呼ぶ）。戻り値は件数（0 か 1）。
 * 見極め中（左ボタンをまだ送っていない）や Force Touch 中は、離すべきボタンがないので 0。
 */
size_t MtpGestureRelease(MTP_GESTURE *g, MTP_EVENT *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* MTP_GESTURE_H */
