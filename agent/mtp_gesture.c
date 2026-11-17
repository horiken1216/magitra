/*
 * mtp_gesture.c - フレーム列から入力イベントを作る（OS 非依存）
 *
 * 判定の規則:
 * - 指として数えるのは、状態ビットが 1（触れ始め）か 2（接触中）で、touch_major >= min_touch_major の接触
 *   （実機で 0/3 は離れる途中、接触サイズ 0 と確認済み。docs/research/FINDINGS.md）
 * - 物理クリックの押下時に指がちょうど 2 本なら右ボタン。離すまで右ボタンを保つ
 * - 3 本以上で押し込んだときはクリックを送らない（払う途中の押し込み。実機で確認、2026-11-16）
 * - 1 本指の押下は、Force Touch が有効なら見極め（MTP_PRESS_PENDING）に入る（mtp_gesture.h の説明を参照）。
 *   実機では、強く押すと押し始めから 2〜34ms で圧力が 200 を超え、普通のクリックは最大 156 だった（2026-11-16）
 * - 指の組み合わせ（id の集合）が前フレームと変わったフレームでは動きを出さない（置いた・離した瞬間の飛びを防ぐ）
 * - 見極め中と Force Touch 中は、ポインターもスクロールも出さない
 * - ボタンを押している間: 一番大きく動いた指でポインターを動かす（クリックしている指は止まっているため）
 * - ボタンを押していない間: 1 本ならポインター移動、2 本ならスクロールかピンチ、3 本以上はスワイプの判定だけ
 * - 2 本指: 間隔の変化が中心の移動より大きければピンチ（拡大縮小）、中心が先に大きく動けばスクロールに決まる。
 *   スクロール中に離すと、直前の速さで慣性スクロール（MTP_EVENT_FLING）。スクロール・ピンチの後は、
 *   指がすべて離れるまでポインターを動かさない（片方の指が先に離れたときの動きを拾わない）
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
    config->inertia_enabled = 1;
    config->inertia_min_speed = 800.0f;  /* ホイール単位/秒（約 7 ノッチ/秒） */
    config->inertia_idle_frames = 3;     /* 約 33ms */
    config->pinch_enabled = 1;
    config->pinch_units = 250.0f;        /* 約 5mm */
    config->scroll_lock_units = 300.0f;  /* 約 6mm */
    config->pinch_gain = 350.0f;         /* 間隔が 2 倍で約 240（2 ノッチ） */
    config->double_tap_enabled = 1;
    config->tap_max_ms = 250.0f;
    config->tap_max_units = 150.0f;      /* 約 3mm */
    config->double_tap_ms = 400.0f;
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

/* 2 本の指の間隔（count がちょうど 2 のときだけ意味がある） */
static float TwoFingerDistance(const int *valid, const int16_t *x, const int16_t *y)
{
    int i, k = 0;
    float px[2] = { 0, 0 }, py[2] = { 0, 0 };
    for (i = 0; i < MTP_MAX_CONTACTS && k < 2; i++) {
        if (valid[i]) {
            px[k] = (float)x[i];
            py[k] = (float)y[i];
            k++;
        }
    }
    if (k < 2) {
        return 0;
    }
    return sqrtf((px[1] - px[0]) * (px[1] - px[0]) + (py[1] - py[0]) * (py[1] - py[0]));
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
    if (g == NULL) {
        return 0;
    }
    g->frame_clock_ms += 1000.0 / MTP_FRAMES_PER_SECOND;
    return MtpGestureProcessAt(g, frame, g->frame_clock_ms, out, capacity);
}

