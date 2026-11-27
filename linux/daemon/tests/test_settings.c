/*
 * test_settings.c - settings.c と binding.c の単体テスト（/dev も設定ファイルも読まない）
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pathprefix.h"
#include "settings.h"

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

static void test_defaults(void)
{
    MAGITRA_SETTINGS s;
    MTP_GESTURE_CONFIG c;
    MTP_HAPTIC h;

    MagitraSettingsDefaults(&s);
    CHECK(s.natural_scroll == 1 && s.pointer_speed == 50 && s.pointer_accel == 15);
    CHECK(s.force_threshold == 200 && s.force_action == MAGITRA_ACTION_MIDDLE_CLICK);
    CHECK(s.double_tap_action == MAGITRA_ACTION_ZOOM_RESET && s.double_tap_interval == 400);
    CHECK(MagitraSettingsSwipeAction(&s, 3, MTP_DIR_LEFT) == MAGITRA_ACTION_DESKTOP_NEXT);
    CHECK(MagitraSettingsSwipeAction(&s, 4, MTP_DIR_UP) == MAGITRA_ACTION_LAUNCHER);
    CHECK(MagitraSettingsSwipeAction(&s, 4, MTP_DIR_DOWN) == MAGITRA_ACTION_NONE);
    CHECK(MagitraSettingsSwipeAction(&s, 2, MTP_DIR_UP) == MAGITRA_ACTION_NONE);
    CHECK(strcmp(s.profile, "generic") == 0);
    CHECK(strcmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_DESKTOP_NEXT), "ctrl+alt+right") == 0);
    CHECK(strcmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_SEARCH), "super") == 0);
    CHECK(strcmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_ZOOM_RESET), "ctrl+0") == 0);
    CHECK(MagitraSettingsBinding(&s, MAGITRA_ACTION_MIDDLE_CLICK) == NULL);
    CHECK(MagitraSettingsBinding(&s, MAGITRA_ACTION_LAUNCHER) == NULL);
    CHECK(strcmp(s.launcher_command, "magitra-gui --launcher") == 0);

    MagitraSettingsApply(&s, &c);
    CHECK(Near(c.pointer_gain, 0.12f) && Near(c.scroll_gain, 0.6f) && Near(c.pointer_accel, 1.5f));
    CHECK(c.force_threshold == 200 && c.double_tap_enabled == 1 && Near(c.double_tap_ms, 400.0f));
    MagitraSettingsHaptic(&s, &h);
    CHECK(h.strength == 0x30 && h.sharpness == 0 && h.depth == 0x40);
    CHECK(Near(MagitraSettingsGlideToTau(50), 400.0f) && Near(MagitraSettingsGlideToTau(100), 800.0f));
    CHECK(Near(MagitraSettingsSpeedToGain(75, 1.0f), 2.0f));
}

static void test_parse(void)
{
    MAGITRA_SETTINGS s;
    MagitraSettingsParse(&s,
        "# コメント\n"
        "NaturalScroll=0\r\n"
        "  PointerSpeed = 70 \n"
        "ScrollSpeed=abc\n"
        "Swipe3Left=5\n"
        "Unknown=1\n"
        "ForceAction=99\n"
        "LauncherCommand=my-launcher --x\n"
        "Key.AppSwitch=super+tab\n"
        "Key.TaskView=exec:rofi -show window\n"
        "Key.Search=\n"
        "Key.ZoomReset=ctrl+nonsense\n"
        "NoEqualsLine\n");
    CHECK(s.natural_scroll == 0);
    CHECK(s.pointer_speed == 70);
    CHECK(s.scroll_speed == 50);                 /* 数値でなければ既定 */
    CHECK(s.swipe[0][MTP_DIR_LEFT] == MAGITRA_ACTION_APP_SWITCH);
    CHECK(s.force_action == MAGITRA_ACTION_NONE);/* 範囲外の操作は「なし」 */
    CHECK(strcmp(s.launcher_command, "my-launcher --x") == 0);
    CHECK(strcmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_APP_SWITCH), "super+tab") == 0);
    CHECK(strcmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_TASK_VIEW), "exec:rofi -show window") == 0);
    CHECK(strcmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_SEARCH), "super") == 0);      /* 空は既定 */
    CHECK(strcmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_ZOOM_RESET), "ctrl+0") == 0); /* 誤りは既定 */
    CHECK(strcmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_DESKTOP_PREV), "ctrl+alt+left") == 0);

    MagitraSettingsParse(&s, "Profile=nosuch\n");
    CHECK(strcmp(s.profile, "generic") == 0);
    MagitraSettingsParse(&s, "");
    CHECK(s.pointer_speed == 50);
}

static void test_clamp(void)
{
    MAGITRA_SETTINGS s;
    MagitraSettingsParse(&s,
        "PointerSpeed=0\nPointerAccel=99\nScrollSpeed=1000\nForceThreshold=10\n"
        "HapticStrength=255\nHapticSharpness=255\nHapticDepth=-5\nInertiaGlide=0\n"
        "DoubleTapInterval=5000\nForceTouch=7\nInertia=-1\nSwipe4Down=-3\n"
        "PointerSpeed2=1\nHapticStrength=99999999999\n");
    CHECK(s.pointer_speed == 1);
    CHECK(s.pointer_accel == 30);
    CHECK(s.scroll_speed == 100);
    CHECK(s.force_threshold == 170);
    CHECK(s.haptic_strength == 0x30);
    CHECK(s.haptic_sharpness == 0x20);
    CHECK(s.haptic_depth == 0);
    CHECK(s.inertia_glide == 1);
    CHECK(s.double_tap_interval == 800);
    CHECK(s.force_enabled == 1 && s.inertia_enabled == 1);
    CHECK(s.swipe[1][MTP_DIR_DOWN] == MAGITRA_ACTION_NONE);
}

