/*
 * mtp_input.c - イベントを SendInput の INPUT に変換して送る
 */
#include "mtp_input.h"

#include <string.h>

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
    default:
        break;
    }
    if (n == 0) {
        return TRUE;
    }
    return SendInput(n, in, sizeof(INPUT)) == n;
}
