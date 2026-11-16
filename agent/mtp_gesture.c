/*
 * mtp_gesture.c - フレーム列から入力イベントを作る（OS 非依存）
 *
 * 判定の規則:
 * - 指として数えるのは、状態ビットが 1（触れ始め）か 2（接触中）で、touch_major >= min_touch_major の接触
 *   （実機で 0/3 は離れる途中、接触サイズ 0 と確認済み。probe/FINDINGS.md）
 * - 物理クリックの押下時に指がちょうど 2 本なら右ボタン。離すまで右ボタンを保つ
 * - 3 本以上で押し込んだときはクリックを送らない（払う途中の押し込み。実機で確認、2026-11-16）
 * - 1 本指の押下は、Force Touch が有効なら見極め（MTP_PRESS_PENDING）に入る（mtp_gesture.h の説明を参照）。
 *   実機では、強く押すと押し始めから 2〜34ms で圧力が 200 を超え、普通のクリックは最大 156 だった（2026-11-16）
 * - 指の組み合わせ（id の集合）が前フレームと変わったフレームでは動きを出さない（置いた・離した瞬間の飛びを防ぐ）
 * - 見極め中と Force Touch 中は、ポインターもスクロールも出さない
 * - ボタンを押している間: 一番大きく動いた指でポインターを動かす（クリックしている指は止まっているため）
 * - ボタンを押していない間: 1 本ならポインター移動、2 本ならスクロール、3 本以上はスワイプの判定だけ
 * - 3 本以上で触れた後は、指がすべて離れるまでポインターもスクロールも出さない（離す途中で残った指の動きを拾わない）
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
    config->force_enabled = 1;
    config->force_threshold = 200;
    config->force_dwell_frames = 3;   /* 約 33ms */
    config->force_window_frames = 14; /* 約 155ms */
    config->drag_start_units = 100.0f; /* 約 2mm */
    config->swipe_enabled = 1;
    config->swipe_units = 500.0f;     /* 約 10mm */
    config->swipe_axis_ratio = 1.5f;
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