static void test_binding(void)
{
    MAGITRA_BINDING b;

    CHECK(MagitraBindingParse("Ctrl+Alt+Right", &b) == 0);
    CHECK(b.type == MAGITRA_BINDING_KEYS && b.nkeys == 3);
    CHECK(b.keys[0] == MAGITRA_KEY_LEFTCTRL && b.keys[1] == MAGITRA_KEY_LEFTALT && b.keys[2] == 106);
    CHECK(MagitraBindingParse("super", &b) == 0 && b.nkeys == 1 && b.keys[0] == MAGITRA_KEY_LEFTMETA);
    CHECK(MagitraBindingParse(" shift + f12 ", &b) == 0 && b.keys[0] == MAGITRA_KEY_LEFTSHIFT && b.keys[1] == 88);
    CHECK(MagitraBindingParse("playpause", &b) == 0 && b.keys[0] == 164);
    CHECK(MagitraBindingParse("ctrl+0", &b) == 0 && b.keys[1] == 11);
    CHECK(MagitraBindingParse("EXEC:notify-send hi", &b) == 0);
    CHECK(b.type == MAGITRA_BINDING_EXEC && strcmp(b.command, "notify-send hi") == 0);

    CHECK(MagitraBindingParse("", &b) == -1 && b.type == MAGITRA_BINDING_NONE);
    CHECK(MagitraBindingParse("exec:", &b) == -1);
    CHECK(MagitraBindingParse("ctrl+", &b) == -1);
    CHECK(MagitraBindingParse("+a", &b) == -1);
    CHECK(MagitraBindingParse("ctrl+bogus", &b) == -1);
    CHECK(MagitraBindingParse("a+b+c+d+e+f+g", &b) == -1);
    CHECK(MagitraBindingParse(NULL, &b) == -1);

    /* キーボードに登録する一覧: 先頭は ctrl、範囲外は 0 */
    CHECK(MagitraBindingKeyAt(0) == MAGITRA_KEY_LEFTCTRL);
    CHECK(MagitraBindingKeyAt(100000) == 0);
}

static void test_path(void)
{
    char p[256];
    setenv("XDG_CONFIG_HOME", "/tmp/xdg", 1);
    CHECK(MagitraSettingsPath(p, sizeof(p)) == 0 && strcmp(p, "/tmp/xdg/magitra/agent.conf") == 0);
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/u", 1);
    CHECK(MagitraSettingsPath(p, sizeof(p)) == 0 && strcmp(p, "/home/u/.config/magitra/agent.conf") == 0);
}

static void test_pathprefix(void)
{
    CHECK(MagitraPathUnder("/sys/d/1-2", "/sys/d/1-2") == 1);
    CHECK(MagitraPathUnder("/sys/d/1-2/1-2:1.1/input/input5", "/sys/d/1-2") == 1);
    CHECK(MagitraPathUnder("/sys/d/1-2/", "/sys/d/1-2") == 1);
    CHECK(MagitraPathUnder("/sys/d/1-20/x", "/sys/d/1-2") == 0);
    CHECK(MagitraPathUnder("/sys/d/1-2x", "/sys/d/1-2") == 0);
    CHECK(MagitraPathUnder("/sys/d", "/sys/d/1-2") == 0);
    CHECK(MagitraPathUnder("/sys/d/1-2/x", "/sys/d/1-2/") == 1);
    CHECK(MagitraPathUnder("/sys/d/1-2", "") == 0);
    CHECK(MagitraPathUnder(NULL, "/a") == 0 && MagitraPathUnder("/a", NULL) == 0);
}

static void test_long_exec(void)
{
    MAGITRA_SETTINGS s;
    char text[2048];
    char longexec[1200], longkey[1200];
    const char *b;

    memset(longexec, 'x', sizeof(longexec));
    memcpy(longexec, "exec:", 5);
    longexec[sizeof(longexec) - 1] = '\0';
    memset(longkey, 'a', sizeof(longkey));
    memcpy(longkey, "ctrl+", 5);
    longkey[sizeof(longkey) - 1] = '\0';

    snprintf(text, sizeof(text), "Key.TaskView=%s\n", longexec);
    MagitraSettingsParse(&s, text);
    b = MagitraSettingsBinding(&s, MAGITRA_ACTION_TASK_VIEW);
    CHECK(strncmp(b, "exec:xxx", 8) == 0 && strlen(b) < MAGITRA_BINDING_MAX_LEN);

    snprintf(text, sizeof(text), "Key.TaskView=%s\n", longkey);
    MagitraSettingsParse(&s, text);
    CHECK(strncmp(MagitraSettingsBinding(&s, MAGITRA_ACTION_TASK_VIEW), "ctrl+aaa", 8) != 0);
}

int main(void)
{
    test_defaults();
    test_parse();
    test_clamp();
    test_binding();
    test_path();
    test_pathprefix();
    test_long_exec();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
