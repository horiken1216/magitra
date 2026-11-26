/*
 * uinput.h - 仮想のポインターとキーボード（/dev/uinput）への入力注入
 */
#ifndef MAGITRA_UINPUT_H
#define MAGITRA_UINPUT_H

#include <stdint.h>

#include "mtp_gesture.h"

typedef struct MAGITRA_UINPUT {
    int ptr_fd;
    int kbd_fd;
    int rem_v, rem_h;   /* 120 に満たないホイールの端数 */
    int ctrl_held;      /* ピンチ中に押している Ctrl */
} MAGITRA_UINPUT;

/* 2 つの仮想デバイスを作る。成功なら 0 */
int UinputOpen(MAGITRA_UINPUT *u);

/* 押しているキー・ボタンを離してから破棄する */
void UinputClose(MAGITRA_UINPUT *u);

void UinputMove(MAGITRA_UINPUT *u, int dx, int dy);
void UinputButton(MAGITRA_UINPUT *u, MTP_BUTTON button, int down);

/* ホイール単位（120 = 1 ノッチ）。dy は正で上、dx は正で右 */
void UinputScroll(MAGITRA_UINPUT *u, int dx, int dy);

/* ピンチ: Ctrl を押したままにしてホイールを回す / Ctrl を離す */
void UinputZoom(MAGITRA_UINPUT *u, int dy);
void UinputZoomEnd(MAGITRA_UINPUT *u);

/* キーを 1 つ押す/離す（そのたびに SYN を送る） */
void UinputKey(MAGITRA_UINPUT *u, uint16_t code, int down);

#endif /* MAGITRA_UINPUT_H */
