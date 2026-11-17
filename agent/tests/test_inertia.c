/*
 * test_inertia.c - 2 本指の慣性スクロール・ピンチ（mtp_gesture.c）と減速（mtp_momentum.c）の単体テスト（実機なし）
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mtp_gesture.h"
#include "mtp_momentum.h"

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

typedef struct { uint8_t id; int16_t x, y; uint8_t state; uint8_t major; } F;

static size_t Step(MTP_GESTURE *g, const F *fingers, int n, MTP_EVENT *ev)
{
    MTP_FRAME f;
    int i;
    memset(&f, 0, sizeof(f));
    f.contact_count = (uint8_t)n;
    for (i = 0; i < n; i++) {
        f.contacts[i].id = fingers[i].id;
        f.contacts[i].x = fingers[i].x;
        f.contacts[i].y = fingers[i].y;
        f.contacts[i].state = fingers[i].state;
        f.contacts[i].touch_major = fingers[i].major;
    }
    return MtpGestureProcess(g, &f, ev, MTP_GESTURE_MAX_EVENTS);
}

static const MTP_EVENT *Find(const MTP_EVENT *ev, size_t n, MTP_EVENT_TYPE type)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (ev[i].type == type) return &ev[i];
    }
    return NULL;
}

/* 2 本指を (x1, y) と (x2, y) に置く */
static size_t Two(MTP_GESTURE *g, int x1, int x2, int y, MTP_EVENT *ev)
{
    F f[2];
    f[0].id = 1; f[0].x = (int16_t)x1; f[0].y = (int16_t)y; f[0].state = 2; f[0].major = 50;
    f[1].id = 2; f[1].x = (int16_t)x2; f[1].y = (int16_t)y; f[1].state = 2; f[1].major = 50;
    return Step(g, f, 2, ev);
}

/* 2 本指で奥へ frames フレーム払う（1 フレームに step 単位） */
static void Swipe(MTP_GESTURE *g, int frames, int step, int *y, MTP_EVENT *ev)
{
    int i;
    for (i = 0; i < frames; i++) {
        *y -= step;
        Two(g, -300, 300, *y, ev);
    }
}

static void test_fling_after_fast_scroll(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    const MTP_EVENT *f;
    size_t n;
    int y = 0;

    MtpGestureInit(&g, NULL);
    Two(&g, -300, 300, y, ev);
    Swipe(&g, 8, 40, &y, ev);
    n = Step(&g, NULL, 0, ev);
    f = Find(ev, n, MTP_EVENT_FLING);
    /* ナチュラル: 指を奥へ → 下へスクロール（縦は負）。40 単位 × 0.6 × 90 フレーム/秒 = 2160 */
    CHECK(f != NULL);
    CHECK(f != NULL && f->dy < -2000 && f->dy > -2300 && f->dx == 0);

    /* 次に触れたら、止める合図が 1 回だけ出る */
    {
        F a = { 1, 0, 0, 1, 50 };
        n = Step(&g, &a, 1, ev);
        CHECK(Find(ev, n, MTP_EVENT_TOUCH) != NULL);
        n = Step(&g, &a, 1, ev);
        CHECK(Find(ev, n, MTP_EVENT_TOUCH) == NULL);
    }
}

static void test_no_fling_when_stopped_before_lifting(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    size_t n;
    int y = 0, i;

    MtpGestureInit(&g, NULL);
    Two(&g, -300, 300, y, ev);
    Swipe(&g, 8, 40, &y, ev);
    for (i = 0; i < 6; i++) {
        Two(&g, -300, 300, y, ev); /* 止めて */
    }
    n = Step(&g, NULL, 0, ev);  /* から離す */
    CHECK(Find(ev, n, MTP_EVENT_FLING) == NULL);
}