size_t MtpGestureProcessAt(MTP_GESTURE *g, const MTP_FRAME *frame, double now_ms, MTP_EVENT *out, size_t capacity)
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

    g->now_ms = now_ms;
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

    /* 慣性スクロールの後の触れ始め（止める合図） */
    if (count > 0 && g->fling_sent) {
        g->fling_sent = 0;
        n = PushEvent(out, n, capacity, MTP_EVENT_TOUCH, 0, 0, MTP_BUTTON_LEFT, 0);
    }

    /* 2 本指の動作の終わり（指の組み合わせが変わった） */
    if (g->two_active && (count != 2 || mask != g->two_mask)) {
        if (g->two_mode == 1) {
            n = PushEvent(out, n, capacity, MTP_EVENT_ZOOM_END, 0, 0, MTP_BUTTON_LEFT, 0);
        } else if (g->config.inertia_enabled && count < 2 && g->fling_count >= 3 &&
                   g->fling_idle <= g->config.inertia_idle_frames) {
            int k, samples = g->fling_count < MTP_FLING_SAMPLES ? g->fling_count : MTP_FLING_SAMPLES;
            float h = 0, v = 0, speed;
            for (k = 0; k < samples; k++) {
                h += g->fling_h[k];
                v += g->fling_v[k];
            }
            h = h / (float)samples * MTP_FRAMES_PER_SECOND;
            v = v / (float)samples * MTP_FRAMES_PER_SECOND;
            speed = sqrtf(h * h + v * v);
            if (speed >= g->config.inertia_min_speed) {
                n = PushEvent(out, n, capacity, MTP_EVENT_FLING, (int)h, (int)v, MTP_BUTTON_LEFT, 0);
                g->fling_sent = (count == 0); /* 1 本残っているなら、その指で触れているので止める合図は要らない */
            }
        }
        if (count > 0 && (g->two_mode == 1 || g->two_travel >= 30.0f)) { /* 約 0.6mm。置いただけなら止めない */
            g->two_cooldown = 1;
        }
        g->two_active = 0;
    }
    if (count == 0) {
        g->two_cooldown = 0;
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

    /* タップの判定（触れてから全部離すまでを 1 回とみなす） */
    if (count > 0) {
        if (!g->seq_active) {
            g->seq_active = 1;
            g->seq_start_ms = now_ms;
            g->seq_max_fingers = 0;
            g->seq_clicked = 0;
            g->seq_travel = 0;
            if (g->tap_pending && now_ms - g->tap_end_ms > g->config.double_tap_ms) {
                g->tap_pending = 0; /* 1 回目から間が空きすぎた */
            }
        }
        if (count > g->seq_max_fingers) g->seq_max_fingers = count;
        if (frame->button) g->seq_clicked = 1;
        if (motion_valid) g->seq_travel += sqrtf(sum_dx * sum_dx + sum_dy * sum_dy) / (float)count;
    } else if (g->seq_active) {
        int tap = g->config.double_tap_enabled && g->seq_max_fingers == 2 && !g->seq_clicked &&
            now_ms - g->seq_start_ms <= g->config.tap_max_ms && g->seq_travel <= g->config.tap_max_units;
        g->seq_active = 0;
        if (!tap) {
            g->tap_pending = 0;
        } else if (g->tap_pending && now_ms - g->tap_end_ms <= g->config.double_tap_ms) {
            g->tap_pending = 0;
            if (n < capacity) {
                memset(&out[n], 0, sizeof(out[n]));
                out[n].type = MTP_EVENT_DOUBLE_TAP;
                out[n].fingers = 2;
                n++;
            }
        } else {
            g->tap_pending = 1;
            g->tap_end_ms = now_ms;
        }
    }

    /* 2 本指の動作の始まり */
    if (count == 2 && !g->two_active && g->press == MTP_PRESS_NONE && !g->multi_cooldown && !g->multi_active) {
        g->two_active = 1;
        g->two_mask = mask;
        g->two_mode = 0;
        g->two_start_dist = g->two_prev_dist = TwoFingerDistance(cur_valid, cur_x, cur_y);
        g->two_travel = 0;
        g->fling_count = 0;
        g->fling_idle = 0;
        g->zoom_rem = 0;
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
            pointer = !g->two_cooldown;
            pdx = sum_dx;
            pdy = sum_dy;
        } else if (count == 2 && g->two_active && g->two_mode == 1) {
            /* ピンチ: 間隔の比の対数に比例（同じ比なら、どの間隔からでも同じだけ拡大縮小する） */
            float d = TwoFingerDistance(cur_valid, cur_x, cur_y);
            if (d > 1.0f && g->two_prev_dist > 1.0f) {
                int z = Take(&g->zoom_rem, g->config.pinch_gain * logf(d / g->two_prev_dist));
                g->two_prev_dist = d;
                if (z != 0) {
                    n = PushEvent(out, n, capacity, MTP_EVENT_ZOOM, 0, z, MTP_BUTTON_LEFT, 0);
                }
            }
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
            float hraw = hsign * adx * g->config.scroll_gain;
            float vraw = vsign * ady * g->config.scroll_gain;
            int h, v;
            int pinching = 0;

            if (g->two_active) {
                g->two_travel += sqrtf(adx * adx + ady * ady);
                if (g->two_mode == 0) {
                    float d = TwoFingerDistance(cur_valid, cur_x, cur_y);
                    float change = fabsf(d - g->two_start_dist);
                    if (g->config.pinch_enabled && change >= g->config.pinch_units && change >= g->two_travel) {
                        g->two_mode = 1; /* ここから拡大縮小。このフレームのスクロールは出さない */
                        g->two_prev_dist = d;
                        g->zoom_rem = 0;
                        pinching = 1;
                    } else if (g->two_travel >= g->config.scroll_lock_units) {
                        g->two_mode = 2;
                    }
                }
                if (!pinching) {
                    /* 慣性スクロール用に、最近のフレームの量を覚える（輪のように上書き） */
                    g->fling_h[g->fling_count % MTP_FLING_SAMPLES] = hraw;
                    g->fling_v[g->fling_count % MTP_FLING_SAMPLES] = vraw;
                    g->fling_count++;
                    g->fling_idle = (fabsf(hraw) + fabsf(vraw) < 0.5f) ? g->fling_idle + 1 : 0;
                }
            }
            if (!pinching) {
                h = Take(&g->scroll_rem_x, hraw);
                v = Take(&g->scroll_rem_y, vraw);
                if (h != 0 || v != 0) {
                    n = PushEvent(out, n, capacity, MTP_EVENT_SCROLL, h, v, MTP_BUTTON_LEFT, 0);
                }
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
    size_t n = 0;

    if (g == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    if (g->two_active && g->two_mode == 1) {
        n = PushEvent(out, n, capacity, MTP_EVENT_ZOOM_END, 0, 0, MTP_BUTTON_LEFT, 0);
    }
    g->two_active = 0;
    if (g->press == MTP_PRESS_LEFT) {
        n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_LEFT, 0);
    } else if (g->press == MTP_PRESS_RIGHT) {
        n = PushEvent(out, n, capacity, MTP_EVENT_BUTTON, 0, 0, MTP_BUTTON_RIGHT, 0);
    }
    g->press = MTP_PRESS_NONE; /* 見極め中・Force Touch 中は送ったボタンがない */
    return n;
}
