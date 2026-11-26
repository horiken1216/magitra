/*
 * settings.h - 常駐アプリ magitrad の設定（$XDG_CONFIG_HOME/magitra/agent.conf）
 *
 * 数値の名前・範囲・既定値は agent/mtp_settings.h と同じ（agent/mtp_settings.c と同じにしておく）。
 * Linux だけの文字列キー: Key.<操作名> と LauncherCommand、Profile。
 * 既定値はこのファイル（settings.c）だけで決める。GUI 側は linux/gui/settings.cpp。
 */
#ifndef MAGITRA_SETTINGS_H
#define MAGITRA_SETTINGS_H

#include "binding.h"
#include "mtp_core.h"
#include "mtp_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MAGITRA_DEFAULT_PROFILE
#define MAGITRA_DEFAULT_PROFILE "generic"
#endif

/* agent/mtp_actions.h の MTP_ACTION と同じ値（そちらは windows.h が要るので別に定義する） */
typedef enum MAGITRA_ACTION {
    MAGITRA_ACTION_NONE = 0,
    MAGITRA_ACTION_DESKTOP_NEXT = 1,
    MAGITRA_ACTION_DESKTOP_PREV = 2,
    MAGITRA_ACTION_TASK_VIEW = 3,
    MAGITRA_ACTION_SHOW_DESKTOP = 4,
    MAGITRA_ACTION_APP_SWITCH = 5,
    MAGITRA_ACTION_MIDDLE_CLICK = 6,
    MAGITRA_ACTION_BROWSER_BACK = 7,
    MAGITRA_ACTION_BROWSER_FORWARD = 8,
    MAGITRA_ACTION_PLAY_PAUSE = 9,
    MAGITRA_ACTION_SEARCH = 10,
    MAGITRA_ACTION_ZOOM_RESET = 11,
    MAGITRA_ACTION_LAUNCHER = 12,
    MAGITRA_ACTION_COUNT
} MAGITRA_ACTION;

#define MAGITRA_KEY_ACTIONS 10  /* Key.<操作名> で割り当てを変えられる操作の数 */
#define MAGITRA_PROFILE_MAX 16

typedef struct MAGITRA_SETTINGS {
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
    int swipe[2][4]; /* [指の本数 - 3][MTP_DIRECTION] = MAGITRA_ACTION */
    int inertia_enabled;
    int inertia_glide;
    int pinch_enabled;
    int double_tap_action;
    int double_tap_interval;

    char profile[MAGITRA_PROFILE_MAX];
    char binding[MAGITRA_KEY_ACTIONS][MAGITRA_BINDING_MAX_LEN]; /* 実際に使う割り当て（既定を埋めた後） */
    char launcher_command[MAGITRA_BINDING_MAX_LEN];
} MAGITRA_SETTINGS;

void MagitraSettingsDefaults(MAGITRA_SETTINGS *s);

/* 範囲外の値を範囲内へ丸める */
void MagitraSettingsClamp(MAGITRA_SETTINGS *s);

/* 設定ファイルの中身（テキスト）を既定値の上に重ねて読む（丸め込み済み） */
void MagitraSettingsParse(MAGITRA_SETTINGS *s, const char *text);

/* path の設定ファイルを読む。ファイルがなければ既定値 */
void MagitraSettingsLoadFile(MAGITRA_SETTINGS *s, const char *path);

/* 既定の場所（$XDG_CONFIG_HOME/magitra/agent.conf）を path に書く。成功なら 0 */
int MagitraSettingsPath(char *path, size_t size);

/* 操作に割り当てたキー/コマンドの文字列。割り当てられない操作（なし・中クリック・ランチャー）は NULL */
const char *MagitraSettingsBinding(const MAGITRA_SETTINGS *s, int action);

/* ジェスチャー判定の設定へ反映する（判定の内部パラメーターは既定値のまま） */
void MagitraSettingsApply(const MAGITRA_SETTINGS *s, MTP_GESTURE_CONFIG *config);

int MagitraSettingsSwipeAction(const MAGITRA_SETTINGS *s, int fingers, MTP_DIRECTION direction);

void MagitraSettingsHaptic(const MAGITRA_SETTINGS *s, MTP_HAPTIC *haptic);

/* 速さ（1..100）を倍率に変換する。50 で base、25 上げるごとに 2 倍 */
float MagitraSettingsSpeedToGain(int speed, float base);

/* 慣性の滑る長さ（1..100）を、速さが 1/e になる時間（ミリ秒）に変換する */
float MagitraSettingsGlideToTau(int glide);

#ifdef __cplusplus
}
#endif

#endif /* MAGITRA_SETTINGS_H */
