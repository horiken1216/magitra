/*
 * mtp_momentum.h - 慣性スクロール（指を離した後、だんだん遅くなりながら続くスクロール）
 *
 * OS に依存しない純粋なロジック。MTP_EVENT_FLING の速さで始め、時間を渡すたびに、その間に進む量を返す。
 * 速さは v(t) = v0 * exp(-t / tau) で落ちる。進む量はこれを積分したもの（呼ぶ間隔に左右されない）。
 * 単位: 速さはホイール単位/秒（120 = 1 ノッチ）、時間はミリ秒。
 */
#ifndef MTP_MOMENTUM_H
#define MTP_MOMENTUM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MTP_MOMENTUM {
    float tau_ms;     /* 速さが 1/e になるまでの時間。大きいほど長く滑る */
    float min_speed;  /* これより遅くなったら止める（ホイール単位/秒） */
    float max_speed;  /* 始めの速さの上限 */

    int   active;
    float vx, vy;
    float rem_x, rem_y;
} MTP_MOMENTUM;

void MtpMomentumInit(MTP_MOMENTUM *m);

/* 速さ (vx, vy) で始める（MTP_EVENT_SCROLL と同じ符号）。遅すぎれば始めない */
void MtpMomentumStart(MTP_MOMENTUM *m, float vx, float vy);

/* 止める（指が触れたとき・一時停止など） */
void MtpMomentumStop(MTP_MOMENTUM *m);

/*
 * dt_ms だけ時間を進め、その間に進む量（整数のホイール単位）を *h, *v に入れる。端数は次へ持ち越す。
 * 戻り値: まだ続いていれば 1、止まったら 0
 */
int MtpMomentumStep(MTP_MOMENTUM *m, float dt_ms, int *h, int *v);

#ifdef __cplusplus
}
#endif

#endif /* MTP_MOMENTUM_H */
