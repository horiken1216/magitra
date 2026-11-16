/*
 * mtp_settings.c - 設定の既定値・読み込み・保存・ジェスチャー判定への反映
 */
#include "mtp_settings.h"

#include <math.h>
#include <string.h>

#define POINTER_GAIN_BASE 0.12f
#define SCROLL_GAIN_BASE  0.6f

static const wchar_t *kSwipeNames[2][4] = {
    { L"Swipe3Left", L"Swipe3Right", L"Swipe3Up", L"Swipe3Down" },
    { L"Swipe4Left", L"Swipe4Right", L"Swipe4Up", L"Swipe4Down" },
};

void MtpSettingsDefaults(MTP_SETTINGS *s)
{
    memset(s, 0, sizeof(*s));
    s->natural_scroll = 1;
    s->force_enabled = 1;
    s->gestures_enabled = 1;
    s->pointer_speed = 50;
    s->pointer_accel = 15;
    s->scroll_speed = 50;
    s->force_threshold = 200;
    s->force_action = MTP_ACTION_MIDDLE_CLICK;
    s->haptic_strength = MTP_ACTUATOR_STRENGTH_MAX;  /* 0x30: 実機で「強くなった」 */
    s->haptic_sharpness = 0;
    s->haptic_depth = MTP_ACTUATOR_DEPTH_MAX;        /* 0x40: 実機で「重くなった」 */

    /* 3 本指: 指を左へ払うと右隣のデスクトップへ（Mac と同じ向き） */
    s->swipe[0][MTP_DIR_LEFT] = MTP_ACTION_DESKTOP_NEXT;
    s->swipe[0][MTP_DIR_RIGHT] = MTP_ACTION_DESKTOP_PREV;
    s->swipe[0][MTP_DIR_UP] = MTP_ACTION_TASK_VIEW;
    s->swipe[0][MTP_DIR_DOWN] = MTP_ACTION_SHOW_DESKTOP;
    /* 4 本指: 左右でアプリの切り替え。上下はランチャー用に空けておく */
    s->swipe[1][MTP_DIR_LEFT] = MTP_ACTION_APP_SWITCH;
    s->swipe[1][MTP_DIR_RIGHT] = MTP_ACTION_APP_SWITCH;
    s->swipe[1][MTP_DIR_UP] = MTP_ACTION_NONE;
    s->swipe[1][MTP_DIR_DOWN] = MTP_ACTION_NONE;
}

static int ClampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int ClampAction(int v)
{
    return (v >= 0 && v < MTP_ACTION_COUNT) ? v : MTP_ACTION_NONE;
}

void MtpSettingsClamp(MTP_SETTINGS *s)
{
    int f, d;
    s->natural_scroll = s->natural_scroll ? 1 : 0;
    s->force_enabled = s->force_enabled ? 1 : 0;
    s->gestures_enabled = s->gestures_enabled ? 1 : 0;
    s->pointer_speed = ClampInt(s->pointer_speed, 1, 100);
    s->pointer_accel = ClampInt(s->pointer_accel, 0, 30);
    s->scroll_speed = ClampInt(s->scroll_speed, 1, 100);
    s->force_threshold = ClampInt(s->force_threshold, 170, 250);
    s->force_action = ClampAction(s->force_action);
    s->haptic_strength = ClampInt(s->haptic_strength, 0, MTP_ACTUATOR_STRENGTH_MAX);
    s->haptic_sharpness = ClampInt(s->haptic_sharpness, 0, MTP_ACTUATOR_SHARPNESS_MAX);
    s->haptic_depth = ClampInt(s->haptic_depth, 0, MTP_ACTUATOR_DEPTH_MAX);
    for (f = 0; f < 2; f++) {
        for (d = 0; d < 4; d++) {
            s->swipe[f][d] = ClampAction(s->swipe[f][d]);
        }
    }
}

static void ReadInt(HKEY key, const wchar_t *name, int *value)
{
    DWORD v = 0, size = sizeof(v), type = 0;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&v, &size) == ERROR_SUCCESS && type == REG_DWORD) {
        *value = (int)v;
    }
}

void MtpSettingsLoad(MTP_SETTINGS *s)
{
    HKEY key;
    int f, d;

    MtpSettingsDefaults(s);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, MTP_SETTINGS_KEY, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        ReadInt(key, L"NaturalScroll", &s->natural_scroll);
        ReadInt(key, L"ForceTouch", &s->force_enabled);
        ReadInt(key, L"Gestures", &s->gestures_enabled);
        ReadInt(key, L"PointerSpeed", &s->pointer_speed);
        ReadInt(key, L"PointerAccel", &s->pointer_accel);
        ReadInt(key, L"ScrollSpeed", &s->scroll_speed);
        ReadInt(key, L"ForceThreshold", &s->force_threshold);
        ReadInt(key, L"ForceAction", &s->force_action);
        ReadInt(key, L"HapticStrength", &s->haptic_strength);
        ReadInt(key, L"HapticSharpness", &s->haptic_sharpness);
        ReadInt(key, L"HapticDepth", &s->haptic_depth);
        for (f = 0; f < 2; f++) {
            for (d = 0; d < 4; d++) {
                ReadInt(key, kSwipeNames[f][d], &s->swipe[f][d]);
            }
        }
        RegCloseKey(key);
    }
    MtpSettingsClamp(s);
}

BOOL MtpSettingsSaveValue(const wchar_t *name, DWORD value)
{
    HKEY key;
    LSTATUS rc;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, MTP_SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return FALSE;
    }
    rc = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

float MtpSettingsSpeedToGain(int speed, float base)
{
    speed = ClampInt(speed, 1, 100);
    return base * powf(2.0f, (float)(speed - 50) / 25.0f);
}

void MtpSettingsApply(const MTP_SETTINGS *s, MTP_GESTURE_CONFIG *config)
{
    MtpGestureDefaultConfig(config);
    config->natural_scroll = s->natural_scroll;
    config->force_enabled = s->force_enabled;
    config->swipe_enabled = s->gestures_enabled;
    config->pointer_gain = MtpSettingsSpeedToGain(s->pointer_speed, POINTER_GAIN_BASE);
    config->pointer_accel = (float)s->pointer_accel / 10.0f;
    config->scroll_gain = MtpSettingsSpeedToGain(s->scroll_speed, SCROLL_GAIN_BASE);
    config->force_threshold = (uint8_t)s->force_threshold;
}

MTP_ACTION MtpSettingsSwipeAction(const MTP_SETTINGS *s, int fingers, MTP_DIRECTION direction)
{
    if ((fingers != 3 && fingers != 4) || (int)direction < 0 || (int)direction > 3) {
        return MTP_ACTION_NONE;
    }
    return (MTP_ACTION)s->swipe[fingers - 3][direction];
}

void MtpSettingsHaptic(const MTP_SETTINGS *s, MTP_HAPTIC *haptic)
{
    haptic->strength = (uint8_t)s->haptic_strength;
    haptic->sharpness = (uint8_t)s->haptic_sharpness;
    haptic->depth = (uint8_t)s->haptic_depth;
}
