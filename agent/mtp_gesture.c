/*
 * mtp_gesture.c - フレーム列から入力イベントを作る（OS 非依存）
 *
 * 判定の規則:
 * - 指として数えるのは、状態ビットが 1（触れ始め）か 2（接触中）で、touch_major >= min_touch_major の接触
 *   （実機で 0/3 は離れる途中、接触サイズ 0 と確認済み。probe/FINDINGS.md）
 * - 物理クリックの押下時に指が 2 本以上なら右ボタン、それ以外は左ボタン。離すまで同じボタンを保つ
 * - 指の組み合わせ（id の集合）が前フレームと変わったフレームでは動きを出さない（置いた・離した瞬間の飛びを防ぐ）
 * - ボタンを押している間: 一番大きく動いた指でポインターを動かす（クリックしている指は止まっているため）
 * - ボタンを押していない間: 1 本ならポインター移動、2 本ならスクロール、3 本以上は何もしない（今後のジェスチャー用）
 * - 1 フレームで MTP_GLITCH_UNITS を超える移動は読み取りの乱れとして捨てる
 */
#include "mtp_gesture.h"

#include <math.h>
#include <string.h>

#define MTP_GLITCH_UNITS      800.0f  /* 約 17mm（X 方向の約 47.6 単位/mm で換算） */
#define MTP_ACCEL_REF_UNITS   40.0f   /* この速さで加速が 1 段分かかる */
#define MTP_ACCEL_MAX_STEPS   3.0f

void MtpGestureDefaultConfig(MTP_GESTURE_CONFIG *config)
{
    config->pointer_gain = 0.12f;
    config->pointer_accel = 1.5f;
    config->scroll_gain = 0.6f;
    config->natural_scroll = 1;
    config->min_touch_major = 1;
}

void MtpGestureInit(MTP_GESTURE *g, const MTP_GESTURE_CONFIG *config)
{
    memset(g, 0, sizeof(*g));
    if (config != NULL) {
        g->config = *config;
    } else {
        MtpGestureDefaultConfig(&g->config);
    }
}

static int IsFinger(const MTP_GESTURE *g, const MTP_CONTACT *c)
{
    return (c->state == 1 || c->state == 2) && c->touch_major >= g->config.min_touch_major && c->id < MTP_MAX_CONTACTS;
}

static int Take(float *remainder, float value)
{
    float total = *remainder + value;
    int whole = (int)total; /* 0 方向へ切り捨て */
    *remainder = total - (float)whole;
    return whole;
}

static size_t Push(MTP_EVENT *out, size_t count, size_t capacity, const MTP_EVENT *ev)
{
    if (count < capacity) {
        out[count] = *ev;
        return count + 1;
    }
    return count;
}

