/*
 * settings.c - 設定の既定値・読み込み・ジェスチャー判定への反映
 * 数値まわりは agent/mtp_settings.c と同じにしておく。
 */
#include "settings.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POINTER_GAIN_BASE 0.12f
#define SCROLL_GAIN_BASE  0.6f

static const char *kSwipeNames[2][4] = {
    { "Swipe3Left", "Swipe3Right", "Swipe3Up", "Swipe3Down" },
    { "Swipe4Left", "Swipe4Right", "Swipe4Up", "Swipe4Down" },
};

/* Key.<名前> で割り当てを変えられる操作 */
static const struct {
    const char *name;
    int action;
} kKeyActions[MAGITRA_KEY_ACTIONS] = {
    { "DesktopNext", MAGITRA_ACTION_DESKTOP_NEXT },
    { "DesktopPrev", MAGITRA_ACTION_DESKTOP_PREV },
    { "TaskView", MAGITRA_ACTION_TASK_VIEW },
    { "ShowDesktop", MAGITRA_ACTION_SHOW_DESKTOP },
    { "AppSwitch", MAGITRA_ACTION_APP_SWITCH },
    { "BrowserBack", MAGITRA_ACTION_BROWSER_BACK },
    { "BrowserForward", MAGITRA_ACTION_BROWSER_FORWARD },
    { "PlayPause", MAGITRA_ACTION_PLAY_PAUSE },
    { "Search", MAGITRA_ACTION_SEARCH },
    { "ZoomReset", MAGITRA_ACTION_ZOOM_RESET },
};

/* プロファイルごとの既定の割り当て（kKeyActions と同じ並び）。足すときはここに表を 1 つ足す */
static const struct {
    const char *name;
    const char *keys[MAGITRA_KEY_ACTIONS];
} kProfiles[] = {
    { "generic", {
        "ctrl+alt+right", "ctrl+alt+left", "super+w", "super+d", "alt+tab",
        "alt+left", "alt+right", "playpause", "super", "ctrl+0" } },
};

#define PROFILE_COUNT (sizeof(kProfiles) / sizeof(kProfiles[0]))

static const char *FindProfile(const char *name)
{
    size_t i;
    for (i = 0; i < PROFILE_COUNT; i++) {
        if (strcmp(kProfiles[i].name, name) == 0) return kProfiles[i].name;
    }
    return kProfiles[0].name;
}

/* explicit_keys[i] が 0 の割り当てをプロファイルの既定で埋める */
static void ApplyProfile(MAGITRA_SETTINGS *s, const int *explicit_keys)
{
    size_t p, i;
    for (p = 0; p < PROFILE_COUNT; p++) {
        if (strcmp(kProfiles[p].name, FindProfile(s->profile)) == 0) break;
    }
    for (i = 0; i < MAGITRA_KEY_ACTIONS; i++) {
        if (explicit_keys == NULL || !explicit_keys[i]) {
            snprintf(s->binding[i], sizeof(s->binding[i]), "%s", kProfiles[p].keys[i]);
        }
    }
}

void MagitraSettingsDefaults(MAGITRA_SETTINGS *s)
{
    memset(s, 0, sizeof(*s));
    s->natural_scroll = 1;
    s->force_enabled = 1;
    s->gestures_enabled = 1;
    s->pointer_speed = 50;
    s->pointer_accel = 15;
    s->scroll_speed = 50;
    s->force_threshold = 200;
    s->force_action = MAGITRA_ACTION_MIDDLE_CLICK;
    s->haptic_strength = MTP_ACTUATOR_STRENGTH_MAX;
    s->haptic_sharpness = 0;
    s->haptic_depth = MTP_ACTUATOR_DEPTH_MAX;

    s->swipe[0][MTP_DIR_LEFT] = MAGITRA_ACTION_DESKTOP_NEXT;
    s->swipe[0][MTP_DIR_RIGHT] = MAGITRA_ACTION_DESKTOP_PREV;
    s->swipe[0][MTP_DIR_UP] = MAGITRA_ACTION_TASK_VIEW;
    s->swipe[0][MTP_DIR_DOWN] = MAGITRA_ACTION_SHOW_DESKTOP;
    s->swipe[1][MTP_DIR_LEFT] = MAGITRA_ACTION_APP_SWITCH;
    s->swipe[1][MTP_DIR_RIGHT] = MAGITRA_ACTION_APP_SWITCH;
    s->swipe[1][MTP_DIR_UP] = MAGITRA_ACTION_LAUNCHER;
    s->swipe[1][MTP_DIR_DOWN] = MAGITRA_ACTION_NONE;
    s->inertia_enabled = 1;
    s->inertia_glide = 50;
    s->pinch_enabled = 1;
    s->double_tap_action = MAGITRA_ACTION_ZOOM_RESET;
    s->double_tap_interval = 400;

    snprintf(s->profile, sizeof(s->profile), "%s", FindProfile(MAGITRA_DEFAULT_PROFILE));
    ApplyProfile(s, NULL);
    snprintf(s->launcher_command, sizeof(s->launcher_command), "magitra-gui --launcher");
}