static void test_no_fling_when_slow_or_disabled(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    MTP_GESTURE_CONFIG c;
    size_t n;
    int y = 0;

    MtpGestureInit(&g, NULL);
    Two(&g, -300, 300, y, ev);
    Swipe(&g, 30, 5, &y, ev); /* 5 × 0.6 × 90 = 270/秒（遅い） */
    n = Step(&g, NULL, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_FLING) == NULL);

    MtpGestureDefaultConfig(&c);
    c.inertia_enabled = 0;
    MtpGestureInit(&g, &c);
    y = 0;
    Two(&g, -300, 300, y, ev);
    Swipe(&g, 8, 40, &y, ev);
    n = Step(&g, NULL, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_FLING) == NULL);
}

static void test_no_pointer_from_remaining_finger_after_scroll(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F a = { 1, -300, -320, 2, 50 };
    size_t n;
    int y = 0, i;

    MtpGestureInit(&g, NULL);
    Two(&g, -300, 300, y, ev);
    Swipe(&g, 8, 40, &y, ev);
    Step(&g, &a, 1, ev); /* 片方が先に離れた */
    for (i = 0; i < 3; i++) {
        a.x += 50;
        n = Step(&g, &a, 1, ev);
        CHECK(Find(ev, n, MTP_EVENT_MOVE) == NULL);
    }
    Step(&g, NULL, 0, ev); /* すべて離したら元どおり */
    a.x = 0;
    Step(&g, &a, 1, ev);
    a.x = 100;
    n = Step(&g, &a, 1, ev);
    CHECK(Find(ev, n, MTP_EVENT_MOVE) != NULL);
}

static void test_resting_two_fingers_do_not_block_pointer(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F a = { 1, -300, 0, 2, 50 };
    size_t n;

    MtpGestureInit(&g, NULL);
    Two(&g, -300, 300, 0, ev);
    Two(&g, -300, 300, 0, ev);
    Step(&g, &a, 1, ev); /* 置いただけの 2 本目を離す */
    a.x = -200;
    n = Step(&g, &a, 1, ev);
    CHECK(Find(ev, n, MTP_EVENT_MOVE) != NULL);
}

static void test_pinch_zooms(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    size_t n;
    int i, zoom = 0, scroll = 0, half = 500;
    const MTP_EVENT *e;

    MtpGestureInit(&g, NULL);
    Two(&g, -half, half, 0, ev);
    for (i = 0; i < 12; i++) { /* 両方の指を外へ（間隔 1000 → 2000） */
        half += 42;
        n = Two(&g, -half, half, 0, ev);
        if ((e = Find(ev, n, MTP_EVENT_ZOOM)) != NULL) zoom += e->dy;
        if (Find(ev, n, MTP_EVENT_SCROLL) != NULL) scroll++;
    }
    /* ピンチに入るまで（間隔の変化 250）の分を除いても、拡大（正）が出る */
    CHECK(zoom > 100);
    CHECK(scroll == 0);
    n = Step(&g, NULL, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_ZOOM_END) != NULL);
    CHECK(Find(ev, n, MTP_EVENT_FLING) == NULL);

    /* つまむと縮小（負） */
    MtpGestureInit(&g, NULL);
    half = 1000;
    zoom = 0;
    Two(&g, -half, half, 0, ev);
    for (i = 0; i < 12; i++) {
        half -= 40;
        n = Two(&g, -half, half, 0, ev);
        if ((e = Find(ev, n, MTP_EVENT_ZOOM)) != NULL) zoom += e->dy;
    }
    CHECK(zoom < -50);
}

static void test_scroll_locks_out_pinch(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    size_t n;
    int i, y = 0, half = 300, zoomed = 0;

    MtpGestureInit(&g, NULL);
    Two(&g, -half, half, y, ev);
    Swipe(&g, 10, 40, &y, ev); /* 400 動かしてスクロールに決める */
    for (i = 0; i < 10; i++) {
        half += 40;
        n = Two(&g, -half, half, y, ev);
        if (Find(ev, n, MTP_EVENT_ZOOM) != NULL) zoomed = 1;
    }
    CHECK(!zoomed);
}

