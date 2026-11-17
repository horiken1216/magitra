/*
 * mtp_settings.h - 常駐アプリの設定（HKCU\Software\MTP\Agent の DWORD 値）
 *
 * 設定画面（settings/MtpSettings.cs）と同じ名前・範囲を使う。名前や意味を変えるときは両方を直すこと。
 *
 * | 値の名前          | 範囲      | 既定 | 意味                                                         |
 * |-------------------|-----------|------|--------------------------------------------------------------|
 * | NaturalScroll     | 0/1       | 1    | ナチュラルスクロール                                         |
 * | ForceTouch        | 0/1       | 1    | Force Touch                                                  |
 * | Gestures          | 0/1       | 1    | 3・4 本指ジェスチャー                                        |
 * | PointerSpeed      | 1..100    | 50   | ポインターの速さ（50 で倍率 0.12、25 上げるごとに 2 倍）     |
 * | PointerAccel      | 0..30     | 15   | 加速（10 で 1.0）                                            |
 * | ScrollSpeed       | 1..100    | 50   | スクロールの速さ（50 で倍率 0.6、25 上げるごとに 2 倍）      |
 * | ForceThreshold    | 170..250  | 200  | Force Touch とみなす圧力                                     |
 * | ForceAction       | MTP_ACTION| 6    | Force Touch の割り当て（既定は中クリック）                   |
 * | HapticStrength    | 0..0x30   | 0x30 | Force Touch の振動: 強さ                                     |
 * | HapticSharpness   | 0..0x20   | 0    | Force Touch の振動: 鋭さ                                     |
 * | HapticDepth       | 0..0x40   | 0x40 | Force Touch の振動: 重さ                                     |
 * | Swipe3Left など   | MTP_ACTION| 下表 | スワイプの割り当て（Swipe{3,4}{Left,Right,Up,Down}）          |
 * | Inertia           | 0/1       | 1    | 慣性スクロール                                               |
 * | InertiaGlide      | 1..100    | 50   | 慣性の滑る長さ（50 で 0.4 秒、50 上げるごとに 2 倍）         |
 * | Pinch             | 0/1       | 1    | ピンチで拡大縮小（Ctrl + ホイール）                          |
 * | TwoFingerDoubleTap| MTP_ACTION| 11   | 2 本指のダブルタップの割り当て（既定は拡大率を戻す）         |
 * | DoubleTapInterval | 200..800  | 400  | ダブルタップの間隔（ミリ秒。1 回目を離してから 2 回目まで）  |
 */
#ifndef MTP_SETTINGS_H
#define MTP_SETTINGS_H

#include <windows.h>

#include "mtp_actions.h"
#include "mtp_core.h"
#include "mtp_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_SETTINGS_KEY L"Software\\MTP\\Agent"

/* 設定画面から常駐アプリへ送る登録メッセージ（RegisterWindowMessageW） */
#define MTP_MSG_SETTINGS_CHANGED L"MTP_SETTINGS_CHANGED"  /* 設定を読み直す */
#define MTP_MSG_HAPTIC_TEST      L"MTP_HAPTIC_TEST"       /* wParam = 強さ | 鋭さ << 8 | 重さ << 16 で振動させる */

/* 常駐アプリ（トレイ版）の見えないウィンドウ。設定画面がこれを探して上のメッセージを送る */
#define MTP_AGENT_WINDOW_CLASS L"MtpAgentTrayWindow"

typedef struct MTP_SETTINGS {
    int natural_scroll;
    int force_enabled;
    int gestures_enabled;
    int pointer_speed;
    int pointer_accel;
    int scroll_speed;
    int force_threshold;
    int force_action;
    int haptic_strength;
    int haptic_sharpness;
    int haptic_depth;
    int swipe[2][4]; /* [指の本数 - 3][MTP_DIRECTION] = MTP_ACTION */
    int inertia_enabled;
    int inertia_glide;
    int pinch_enabled;
    int double_tap_action;
    int double_tap_interval;
} MTP_SETTINGS;

void MtpSettingsDefaults(MTP_SETTINGS *s);

/* 範囲外の値を範囲内へ丸める */
void MtpSettingsClamp(MTP_SETTINGS *s);

/* 既定値に、レジストリに保存された値を上書きして読む（丸め込み済み） */
void MtpSettingsLoad(MTP_SETTINGS *s);

/* 1 つの値を保存する */
BOOL MtpSettingsSaveValue(const wchar_t *name, DWORD value);

/* ジェスチャー判定の設定へ反映する（判定の内部パラメーターは既定値のまま） */
void MtpSettingsApply(const MTP_SETTINGS *s, MTP_GESTURE_CONFIG *config);

MTP_ACTION MtpSettingsSwipeAction(const MTP_SETTINGS *s, int fingers, MTP_DIRECTION direction);

void MtpSettingsHaptic(const MTP_SETTINGS *s, MTP_HAPTIC *haptic);

/* 速さ（1..100）を倍率に変換する。50 で base、25 上げるごとに 2 倍 */
float MtpSettingsSpeedToGain(int speed, float base);

/* 慣性の滑る長さ（1..100）を、速さが 1/e になる時間（ミリ秒）に変換する。50 で 400ms、50 上げるごとに 2 倍 */
float MtpSettingsGlideToTau(int glide);

#ifdef __cplusplus
}
#endif

#endif /* MTP_SETTINGS_H */
