/*
 * test_settings.c - mtp_settings.c の単体テスト（レジストリは読まない）
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mtp_settings.h"

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int Near(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

/* 既定の設定で、これまでの既定の動き（設定画面を作る前）と完全に同じになること */
static void test_defaults_match_previous_behavior(void)
{
    MTP_SETTINGS s;
    MTP_GESTURE_CONFIG fromSettings, previous;
    MTP_HAPTIC h;

    MtpSettingsDefaults(&s);
    MtpSettingsApply(&s, &fromSettings);
    MtpGestureDefaultConfig(&previous);

    CHECK(Near(fromSettings.pointer_gain, previous.pointer_gain));
    CHECK(Near(fromSettings.pointer_accel, previous.pointer_accel));
    CHECK(Near(fromSettings.scroll_gain, previous.scroll_gain));
    CHECK(fromSettings.natural_scroll == previous.natural_scroll);
    CHECK(fromSettings.force_enabled == previous.force_enabled);
    CHECK(fromSettings.force_threshold == previous.force_threshold);
    CHECK(fromSettings.swipe_enabled == previous.swipe_enabled);
    CHECK(fromSettings.force_window_frames == previous.force_window_frames);
    CHECK(Near(fromSettings.swipe_units, previous.swipe_units));

    /* 割り当ての既定（3・4 本指ジェスチャーを追加したときの表と同じ） */
    CHECK(MtpSettingsSwipeAction(&s, 3, MTP_DIR_LEFT) == MTP_ACTION_DESKTOP_NEXT);
    CHECK(MtpSettingsSwipeAction(&s, 3, MTP_DIR_RIGHT) == MTP_ACTION_DESKTOP_PREV);
    CHECK(MtpSettingsSwipeAction(&s, 3, MTP_DIR_UP) == MTP_ACTION_TASK_VIEW);
    CHECK(MtpSettingsSwipeAction(&s, 3, MTP_DIR_DOWN) == MTP_ACTION_SHOW_DESKTOP);
    CHECK(MtpSettingsSwipeAction(&s, 4, MTP_DIR_LEFT) == MTP_ACTION_APP_SWITCH);
    CHECK(MtpSettingsSwipeAction(&s, 4, MTP_DIR_RIGHT) == MTP_ACTION_APP_SWITCH);
    CHECK(MtpSettingsSwipeAction(&s, 4, MTP_DIR_UP) == MTP_ACTION_NONE);
    CHECK(MtpSettingsSwipeAction(&s, 4, MTP_DIR_DOWN) == MTP_ACTION_NONE);
    CHECK(MtpSettingsSwipeAction(&s, 2, MTP_DIR_LEFT) == MTP_ACTION_NONE);
    CHECK(s.force_action == MTP_ACTION_MIDDLE_CLICK);

    /* 振動の既定（実機で体感を確かめた 0x30 / 0x00 / 0x40） */
    MtpSettingsHaptic(&s, &h);
    CHECK(h.strength == 0x30 && h.sharpness == 0x00 && h.depth == 0x40);
}

static void test_speed_mapping(void)
{
    CHECK(Near(MtpSettingsSpeedToGain(50, 0.12f), 0.12f));
    CHECK(Near(MtpSettingsSpeedToGain(75, 0.12f), 0.24f)); /* 25 上げると 2 倍 */
    CHECK(Near(MtpSettingsSpeedToGain(25, 0.12f), 0.06f)); /* 25 下げると半分 */
    CHECK(MtpSettingsSpeedToGain(100, 0.12f) > MtpSettingsSpeedToGain(99, 0.12f));
    CHECK(Near(MtpSettingsSpeedToGain(500, 0.12f), MtpSettingsSpeedToGain(100, 0.12f))); /* 範囲外は丸める */
    CHECK(Near(MtpSettingsSpeedToGain(-3, 0.12f), MtpSettingsSpeedToGain(1, 0.12f)));
}

static void test_clamp(void)
{
    MTP_SETTINGS s;
    MtpSettingsDefaults(&s);
    s.natural_scroll = 7;
    s.pointer_speed = 0;
    s.scroll_speed = 1000;
    s.pointer_accel = -5;
    s.force_threshold = 300;
    s.force_action = 999;
    s.haptic_strength = 0xFF;
    s.haptic_sharpness = 0x21;
    s.haptic_depth = 0x41;
    s.swipe[1][MTP_DIR_UP] = -1;
    MtpSettingsClamp(&s);
    CHECK(s.natural_scroll == 1);
    CHECK(s.pointer_speed == 1 && s.scroll_speed == 100 && s.pointer_accel == 0);
    CHECK(s.force_threshold == 250);
    CHECK(s.force_action == MTP_ACTION_NONE);
    CHECK(s.haptic_strength == MTP_ACTUATOR_STRENGTH_MAX);
    CHECK(s.haptic_sharpness == MTP_ACTUATOR_SHARPNESS_MAX);
    CHECK(s.haptic_depth == MTP_ACTUATOR_DEPTH_MAX);
    CHECK(s.swipe[1][MTP_DIR_UP] == MTP_ACTION_NONE);
}

static void test_apply_changes(void)
{
    MTP_SETTINGS s;
    MTP_GESTURE_CONFIG c;
    MtpSettingsDefaults(&s);
    s.natural_scroll = 0;
    s.force_enabled = 0;
    s.gestures_enabled = 0;
    s.pointer_accel = 0;
    s.force_threshold = 230;
    s.swipe[0][MTP_DIR_UP] = MTP_ACTION_SEARCH;
    MtpSettingsApply(&s, &c);
    CHECK(c.natural_scroll == 0 && c.force_enabled == 0 && c.swipe_enabled == 0);
    CHECK(Near(c.pointer_accel, 0.0f));
    CHECK(c.force_threshold == 230);
    CHECK(MtpSettingsSwipeAction(&s, 3, MTP_DIR_UP) == MTP_ACTION_SEARCH);
}

static void test_action_names(void)
{
    int a;
    for (a = 0; a < MTP_ACTION_COUNT; a++) {
        CHECK(MtpActionName((MTP_ACTION)a) != NULL && MtpActionName((MTP_ACTION)a)[0] != '\0');
    }
    CHECK(strcmp(MtpActionName((MTP_ACTION)999), "なし") == 0);
}

int main(void)
{
    test_defaults_match_previous_behavior();
    test_speed_mapping();
    test_clamp();
    test_apply_changes();
    test_action_names();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