static void test_pinch_disabled(void)
{
    MTP_GESTURE g;
    MTP_GESTURE_CONFIG c;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    size_t n;
    int i, half = 500, zoomed = 0;

    MtpGestureDefaultConfig(&c);
    c.pinch_enabled = 0;
    MtpGestureInit(&g, &c);
    Two(&g, -half, half, 0, ev);
    for (i = 0; i < 12; i++) {
        half += 42;
        n = Two(&g, -half, half, 0, ev);
        if (Find(ev, n, MTP_EVENT_ZOOM) != NULL) zoomed = 1;
    }
    CHECK(!zoomed);
}

static void test_release_during_pinch_ends_zoom(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    int i, half = 500;

    MtpGestureInit(&g, NULL);
    Two(&g, -half, half, 0, ev);
    for (i = 0; i < 8; i++) {
        half += 42;
        Two(&g, -half, half, 0, ev);
    }
    CHECK(g.two_mode == 1);
    CHECK(MtpGestureRelease(&g, ev, MTP_GESTURE_MAX_EVENTS) == 1);
    CHECK(ev[0].type == MTP_EVENT_ZOOM_END);
    CHECK(MtpGestureRelease(&g, ev, MTP_GESTURE_MAX_EVENTS) == 0);
}

static void test_momentum_decays_and_stops(void)
{
    MTP_MOMENTUM m;
    int h, v, total = 0, steps = 0;

    MtpMomentumInit(&m);
    MtpMomentumStart(&m, 0, 1200);
    CHECK(m.active);
    while (MtpMomentumStep(&m, 8, &h, &v) && steps < 10000) {
        total += v;
        CHECK(h == 0);
        steps++;
    }
    total += v;
    /* 全体の距離 ≈ v0 × tau = 1200 × 0.4 = 480（min_speed で打ち切る分だけ少ない） */
    CHECK(total > 400 && total <= 480);
    CHECK(steps > 50 && steps < 400);

    /* 呼ぶ間隔が違っても、同じ時間なら同じくらい進む */
    {
        MTP_MOMENTUM a, b;
        int ta = 0, tb = 0, i;
        MtpMomentumInit(&a);
        MtpMomentumInit(&b);
        MtpMomentumStart(&a, 3000, 0);
        MtpMomentumStart(&b, 3000, 0);
        for (i = 0; i < 25; i++) { MtpMomentumStep(&a, 8, &h, &v); ta += h; }
        for (i = 0; i < 10; i++) { MtpMomentumStep(&b, 20, &h, &v); tb += h; }
        CHECK(abs(ta - tb) <= 2);
    }
}

static void test_momentum_stop_and_slow_start(void)
{
    MTP_MOMENTUM m;
    int h, v;

    MtpMomentumInit(&m);
    MtpMomentumStart(&m, 30, 0); /* min_speed 未満は始めない */
    CHECK(!m.active);
    MtpMomentumStart(&m, -5000, 0);
    CHECK(m.active);
    MtpMomentumStep(&m, 8, &h, &v);
    CHECK(h < 0);
    MtpMomentumStop(&m);
    CHECK(MtpMomentumStep(&m, 8, &h, &v) == 0 && h == 0 && v == 0);

    /* 上限 */
    MtpMomentumStart(&m, 0, 100000);
    CHECK(fabsf(m.vy - m.max_speed) < 1.0f);
}

/* 時刻つきで 1 フレーム */
static size_t StepAt(MTP_GESTURE *g, const F *fingers, int n, double t, MTP_EVENT *ev)
{
    MTP_FRAME f;
    int i;
    memset(&f, 0, sizeof(f));
    f.contact_count = (uint8_t)n;
    for (i = 0; i < n; i++) {
        f.contacts[i].id = fingers[i].id;
        f.contacts[i].x = fingers[i].x;
        f.contacts[i].y = fingers[i].y;
        f.contacts[i].state = fingers[i].state;
        f.contacts[i].touch_major = fingers[i].major;
    }
    return MtpGestureProcessAt(g, &f, t, ev, MTP_GESTURE_MAX_EVENTS);
}

