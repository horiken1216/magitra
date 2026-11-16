/*
 * test_gesture.c - mtp_gesture.c の単体テスト（実機なし）
 */
#include <stdio.h>
#include <string.h>

#include "mtp_gesture.h"

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

typedef struct { uint8_t id; int16_t x, y; uint8_t state; uint8_t major; } F;

static MTP_FRAME Frame(int button, const F *fingers, int n)
{
    MTP_FRAME f;
    int i;
    memset(&f, 0, sizeof(f));
    f.button = (uint8_t)button;
    f.contact_count = (uint8_t)n;
    for (i = 0; i < n; i++) {
        f.contacts[i].id = fingers[i].id;
        f.contacts[i].x = fingers[i].x;
        f.contacts[i].y = fingers[i].y;
        f.contacts[i].state = fingers[i].state;
        f.contacts[i].touch_major = fingers[i].major;
    }
    return f;
}

static size_t Step(MTP_GESTURE *g, int button, const F *fingers, int n, MTP_EVENT *ev)
{
    MTP_FRAME f = Frame(button, fingers, n);
    return MtpGestureProcess(g, &f, ev, MTP_GESTURE_MAX_EVENTS);
}

/* イベント列から指定種類の 1 件目を探す */
static const MTP_EVENT *Find(const MTP_EVENT *ev, size_t n, MTP_EVENT_TYPE type)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (ev[i].type == type) return &ev[i];
    }
    return NULL;
}

static void test_one_finger_moves_pointer(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F a0 = { 3, 0, 0, 1, 50 }, a1 = { 3, 100, 0, 2, 50 }, a2 = { 3, 100, -100, 2, 50 };
    size_t n;
    const MTP_EVENT *m;

    MtpGestureInit(&g, NULL);
    /* 置いた最初のフレームでは動かない */
    CHECK(Step(&g, 0, &a0, 1, ev) == 0);

    n = Step(&g, 0, &a1, 1, ev);
    m = Find(ev, n, MTP_EVENT_MOVE);
    CHECK(m != NULL && m->dx > 0 && m->dy == 0);

    /* 奥（上）へ動かす（y が減る）と、画面でも上（dy が負） */
    n = Step(&g, 0, &a2, 1, ev);
    m = Find(ev, n, MTP_EVENT_MOVE);
    CHECK(m != NULL && m->dx == 0 && m->dy < 0);
    CHECK(Find(ev, n, MTP_EVENT_SCROLL) == NULL);
}

static void test_acceleration(void)
{
    MTP_GESTURE slow, fast;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F s0 = { 1, 0, 0, 2, 50 }, s1 = { 1, 20, 0, 2, 50 };
    F f0 = { 1, 0, 0, 2, 50 }, f1 = { 1, 200, 0, 2, 50 };
    const MTP_EVENT *ms, *mf;
    size_t n;

    MtpGestureInit(&slow, NULL);
    MtpGestureInit(&fast, NULL);
    Step(&slow, 0, &s0, 1, ev);
    n = Step(&slow, 0, &s1, 1, ev);
    ms = Find(ev, n, MTP_EVENT_MOVE);
    CHECK(ms != NULL);
    {
        int slow_dx = ms ? ms->dx : 0;
        Step(&fast, 0, &f0, 1, ev);
        n = Step(&fast, 0, &f1, 1, ev);
        mf = Find(ev, n, MTP_EVENT_MOVE);
        /* 10 倍速く動かすと、10 倍より大きく動く */
        CHECK(mf != NULL && slow_dx > 0 && mf->dx > slow_dx * 10);
    }
}

static void test_subpixel_accumulates(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    int total = 0, i;
    F f = { 2, 0, 0, 2, 50 };

    MtpGestureInit(&g, NULL);
    Step(&g, 0, &f, 1, ev);
    /* 1 フレーム 4 単位（1 画素未満）でも、続ければ動く */
    for (i = 1; i <= 50; i++) {
        size_t n;
        const MTP_EVENT *m;
        f.x = (int16_t)(i * 4);
        n = Step(&g, 0, &f, 1, ev);
        m = Find(ev, n, MTP_EVENT_MOVE);
        if (m) total += m->dx;
    }
    CHECK(total >= 20 && total <= 40); /* 200 単位 × 0.12 × (1 + 1.5 × 0.1) ≈ 27.6 */
}

