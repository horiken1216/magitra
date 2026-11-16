/*
 * mtp_actions.c - ジェスチャーに割り当てた操作を SendInput で実行する
 */
#include "mtp_actions.h"

#include <string.h>

#include "mtp_input.h"

MTP_ACTION MtpActionForSwipe(int fingers, MTP_DIRECTION direction)
{
    if (fingers == 3) {
        switch (direction) {
        case MTP_DIR_LEFT: return MTP_ACTION_DESKTOP_NEXT;  /* Mac と同じ: 指を左へ払うと右隣へ */
        case MTP_DIR_RIGHT: return MTP_ACTION_DESKTOP_PREV;
        case MTP_DIR_UP: return MTP_ACTION_TASK_VIEW;       /* Mission Control に相当 */
        case MTP_DIR_DOWN: return MTP_ACTION_SHOW_DESKTOP;
        default: return MTP_ACTION_NONE;
        }
    }
    if (fingers == 4) {
        switch (direction) {
        case MTP_DIR_LEFT:
        case MTP_DIR_RIGHT: return MTP_ACTION_APP_SWITCH;
        default: return MTP_ACTION_NONE; /* 4 本指の上下はランチャー用に空けておく */
        }
    }
    return MTP_ACTION_NONE;
}

const char *MtpActionName(MTP_ACTION action)
{
    switch (action) {
    case MTP_ACTION_DESKTOP_NEXT: return "右隣の仮想デスクトップ";
    case MTP_ACTION_DESKTOP_PREV: return "左隣の仮想デスクトップ";
    case MTP_ACTION_TASK_VIEW: return "タスクビュー";
    case MTP_ACTION_SHOW_DESKTOP: return "デスクトップの表示";
    case MTP_ACTION_APP_SWITCH: return "直前のアプリに切り替え";
    case MTP_ACTION_MIDDLE_CLICK: return "中クリック";
    default: return "なし";
    }
}

static int IsExtendedKey(WORD vk)
{
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_LWIN || vk == VK_RWIN;
}

/* keys を順に押し、逆順に離す（同時押し） */
static BOOL SendChord(const WORD *keys, UINT count)
{
    INPUT in[8];
    UINT i, n = 0;

    if (count == 0 || count > 4) {
        return FALSE;
    }
    memset(in, 0, sizeof(in));
    for (i = 0; i < count; i++, n++) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = keys[i];
        in[n].ki.dwFlags = IsExtendedKey(keys[i]) ? KEYEVENTF_EXTENDEDKEY : 0;
    }
    for (i = count; i > 0; i--, n++) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = keys[i - 1];
        in[n].ki.dwFlags = KEYEVENTF_KEYUP | (IsExtendedKey(keys[i - 1]) ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    return SendInput(n, in, sizeof(INPUT)) == n;
}

BOOL MtpActionRun(MTP_ACTION action)
{
    switch (action) {
    case MTP_ACTION_DESKTOP_NEXT: {
        static const WORD k[] = { VK_LCONTROL, VK_LWIN, VK_RIGHT };
        return SendChord(k, 3);
    }
    case MTP_ACTION_DESKTOP_PREV: {
        static const WORD k[] = { VK_LCONTROL, VK_LWIN, VK_LEFT };
        return SendChord(k, 3);
    }
    case MTP_ACTION_TASK_VIEW: {
        static const WORD k[] = { VK_LWIN, VK_TAB };
        return SendChord(k, 2);
    }
    case MTP_ACTION_SHOW_DESKTOP: {
        static const WORD k[] = { VK_LWIN, 'D' };
        return SendChord(k, 2);
    }
    case MTP_ACTION_APP_SWITCH: {
        static const WORD k[] = { VK_LMENU, VK_TAB };
        return SendChord(k, 2);
    }
    case MTP_ACTION_MIDDLE_CLICK:
        return MtpInputForceAction();
    default:
        return TRUE;
    }
}