/* 2 本指で t から dur ミリ秒触れて離す。離したフレームのイベント数を返す */
static size_t Tap2(MTP_GESTURE *g, double t, double dur, int move, MTP_EVENT *ev)
{
    F f[2] = { { 1, -300, 0, 2, 50 }, { 2, 300, 0, 2, 50 } };
    double k;
    for (k = 0; k <= dur; k += 11) {
        StepAt(g, f, 2, t + k, ev);
        f[0].y = (int16_t)(f[0].y + move);
        f[1].y = (int16_t)(f[1].y + move);
    }
    return StepAt(g, NULL, 0, t + dur + 11, ev);
}

static void test_two_finger_double_tap(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    size_t n;

    MtpGestureInit(&g, NULL);
    n = Tap2(&g, 1000, 80, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) == NULL); /* 1 回目では出ない */
    n = Tap2(&g, 1250, 80, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) != NULL);
    /* 続けてもう 1 回たたいても、3 回目は新しい 1 回目 */
    n = Tap2(&g, 1500, 80, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) == NULL);
}

static void test_double_tap_rejections(void)
{
    MTP_GESTURE g;
    MTP_GESTURE_CONFIG c;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    size_t n;

    /* 間が空きすぎ（離してから 600ms） */
    MtpGestureInit(&g, NULL);
    Tap2(&g, 1000, 80, 0, ev);
    n = Tap2(&g, 1700, 80, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) == NULL);

    /* 長く触れた（400ms）のはタップではない */
    MtpGestureInit(&g, NULL);
    Tap2(&g, 1000, 400, 0, ev);
    n = Tap2(&g, 1500, 80, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) == NULL);

    /* 動かした（スクロール）のはタップではない */
    MtpGestureInit(&g, NULL);
    Tap2(&g, 1000, 120, 30, ev);
    n = Tap2(&g, 1300, 80, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) == NULL);

    /* 1 本指のダブルタップでは出ない */
    MtpGestureInit(&g, NULL);
    {
        F a = { 1, 0, 0, 2, 50 };
        StepAt(&g, &a, 1, 1000, ev); StepAt(&g, NULL, 0, 1080, ev);
        StepAt(&g, &a, 1, 1200, ev); n = StepAt(&g, NULL, 0, 1280, ev);
        CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) == NULL);
    }

    /* 押し込んだ（クリック）ものはタップではない */
    MtpGestureInit(&g, NULL);
    {
        F f[2] = { { 1, -300, 0, 2, 50 }, { 2, 300, 0, 2, 50 } };
        MTP_FRAME fr;
        memset(&fr, 0, sizeof(fr));
        StepAt(&g, f, 2, 1000, ev);
        fr.button = 1;
        fr.contact_count = 2;
        fr.contacts[0].id = 1; fr.contacts[0].x = -300; fr.contacts[0].state = 2; fr.contacts[0].touch_major = 50;
        fr.contacts[1].id = 2; fr.contacts[1].x = 300; fr.contacts[1].state = 2; fr.contacts[1].touch_major = 50;
        MtpGestureProcessAt(&g, &fr, 1030, ev, MTP_GESTURE_MAX_EVENTS);
        StepAt(&g, NULL, 0, 1080, ev);
        n = Tap2(&g, 1250, 80, 0, ev);
        CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) == NULL);
    }

    /* 無効にしたら出ない */
    MtpGestureDefaultConfig(&c);
    c.double_tap_enabled = 0;
    MtpGestureInit(&g, &c);
    Tap2(&g, 1000, 80, 0, ev);
    n = Tap2(&g, 1250, 80, 0, ev);
    CHECK(Find(ev, n, MTP_EVENT_DOUBLE_TAP) == NULL);
}

int main(void)
{
    test_fling_after_fast_scroll();
    test_no_fling_when_stopped_before_lifting();
    test_no_fling_when_slow_or_disabled();
    test_no_pointer_from_remaining_finger_after_scroll();
    test_resting_two_fingers_do_not_block_pointer();
    test_pinch_zooms();
    test_scroll_locks_out_pinch();
    test_pinch_disabled();
    test_release_during_pinch_ends_zoom();
    test_momentum_decays_and_stops();
    test_momentum_stop_and_slow_start();
    test_two_finger_double_tap();
    test_double_tap_rejections();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