static void test_two_finger_scroll(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    MTP_GESTURE_CONFIG cfg;
    /* y は下（手前）が正なので、上（奥）へ動かすと y が減る */
    F up0[2] = { { 1, 0, 0, 2, 50 }, { 2, 500, 0, 2, 50 } };
    F up1[2] = { { 1, 0, -100, 2, 50 }, { 2, 500, -100, 2, 50 } };
    F rt1[2] = { { 1, 100, -100, 2, 50 }, { 2, 600, -100, 2, 50 } };
    const MTP_EVENT *s;
    size_t n;

    /* ナチュラル: 指を上へ → 内容が上へ動く → 下へスクロール（縦ホイールは負） */
    MtpGestureInit(&g, NULL);
    CHECK(Step(&g, 0, up0, 2, ev) == 0);
    n = Step(&g, 0, up1, 2, ev);
    s = Find(ev, n, MTP_EVENT_SCROLL);
    CHECK(s != NULL && s->dy < 0 && s->dx == 0);
    CHECK(Find(ev, n, MTP_EVENT_MOVE) == NULL);

    /* ナチュラル: 指を右へ → 横ホイールは負 */
    n = Step(&g, 0, rt1, 2, ev);
    s = Find(ev, n, MTP_EVENT_SCROLL);
    CHECK(s != NULL && s->dx < 0 && s->dy == 0);

    /* 従来方向: 符号が逆になる */
    MtpGestureDefaultConfig(&cfg);
    cfg.natural_scroll = 0;
    MtpGestureInit(&g, &cfg);
    Step(&g, 0, up0, 2, ev);
    n = Step(&g, 0, up1, 2, ev);
    s = Find(ev, n, MTP_EVENT_SCROLL);
    CHECK(s != NULL && s->dy > 0);
}

static void test_left_click(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F a = { 4, 0, 0, 2, 50 };
    const MTP_EVENT *b;
    size_t n;

    MtpGestureInit(&g, NULL);
    Step(&g, 0, &a, 1, ev);
    n = Step(&g, 1, &a, 1, ev);
    b = Find(ev, n, MTP_EVENT_BUTTON);
    CHECK(b != NULL && b->button == MTP_BUTTON_LEFT && b->down == 1);
    /* 押したまま: 新たなボタンイベントは出ない */
    n = Step(&g, 1, &a, 1, ev);
    CHECK(Find(ev, n, MTP_EVENT_BUTTON) == NULL);
    n = Step(&g, 0, &a, 1, ev);
    b = Find(ev, n, MTP_EVENT_BUTTON);
    CHECK(b != NULL && b->button == MTP_BUTTON_LEFT && b->down == 0);
}

static void test_right_click_with_two_fingers(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F two[2] = { { 1, 0, 0, 2, 50 }, { 2, 500, 0, 2, 50 } };
    F one = { 1, 0, 0, 2, 50 };
    const MTP_EVENT *b;
    size_t n;

    MtpGestureInit(&g, NULL);
    Step(&g, 0, two, 2, ev);
    n = Step(&g, 1, two, 2, ev);
    b = Find(ev, n, MTP_EVENT_BUTTON);
    CHECK(b != NULL && b->button == MTP_BUTTON_RIGHT && b->down == 1);
    /* 押している途中で 1 本離しても、離すのは右ボタン */
    Step(&g, 1, &one, 1, ev);
    n = Step(&g, 0, &one, 1, ev);
    b = Find(ev, n, MTP_EVENT_BUTTON);
    CHECK(b != NULL && b->button == MTP_BUTTON_RIGHT && b->down == 0);
}