static size_t PushEvent(MTP_EVENT *out, size_t count, size_t capacity, MTP_EVENT_TYPE type, int dx, int dy,
    MTP_BUTTON button, int down)
{
    if (count < capacity) {
        memset(&out[count], 0, sizeof(out[count]));
        out[count].type = type;
        out[count].dx = dx;
        out[count].dy = dy;
        out[count].button = button;
        out[count].down = down;
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
    uint8_t pmax = 0;
    size_t n = 0;
    int i;
    int motion_valid = 0;
    float sum_dx = 0, sum_dy = 0;
    float best_dx = 0, best_dy = 0, best_len = 0;

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
        if (c->pressure > pmax) {
            pmax = c->pressure;
        }
    }

    /* 動きの量（指の組み合わせが前フレームと同じときだけ） */
    if (count > 0 && mask == g->prev_id_mask) {
        int glitch = 0;
        float best = -1;
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
            if (len > best) {
                best = len;
                best_dx = dx;
                best_dy = dy;
            }
        }
        if (!glitch) {
            motion_valid = 1;
            best_len = best > 0 ? best : 0;
        }
    } else {
        /* 組み合わせが変わったら端数を捨てる（前の動作の続きを次の動作へ持ち込まない） */
        g->move_rem_x = g->move_rem_y = 0;
        g->scroll_rem_x = g->scroll_rem_y = 0;
    }

    /* ボタン */
    if (frame->button) {
        if (g->press == MTP_PRESS_NONE) {
            if (count >= 3) {
                /* 感圧式なので、払う途中で押し込んでしまうことがある。右クリックにはしない */
                g->press = MTP_PRESS_IGNORED;
            } else if (count == 2) {
                g->press = MTP_PRESS_RIGHT;
                n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_RIGHT, 1);
            } else if (g->config.force_enabled) {
                g->press = MTP_PRESS_PENDING;
                g->pending_frames = 0;
                g->force_frames = 0;
                g->press_travel = 0;
            } else {
                g->press = MTP_PRESS_LEFT;
                n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_LEFT, 1);
            }
        }
        if (g->press == MTP_PRESS_PENDING) {
            g->pending_frames++;
            if (motion_valid) {
                g->press_travel += best_len;
            }
            g->force_frames = pmax >= g->config.force_threshold ? g->force_frames + 1 : 0;
            if (g->force_frames >= g->config.force_dwell_frames) {
                g->press = MTP_PRESS_FORCE;
                n = PushEvent(out, n, capacity, MTP_EVENT_FORCE, 0, 0, MTP_BUTTON_LEFT, 1);
            } else if (g->press_travel >= g->config.drag_start_units ||
                       g->pending_frames >= g->config.force_window_frames) {
                g->press = MTP_PRESS_LEFT;
                n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_LEFT, 1);
            }
        }
    } else if (g->press != MTP_PRESS_NONE) {
        switch (g->press) {
        case MTP_PRESS_PENDING: /* 見極め中に離した → 通常の左クリック */
            n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_LEFT, 1);
            n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_LEFT, 0);
            break;
        case MTP_PRESS_LEFT:
            n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_LEFT, 0);
            break;
        case MTP_PRESS_RIGHT:
            n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_RIGHT, 0);
            break;
        default: /* Force Touch: 左ボタンは送っていないので何もしない */
            break;
        }
        g->press = MTP_PRESS_NONE;
    }

    /* 3・4 本指のスワイプ */
    if (count >= 3 && (g->press == MTP_PRESS_NONE || g->press == MTP_PRESS_IGNORED)) {
        if (!g->multi_active) {
            g->multi_active = 1;
            g->multi_fired = 0;
            g->multi_max_fingers = count;
            g->multi_dx = g->multi_dy = 0;
        }
        if (count > g->multi_max_fingers) {
            g->multi_max_fingers = count;
        }
        if (motion_valid) {
            g->multi_dx += sum_dx / (float)count;
            g->multi_dy += sum_dy / (float)count;
        }
        if (g->config.swipe_enabled && !g->multi_fired) {
            float ax = fabsf(g->multi_dx);
            float ay = fabsf(g->multi_dy);
            int fire = 0;
            MTP_DIRECTION dir = MTP_DIR_LEFT;
            if (ax >= g->config.swipe_units && ax >= ay * g->config.swipe_axis_ratio) {
                fire = 1;
                dir = g->multi_dx < 0 ? MTP_DIR_LEFT : MTP_DIR_RIGHT;
            } else if (ay >= g->config.swipe_units && ay >= ax * g->config.swipe_axis_ratio) {
                fire = 1;
                dir = g->multi_dy < 0 ? MTP_DIR_UP : MTP_DIR_DOWN; /* y は下（手前）が正 */
            }
            if (fire && n < capacity) {
                memset(&out[n], 0, sizeof(out[n]));
                out[n].type = MTP_EVENT_SWIPE;
                out[n].fingers = g->multi_max_fingers >= 4 ? 4 : 3;
                out[n].direction = dir;
                n++;
                g->multi_fired = 1;
            }
        }
    } else if (count < 3 && g->multi_active) {
        g->multi_active = 0;
        g->multi_cooldown = 1;
    }
    if (count == 0) {
        g->multi_cooldown = 0;
    }

    /* 動き */
    if (motion_valid && !g->multi_cooldown && !g->multi_active &&
        g->press != MTP_PRESS_PENDING && g->press != MTP_PRESS_FORCE) {
        int pointer = 0;
        float pdx = 0, pdy = 0;

        if (g->press == MTP_PRESS_LEFT || g->press == MTP_PRESS_RIGHT) {
            pointer = 1;
            pdx = best_dx;
            pdy = best_dy;
        } else if (count == 1) {
            pointer = 1;
            pdx = sum_dx;
            pdy = sum_dy;
        } else if (count == 2) {
            /*
             * Windows のホイール: 縦は正で上へ、横は正で右へスクロール。
             * ナチュラル（内容が指と同じ向きに動く）:
             *   指を上へ（y が減る）→ 下へスクロール（縦は負）→ 縦は y の変化と同じ符号
             *   指を右へ（x が増える）→ 左へスクロール（横は負）→ 横は x の変化と逆の符号
             * 従来方向はどちらも逆。
             */
            float adx = sum_dx / 2.0f;
            float ady = sum_dy / 2.0f;
            float vsign = g->config.natural_scroll ? 1.0f : -1.0f;
            float hsign = -vsign;
            int h = Take(&g->scroll_rem_x, hsign * adx * g->config.scroll_gain);
            int v = Take(&g->scroll_rem_y, vsign * ady * g->config.scroll_gain);
            if (h != 0 || v != 0) {
                n = PushEvent(out, n, capacity, MTP_EVENT_SCROLL, h, v, MTP_BUTTON_LEFT, 0);
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
                n = PushEvent(out, n, capacity, MTP_EVENT_MOVE, ix, iy, MTP_BUTTON_LEFT, 0);
            }
        }
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
    MTP_BUTTON button;

    if (g == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    if (g->press == MTP_PRESS_LEFT) {
        button = MTP_BUTTON_LEFT;
    } else if (g->press == MTP_PRESS_RIGHT) {
        button = MTP_BUTTON_RIGHT;
    } else {
        g->press = MTP_PRESS_NONE; /* 見極め中・Force Touch 中は送ったボタンがない */
        return 0;
    }
    g->press = MTP_PRESS_NONE;
    return PushEvent(out, 0, capacity, MTP_EVENT_BUTTON, 0, 0, button, 0);
}
