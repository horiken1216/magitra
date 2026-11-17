/*
 * mtp_momentum.c - 慣性スクロール（OS 非依存）
 */
#include "mtp_momentum.h"

#include <math.h>
#include <string.h>

void MtpMomentumInit(MTP_MOMENTUM *m)
{
    memset(m, 0, sizeof(*m));
    m->tau_ms = 400.0f;      /* 始めの速さで約 0.4 秒分の距離を滑る */
    m->min_speed = 60.0f;    /* 0.5 ノッチ/秒 */
    m->max_speed = 12000.0f; /* 100 ノッチ/秒 */
}

void MtpMomentumStart(MTP_MOMENTUM *m, float vx, float vy)
{
    float speed = sqrtf(vx * vx + vy * vy);
    if (speed < m->min_speed) {
        MtpMomentumStop(m);
        return;
    }
    if (speed > m->max_speed) {
        vx = vx * m->max_speed / speed;
        vy = vy * m->max_speed / speed;
    }
    m->vx = vx;
    m->vy = vy;
    m->rem_x = m->rem_y = 0;
    m->active = 1;
}

void MtpMomentumStop(MTP_MOMENTUM *m)
{
    m->active = 0;
    m->vx = m->vy = 0;
    m->rem_x = m->rem_y = 0;
}

static int Take(float *remainder, float value)
{
    float total = *remainder + value;
    int whole = (int)total;
    *remainder = total - (float)whole;
    return whole;
}

int MtpMomentumStep(MTP_MOMENTUM *m, float dt_ms, int *h, int *v)
{
    float k, dist;
    *h = 0;
    *v = 0;
    if (!m->active || dt_ms <= 0) {
        return m->active;
    }
    /* dt の間に進む量 = v * tau * (1 - e^(-dt/tau))（秒に直す） */
    k = expf(-dt_ms / m->tau_ms);
    dist = m->tau_ms / 1000.0f * (1.0f - k);
    *h = Take(&m->rem_x, m->vx * dist);
    *v = Take(&m->rem_y, m->vy * dist);
    m->vx *= k;
    m->vy *= k;
    if (sqrtf(m->vx * m->vx + m->vy * m->vy) < m->min_speed) {
        m->active = 0;
    }
    return m->active;
}
