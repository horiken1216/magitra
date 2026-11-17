/*
 * mtp_actions.c - ジェスチャーに割り当てた操作を SendInput で実行する
 */
#include "mtp_actions.h"

#include <string.h>

#include "mtp_input.h"
#include "mtp_launcher.h"

const char *MtpActionName(MTP_ACTION action)
{
    switch (action) {
    case MTP_ACTION_DESKTOP_NEXT: return "右隣の仮想デスクトップ";
    case MTP_ACTION_DESKTOP_PREV: return "左隣の仮想デスクトップ";
    case MTP_ACTION_TASK_VIEW: return "タスクビュー";
    case MTP_ACTION_SHOW_DESKTOP: return "デスクトップの表示";
    case MTP_ACTION_APP_SWITCH: return "直前のアプリに切り替え";
    case MTP_ACTION_MIDDLE_CLICK: return "中クリック";
    case MTP_ACTION_BROWSER_BACK: return "戻る";
    case MTP_ACTION_BROWSER_FORWARD: return "進む";
    case MTP_ACTION_PLAY_PAUSE: return "再生・一時停止";
    case MTP_ACTION_SEARCH: return "検索";
    case MTP_ACTION_ZOOM_RESET: return "拡大率を元に戻す（Ctrl+0）";
    case MTP_ACTION_LAUNCHER: return "ランチャー";
    default: return "なし";
    }
}

static int IsExtendedKey(WORD vk)
{
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_LWIN || vk == VK_RWIN ||
        vk == VK_MEDIA_PLAY_PAUSE;
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
    case MTP_ACTION_BROWSER_BACK: {
        static const WORD k[] = { VK_LMENU, VK_LEFT };
        return SendChord(k, 2);
    }
    case MTP_ACTION_BROWSER_FORWARD: {
        static const WORD k[] = { VK_LMENU, VK_RIGHT };
        return SendChord(k, 2);
    }
    case MTP_ACTION_PLAY_PAUSE: {
        static const WORD k[] = { VK_MEDIA_PLAY_PAUSE };
        return SendChord(k, 1);
    }
    case MTP_ACTION_SEARCH: {
        static const WORD k[] = { VK_LWIN, 'S' };
        return SendChord(k, 2);
    }
    case MTP_ACTION_ZOOM_RESET: {
        static const WORD k[] = { VK_LCONTROL, '0' };
        return SendChord(k, 2);
    }
    case MTP_ACTION_LAUNCHER:
        return MtpLauncherShow();
    case MTP_ACTION_MIDDLE_CLICK:
        return MtpInputForceAction();
    default:
        return TRUE;
    }
}