static int ClampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int ClampAction(int v)
{
    return (v >= 0 && v < MAGITRA_ACTION_COUNT) ? v : MAGITRA_ACTION_NONE;
}

void MagitraSettingsClamp(MAGITRA_SETTINGS *s)
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
    s->inertia_enabled = s->inertia_enabled ? 1 : 0;
    s->inertia_glide = ClampInt(s->inertia_glide, 1, 100);
    s->pinch_enabled = s->pinch_enabled ? 1 : 0;
    s->double_tap_action = ClampAction(s->double_tap_action);
    s->double_tap_interval = ClampInt(s->double_tap_interval, 200, 800);
    for (f = 0; f < 2; f++) {
        for (d = 0; d < 4; d++) {
            s->swipe[f][d] = ClampAction(s->swipe[f][d]);
        }
    }
}

/* 数値として読めたときだけ *value に入れる（範囲は後で Clamp が丸める） */
static void ReadInt(const char *text, int *value)
{
    char *end;
    long v = strtol(text, &end, 10);
    if (end == text || *end != '\0') return;
    *value = v > INT_MAX ? INT_MAX : (v < INT_MIN ? INT_MIN : (int)v);
}

static void SetString(char *dst, size_t size, const char *value)
{
    if (strlen(value) < size) strcpy(dst, value);
}

static char *Trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static void ApplyLine(MAGITRA_SETTINGS *s, int *explicit_keys, const char *name, const char *value)
{
    static const struct {
        const char *name;
        size_t offset;
    } ints[] = {
        { "NaturalScroll", offsetof(MAGITRA_SETTINGS, natural_scroll) },
        { "ForceTouch", offsetof(MAGITRA_SETTINGS, force_enabled) },
        { "Gestures", offsetof(MAGITRA_SETTINGS, gestures_enabled) },
        { "PointerSpeed", offsetof(MAGITRA_SETTINGS, pointer_speed) },
        { "PointerAccel", offsetof(MAGITRA_SETTINGS, pointer_accel) },
        { "ScrollSpeed", offsetof(MAGITRA_SETTINGS, scroll_speed) },
        { "ForceThreshold", offsetof(MAGITRA_SETTINGS, force_threshold) },
        { "ForceAction", offsetof(MAGITRA_SETTINGS, force_action) },
        { "HapticStrength", offsetof(MAGITRA_SETTINGS, haptic_strength) },
        { "HapticSharpness", offsetof(MAGITRA_SETTINGS, haptic_sharpness) },
        { "HapticDepth", offsetof(MAGITRA_SETTINGS, haptic_depth) },
        { "Inertia", offsetof(MAGITRA_SETTINGS, inertia_enabled) },
        { "InertiaGlide", offsetof(MAGITRA_SETTINGS, inertia_glide) },
        { "Pinch", offsetof(MAGITRA_SETTINGS, pinch_enabled) },
        { "TwoFingerDoubleTap", offsetof(MAGITRA_SETTINGS, double_tap_action) },
        { "DoubleTapInterval", offsetof(MAGITRA_SETTINGS, double_tap_interval) },
    };
    size_t i;
    int f, d;

    for (i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        if (strcmp(name, ints[i].name) == 0) {
            ReadInt(value, (int *)((char *)s + ints[i].offset));
            return;
        }
    }
    for (f = 0; f < 2; f++) {
        for (d = 0; d < 4; d++) {
            if (strcmp(name, kSwipeNames[f][d]) == 0) {
                ReadInt(value, &s->swipe[f][d]);
                return;
            }
        }
    }
    if (strcmp(name, "Profile") == 0) {
        SetString(s->profile, sizeof(s->profile), value);
    } else if (strcmp(name, "LauncherCommand") == 0) {
        if (*value != '\0') SetString(s->launcher_command, sizeof(s->launcher_command), value);
    } else if (strncmp(name, "Key.", 4) == 0) {
        MAGITRA_BINDING b;
        for (i = 0; i < MAGITRA_KEY_ACTIONS; i++) {
            if (strcmp(name + 4, kKeyActions[i].name) == 0) {
                /* 空や書式の誤りは無視して既定のままにする */
                if (MagitraBindingParse(value, &b) == 0) {
                    SetString(s->binding[i], sizeof(s->binding[i]), value);
                    explicit_keys[i] = 1;
                }
                return;
            }
        }
    }
}

