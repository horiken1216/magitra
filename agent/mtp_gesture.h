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

    /*
     * 3・4 本指のスワイプ（ボタンを押していないとき）
     * 3 本以上が触れてからの平均移動量が swipe_units を超え、主な向きの移動量が
     * もう一方の swipe_axis_ratio 倍以上なら、その向きのスワイプを 1 回出す。指が 3 本未満になるまで次は出さない。
     */
    int   swipe_enabled;
    float swipe_units;
    float swipe_axis_ratio;

    /*
     * 慣性スクロール: 2 本指のスクロール中に指を離したとき、直前の速さ（最近 MTP_FLING_SAMPLES フレームの平均）が
     * inertia_min_speed（ホイール単位/秒）以上なら MTP_EVENT_FLING を出す。減速して止めるのは呼び出し側（mtp_momentum）。
     * 離す直前に inertia_idle_frames より長く止まっていたら出さない（止めてから離したとき）。
     */
    int   inertia_enabled;
    float inertia_min_speed;
    int   inertia_idle_frames;

    /*
     * ピンチ: 2 本指の間隔が、触れたときから pinch_units 以上変わり、かつ指の中心の移動量より大きければ拡大縮小に入る。
     * 中心が scroll_lock_units 以上動いたら、その 2 本指の動作はスクロールに決まり、ピンチには入らない。
     * 拡大縮小の量: pinch_gain × ln(今の間隔 / 前の間隔)（Ctrl + ホイール単位。間隔が 2 倍で約 2 ノッチ）
     */
    int   pinch_enabled;
    float pinch_units;
    float scroll_lock_units;
    float pinch_gain;

    /*
     * 2 本指のダブルタップ（押し込まずに 2 本指で軽く 2 回たたく）。
     * 1 回のタップ: 触れてから全部離すまでが tap_max_ms 以内、指は最大 2 本、物理クリックなし、
     * 動いた量が tap_max_units 以内。1 回目を離してから double_tap_ms 以内に 2 回目が終われば MTP_EVENT_DOUBLE_TAP。
     */
    int   double_tap_enabled;
    float tap_max_ms;
    float tap_max_units;
    float double_tap_ms;
} MTP_GESTURE_CONFIG;

void MtpGestureDefaultConfig(MTP_GESTURE_CONFIG *config);

typedef enum MTP_EVENT_TYPE {
    MTP_EVENT_MOVE = 1,    /* dx, dy（画素、画面座標） */
    MTP_EVENT_BUTTON = 2,  /* button, down */
    MTP_EVENT_SCROLL = 3,  /* dx = 横ホイール, dy = 縦ホイール（Windows の符号: 縦は正で上、横は正で右） */
    MTP_EVENT_FORCE = 4,   /* Force Touch が成立した（1 回の押し込みにつき 1 回） */
    MTP_EVENT_SWIPE = 5,   /* fingers 本でのスワイプ（direction は指が動いた向き） */
    MTP_EVENT_FLING = 6,   /* 慣性スクロールの開始。dx, dy = 速さ（ホイール単位/秒、MTP_EVENT_SCROLL と同じ符号） */
    MTP_EVENT_TOUCH = 7,   /* MTP_EVENT_FLING の後、最初に指が触れた。慣性スクロールを止める */
    MTP_EVENT_ZOOM = 8,    /* 拡大縮小。dy = Ctrl + ホイールの量（正で拡大） */
    MTP_EVENT_ZOOM_END = 9, /* 拡大縮小の終わり（押していた Ctrl を離す） */
    MTP_EVENT_DOUBLE_TAP = 10 /* 2 本指のダブルタップ（fingers = 2） */
} MTP_EVENT_TYPE;

typedef enum MTP_DIRECTION {
    MTP_DIR_LEFT = 0,
    MTP_DIR_RIGHT = 1,
    MTP_DIR_UP = 2,    /* 奥へ */
    MTP_DIR_DOWN = 3   /* 手前へ */
} MTP_DIRECTION;

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
    int fingers;             /* MTP_EVENT_SWIPE: 3 か 4 */
    MTP_DIRECTION direction; /* MTP_EVENT_SWIPE */
} MTP_EVENT;

#define MTP_GESTURE_MAX_EVENTS 6
#define MTP_FLING_SAMPLES 4
#define MTP_FRAMES_PER_SECOND 90.0f

typedef enum MTP_PRESS_STATE {
    MTP_PRESS_NONE = 0,
    MTP_PRESS_PENDING = 1, /* 1 本指で押し込んだ直後。Force Touch か通常クリックかを見極めている */
    MTP_PRESS_LEFT = 2,    /* 左ボタンの押下を送った */
    MTP_PRESS_RIGHT = 3,   /* 右ボタンの押下を送った */
    MTP_PRESS_FORCE = 4,   /* Force Touch が成立した。離すまで何も送らない */
    MTP_PRESS_IGNORED = 5  /* 3 本以上で押し込んだ（スワイプ中の押し込み）。クリックは送らず、スワイプの判定は続ける */
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

    /* 3 本以上のスワイプ。指が 3 本以上になってから 3 本未満に戻るまでを 1 回の動作とみなす */
    int   multi_active;
    int   multi_max_fingers;
    int   multi_fired;
    float multi_dx, multi_dy;
    int   multi_cooldown; /* 3 本以上の動作の後、指がすべて離れるまでポインターもスクロールも出さない */

    /* 2 本指の動作（スクロールかピンチ）。2 本で触れてから組み合わせが変わるまでを 1 回の動作とみなす */
    int      two_active;
    uint16_t two_mask;
    int      two_mode;       /* 0: まだ決まっていない（スクロールは出す）、1: ピンチ、2: スクロールに決まった */
    float    two_start_dist;
    float    two_prev_dist;
    float    two_travel;
    int      two_cooldown;   /* スクロール・ピンチの後、指がすべて離れるまでポインターを動かさない */
    float    fling_h[MTP_FLING_SAMPLES], fling_v[MTP_FLING_SAMPLES];
    int      fling_count;    /* 記録したフレーム数（MTP_FLING_SAMPLES で頭打ちにせず数える） */
    int      fling_idle;     /* スクロールが止まっているフレーム数 */
    float    zoom_rem;
    int      fling_sent;     /* FLING を出した後、まだ指が触れていない */

    /* タップの判定。時刻はミリ秒（MtpGestureProcessAt で渡す。MtpGestureProcess ならフレーム数から作る） */
    double   now_ms;
    double   frame_clock_ms;
    int      seq_active;     /* 指が触れてから全部離れるまで */
    double   seq_start_ms;
    int      seq_max_fingers;
    int      seq_clicked;
    float    seq_travel;
    int      tap_pending;    /* 1 回目の 2 本指タップが終わった */
    double   tap_end_ms;

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
 * 上と同じ。now_ms にフレームを受け取った時刻（ミリ秒、単調増加）を渡す。
 * 指が触れていない間はフレームが届かないことがあるので、タップの間隔はフレーム数でなく時刻で測る。
 */
size_t MtpGestureProcessAt(MTP_GESTURE *g, const MTP_FRAME *frame, double now_ms, MTP_EVENT *out, size_t capacity);

/*
 * 押したままのボタンがあれば離すイベントを返す（終了時・切断時・一時停止時に呼ぶ）。
 * 拡大縮小の途中なら MTP_EVENT_ZOOM_END も返す。戻り値は件数（0〜2）。
 * 見極め中（左ボタンをまだ送っていない）や Force Touch 中は、離すべきボタンがない。
 */
size_t MtpGestureRelease(MTP_GESTURE *g, MTP_EVENT *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* MTP_GESTURE_H */