size_t MtpGestureProcess(MTP_GESTURE *g, const MTP_FRAME *frame, MTP_EVENT *out, size_t capacity)
{
    int cur_valid[MTP_MAX_CONTACTS];
    int16_t cur_x[MTP_MAX_CONTACTS];
    int16_t cur_y[MTP_MAX_CONTACTS];
    uint16_t mask = 0;
    int count = 0;
    size_t n = 0;
    int i;
    MTP_EVENT ev;

    if (g == NULL || frame == NULL || out == NULL) {
        return 0;
    }

    memset(cur_valid, 0, sizeof(cur_valid));
    memset(cur_x, 0, sizeof(cur_x));
    memset(cur_y, 0, sizeof(cur_y));
    for (i = 0; i < frame->contact_count; i++) {
        const MTP_CONTACT *c = &frame->contacts[i];
        if (!IsFinger(g, c) || cur_valid[c->id]) {
            continue;
        }
        cur_valid[c->id] = 1;
        cur_x[c->id] = c->x;
        cur_y[c->id] = c->y;
        mask |= (uint16_t)(1u << c->id);
        count++;
    }

    /* ボタン */
    if (frame->button && !g->button_down) {
        g->button_down = 1;
        g->button_held = count >= 2 ? MTP_BUTTON_RIGHT : MTP_BUTTON_LEFT;
        memset(&ev, 0, sizeof(ev));
        ev.type = MTP_EVENT_BUTTON;
        ev.button = g->button_held;
        ev.down = 1;
        n = Push(out, n, capacity, &ev);
    } else if (!frame->button && g->button_down) {
        g->button_down = 0;
        memset(&ev, 0, sizeof(ev));
        ev.type = MTP_EVENT_BUTTON;
        ev.button = g->button_held;
        ev.down = 0;
        n = Push(out, n, capacity, &ev);
    }

    /* 動き（指の組み合わせが前フレームと同じときだけ） */
    if (count > 0 && mask == g->prev_id_mask) {
        float sum_dx = 0, sum_dy = 0;
        float best_dx = 0, best_dy = 0, best_len = -1;
        int glitch = 0;

        for (i = 0; i < MTP_MAX_CONTACTS; i++) {
            float dx, dy, len;
            if (!cur_valid[i] || !g->prev_valid[i]) {
                continue;
            }
            dx = (float)(cur_x[i] - g->prev_x[i]);
            dy = (float)(cur_y[i] - g->prev_y[i]);
            len = sqrtf(dx * dx + dy * dy);
            if (len > MTP_GLITCH_UNITS) {
                glitch = 1;
                break;
            }
            sum_dx += dx;
            sum_dy += dy;
            if (len > best_len) {
                best_len = len;
                best_dx = dx;
                best_dy = dy;
            }
        }

        if (!glitch) {
            int pointer = 0;
            float pdx = 0, pdy = 0;

            if (g->button_down) {
                pointer = 1;
                pdx = best_dx;
                pdy = best_dy;
            } else if (count == 1) {
                pointer = 1;
                pdx = sum_dx;
                pdy = sum_dy;
            } else if (count == 2) {
                float adx = sum_dx / 2.0f;
                float ady = sum_dy / 2.0f;
                /*
                 * Windows のホイール: 縦は正で上へ、横は正で右へスクロール。
                 * ナチュラル（内容が指と同じ向きに動く）:
                 *   指を上へ（y が減る）→ 下へスクロール（縦は負）→ 縦は y の変化と同じ符号
                 *   指を右へ（x が増える）→ 左へスクロール（横は負）→ 横は x の変化と逆の符号
                 * 従来方向はどちらも逆。
                 */
                float vsign = g->config.natural_scroll ? 1.0f : -1.0f;
                float hsign = -vsign;
                int h = Take(&g->scroll_rem_x, hsign * adx * g->config.scroll_gain);
                int v = Take(&g->scroll_rem_y, vsign * ady * g->config.scroll_gain);
                if (h != 0 || v != 0) {
                    memset(&ev, 0, sizeof(ev));
                    ev.type = MTP_EVENT_SCROLL;
                    ev.dx = h;
                    ev.dy = v;
                    n = Push(out, n, capacity, &ev);
                }
            }

            if (pointer) {
                float speed = sqrtf(pdx * pdx + pdy * pdy);
                float steps = speed / MTP_ACCEL_REF_UNITS;
                float gain;
                int ix, iy;
                if (steps > MTP_ACCEL_MAX_STEPS) {
                    steps = MTP_ACCEL_MAX_STEPS;
                }
                gain = g->config.pointer_gain * (1.0f + g->config.pointer_accel * steps);
                ix = Take(&g->move_rem_x, pdx * gain);
                iy = Take(&g->move_rem_y, pdy * gain); /* トラックパッドも画面も下が正 */
                if (ix != 0 || iy != 0) {
                    memset(&ev, 0, sizeof(ev));
                    ev.type = MTP_EVENT_MOVE;
                    ev.dx = ix;
                    ev.dy = iy;
                    n = Push(out, n, capacity, &ev);
                }
            }
        }
    } else {
        /* 組み合わせが変わったら端数を捨てる（前の動作の続きを次の動作へ持ち込まない） */
        g->move_rem_x = g->move_rem_y = 0;
        g->scroll_rem_x = g->scroll_rem_y = 0;
    }

    memcpy(g->prev_valid, cur_valid, sizeof(cur_valid));
    for (i = 0; i < MTP_MAX_CONTACTS; i++) {
        if (cur_valid[i]) {
            g->prev_x[i] = cur_x[i];
            g->prev_y[i] = cur_y[i];
        }
    }
    g->prev_count = count;
    g->prev_id_mask = mask;
    return n;
}

size_t MtpGestureRelease(MTP_GESTURE *g, MTP_EVENT *out, size_t capacity)
{
    MTP_EVENT ev;

    if (g == NULL || out == NULL || capacity == 0 || !g->button_down) {
        return 0;
    }
    g->button_down = 0;
    memset(&ev, 0, sizeof(ev));
    ev.type = MTP_EVENT_BUTTON;
    ev.button = g->button_held;
    ev.down = 0;
    out[0] = ev;
    return 1;
}