void MagitraSettingsParse(MAGITRA_SETTINGS *s, const char *text)
{
    int explicit_keys[MAGITRA_KEY_ACTIONS] = { 0 };
    char profile_before[MAGITRA_PROFILE_MAX];
    const char *p = text;

    MagitraSettingsDefaults(s);
    snprintf(profile_before, sizeof(profile_before), "%s", s->profile);
    while (p != NULL && *p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[2 * MAGITRA_BINDING_MAX_LEN];
        if (len < sizeof(line)) {
            char *name, *value, *eq;
            memcpy(line, p, len);
            line[len] = '\0';
            name = Trim(line);
            eq = strchr(name, '=');
            if (*name != '#' && eq != NULL) {
                *eq = '\0';
                value = Trim(eq + 1);
                name = Trim(name);
                ApplyLine(s, explicit_keys, name, value);
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    /* Profile= で表が変わったら、明示していない割り当てを新しい表で埋め直す */
    snprintf(s->profile, sizeof(s->profile), "%s", FindProfile(s->profile));
    if (strcmp(profile_before, s->profile) != 0) {
        ApplyProfile(s, explicit_keys);
    }
    MagitraSettingsClamp(s);
}

void MagitraSettingsLoadFile(MAGITRA_SETTINGS *s, const char *path)
{
    FILE *fp = fopen(path, "r");
    char *buf = NULL;
    size_t len = 0, cap = 0;

    if (fp != NULL) {
        for (;;) {
            size_t n;
            char *grown;
            if (len + 4096 + 1 > cap) {
                cap = cap ? cap * 2 : 8192;
                grown = realloc(buf, cap);
                if (grown == NULL) { free(buf); buf = NULL; len = 0; break; }
                buf = grown;
            }
            n = fread(buf + len, 1, cap - len - 1, fp);
            if (n == 0) break;
            len += n;
            if (len > (1u << 20)) break; /* 設定ファイルとして大きすぎる */
        }
        fclose(fp);
    }
    if (buf != NULL) {
        buf[len] = '\0';
        MagitraSettingsParse(s, buf);
    } else {
        MagitraSettingsDefaults(s);
    }
    free(buf);
}

int MagitraSettingsPath(char *path, size_t size)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg != NULL && xdg[0] == '/') {
        n = snprintf(path, size, "%s/magitra/agent.conf", xdg);
    } else if (home != NULL && home[0] != '\0') {
        n = snprintf(path, size, "%s/.config/magitra/agent.conf", home);
    } else {
        return -1;
    }
    return (n > 0 && (size_t)n < size) ? 0 : -1;
}

const char *MagitraSettingsBinding(const MAGITRA_SETTINGS *s, int action)
{
    size_t i;
    for (i = 0; i < MAGITRA_KEY_ACTIONS; i++) {
        if (kKeyActions[i].action == action) return s->binding[i];
    }
    return NULL;
}

float MagitraSettingsSpeedToGain(int speed, float base)
{
    speed = ClampInt(speed, 1, 100);
    return base * powf(2.0f, (float)(speed - 50) / 25.0f);
}

float MagitraSettingsGlideToTau(int glide)
{
    glide = ClampInt(glide, 1, 100);
    return 400.0f * powf(2.0f, (float)(glide - 50) / 50.0f);
}

void MagitraSettingsApply(const MAGITRA_SETTINGS *s, MTP_GESTURE_CONFIG *config)
{
    MtpGestureDefaultConfig(config);
    config->natural_scroll = s->natural_scroll;
    config->force_enabled = s->force_enabled;
    config->swipe_enabled = s->gestures_enabled;
    config->pointer_gain = MagitraSettingsSpeedToGain(s->pointer_speed, POINTER_GAIN_BASE);
    config->pointer_accel = (float)s->pointer_accel / 10.0f;
    config->scroll_gain = MagitraSettingsSpeedToGain(s->scroll_speed, SCROLL_GAIN_BASE);
    config->force_threshold = (uint8_t)s->force_threshold;
    config->inertia_enabled = s->inertia_enabled;
    config->pinch_enabled = s->pinch_enabled;
    config->double_tap_enabled = s->double_tap_action != MAGITRA_ACTION_NONE;
    config->double_tap_ms = (float)s->double_tap_interval;
}

int MagitraSettingsSwipeAction(const MAGITRA_SETTINGS *s, int fingers, MTP_DIRECTION direction)
{
    if ((fingers != 3 && fingers != 4) || (int)direction < 0 || (int)direction > 3) {
        return MAGITRA_ACTION_NONE;
    }
    return s->swipe[fingers - 3][direction];
}

void MagitraSettingsHaptic(const MAGITRA_SETTINGS *s, MTP_HAPTIC *haptic)
{
    haptic->strength = (uint8_t)s->haptic_strength;
    haptic->sharpness = (uint8_t)s->haptic_sharpness;
    haptic->depth = (uint8_t)s->haptic_depth;
}
