/*
 * mtp_input.c - イベントを SendInput の INPUT に変換して送る
 */
#include "mtp_input.h"

#include <string.h>

/*
 * 拡大縮小は Ctrl + ホイールで送る（ブラウザー・Office・エクスプローラーなどが対応）。
 * Ctrl はピンチの間ずっと押したままにし、MTP_EVENT_ZOOM_END で離す。ホイールのたびに押し離しすると、
 * Ctrl の 2 度押しに反応する道具（PowerToys のマウス検索など）が動いてしまうため。
 * 利用者が自分で Ctrl を押しているときは、こちらからは押しも離しもしない。
 */
static LONG g_ctrlHeld = 0;

static BOOL SendCtrl(BOOL down)
{
    INPUT k;
    memset(&k, 0, sizeof(k));
    k.type = INPUT_KEYBOARD;
    k.ki.wVk = VK_CONTROL;
    k.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    return SendInput(1, &k, sizeof(INPUT)) == 1;
}

BOOL MtpInputSend(const MTP_EVENT *ev)
{
    INPUT in[2];
    UINT n = 0;

    memset(in, 0, sizeof(in));
    in[0].type = INPUT_MOUSE;
    in[1].type = INPUT_MOUSE;

    switch (ev->type) {
    case MTP_EVENT_MOVE:
        in[0].mi.dx = ev->dx;
        in[0].mi.dy = ev->dy;
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
        n = 1;
        break;
    case MTP_EVENT_BUTTON:
        if (ev->button == MTP_BUTTON_RIGHT) {
            in[0].mi.dwFlags = ev->down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        } else if (ev->button == MTP_BUTTON_MIDDLE) {
            in[0].mi.dwFlags = ev->down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        } else {
            in[0].mi.dwFlags = ev->down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        }
        n = 1;
        break;
    case MTP_EVENT_SCROLL:
        if (ev->dy != 0) {
            in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
            in[n].mi.mouseData = (DWORD)ev->dy;
            n++;
        }
        if (ev->dx != 0) {
            in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in[n].mi.mouseData = (DWORD)ev->dx;
            n++;
        }
        break;
    case MTP_EVENT_ZOOM:
        if (ev->dy == 0) {
            break;
        }
        if (!InterlockedCompareExchange(&g_ctrlHeld, 0, 0) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) {
            if (SendCtrl(TRUE)) {
                InterlockedExchange(&g_ctrlHeld, 1);
            }
        }
        in[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
        in[0].mi.mouseData = (DWORD)ev->dy;
        n = 1;
        break;
    case MTP_EVENT_ZOOM_END:
        if (InterlockedExchange(&g_ctrlHeld, 0)) {
            return SendCtrl(FALSE);
        }
        return TRUE;
    default:
        break;
    }
    if (n == 0) {
        return TRUE;
    }
    return SendInput(n, in, sizeof(INPUT)) == n;
}

BOOL MtpInputForceAction(void)
{
    /* Force Touch の割り当て: 中クリック（押下と解放） */
    MTP_EVENT down, up;
    memset(&down, 0, sizeof(down));
    down.type = MTP_EVENT_BUTTON;
    down.button = MTP_BUTTON_MIDDLE;
    down.down = 1;
    up = down;
    up.down = 0;
    return MtpInputSend(&down) && MtpInputSend(&up);
}