static void test_click_drag_follows_moving_finger(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F press = { 1, 0, -2000, 2, 50 };
    F both0[2] = { { 1, 0, -2000, 2, 50 }, { 2, 1000, 0, 2, 50 } };
    F both1[2] = { { 1, 0, -2000, 2, 50 }, { 2, 1100, 0, 2, 50 } };
    const MTP_EVENT *m;
    size_t n;

    MtpGestureInit(&g, NULL);
    Step(&g, 0, &press, 1, ev);
    Step(&g, 1, &press, 1, ev);           /* 1 本で押す → 左ボタン */
    CHECK(Step(&g, 1, both0, 2, ev) == 0); /* 2 本目を置いた瞬間は動かない */
    n = Step(&g, 1, both1, 2, ev);         /* 2 本目を右へ動かす */
    m = Find(ev, n, MTP_EVENT_MOVE);
    CHECK(m != NULL && m->dx > 0);
    CHECK(Find(ev, n, MTP_EVENT_SCROLL) == NULL); /* ボタン中はスクロールにしない */
}

static void test_finger_set_change_does_not_jump(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F one = { 1, 0, 0, 2, 50 };
    F two[2] = { { 1, 3000, 0, 2, 50 }, { 2, -3000, 0, 2, 50 } };

    MtpGestureInit(&g, NULL);
    Step(&g, 0, &one, 1, ev);
    /* 指が増えたフレームは、既存の指が大きく動いていても何も出さない */
    CHECK(Step(&g, 0, two, 2, ev) == 0);
}

static void test_glitch_is_ignored(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F a0 = { 1, 0, 0, 2, 50 }, a1 = { 1, 2000, 0, 2, 50 };

    MtpGestureInit(&g, NULL);
    Step(&g, 0, &a0, 1, ev);
    CHECK(Step(&g, 0, &a1, 1, ev) == 0);
}

static void test_lifting_contacts_are_not_fingers(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    /* 2 本目は離れる途中（state 3 / 接触サイズ 0）。1 本指として扱う */
    F f0[2] = { { 1, 0, 0, 2, 50 }, { 2, 500, 0, 3, 0 } };
    F f1[2] = { { 1, 100, 0, 2, 50 }, { 2, 600, 0, 3, 0 } };
    F g0[2] = { { 1, 100, 0, 2, 50 }, { 3, 900, 0, 0, 40 } }; /* state 0 も指ではない */
    F g1[2] = { { 1, 200, 0, 2, 50 }, { 3, 900, 0, 0, 40 } };
    const MTP_EVENT *m;
    size_t n;

    MtpGestureInit(&g, NULL);
    Step(&g, 0, f0, 2, ev);
    n = Step(&g, 0, f1, 2, ev);
    m = Find(ev, n, MTP_EVENT_MOVE);
    CHECK(m != NULL && m->dx > 0);
    CHECK(Find(ev, n, MTP_EVENT_SCROLL) == NULL);

    Step(&g, 0, g0, 2, ev);
    n = Step(&g, 0, g1, 2, ev);
    CHECK(Find(ev, n, MTP_EVENT_MOVE) != NULL && Find(ev, n, MTP_EVENT_SCROLL) == NULL);

    /* 右クリック判定にも数えない */
    n = Step(&g, 1, g1, 2, ev);
    m = Find(ev, n, MTP_EVENT_BUTTON);
    CHECK(m != NULL && m->button == MTP_BUTTON_LEFT);
}

static void test_release_on_exit(void)
{
    MTP_GESTURE g;
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    F a = { 1, 0, 0, 2, 50 };

    MtpGestureInit(&g, NULL);
    CHECK(MtpGestureRelease(&g, ev, 1) == 0);
    Step(&g, 0, &a, 1, ev);
    Step(&g, 1, &a, 1, ev);
    CHECK(MtpGestureRelease(&g, ev, 1) == 1);
    CHECK(ev[0].type == MTP_EVENT_BUTTON && ev[0].button == MTP_BUTTON_LEFT && ev[0].down == 0);
    CHECK(MtpGestureRelease(&g, ev, 1) == 0);
}

int main(void)
{
    test_one_finger_moves_pointer();
    test_acceleration();
    test_subpixel_accumulates();
    test_two_finger_scroll();
    test_left_click();
    test_right_click_with_two_fingers();
    test_click_drag_follows_moving_finger();
    test_finger_set_change_does_not_jump();
    test_glitch_is_ignored();
    test_lifting_contacts_are_not_fingers();
    test_release_on_exit();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
