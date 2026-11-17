/*
 * mtp_actions.h - ジェスチャーに割り当てる操作（キー操作・クリック）
 *
 * 値は設定（HKCU\Software\MTP\Agent）に保存し、設定画面（settings/）とも共有する。
 * **既存の値は変えないこと**（保存済みの設定の意味が変わる）。追加は末尾に。
 */
#ifndef MTP_ACTIONS_H
#define MTP_ACTIONS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MTP_ACTION {
    MTP_ACTION_NONE = 0,
    MTP_ACTION_DESKTOP_NEXT = 1,   /* 右隣の仮想デスクトップへ（Ctrl+Win+→） */
    MTP_ACTION_DESKTOP_PREV = 2,   /* 左隣の仮想デスクトップへ（Ctrl+Win+←） */
    MTP_ACTION_TASK_VIEW = 3,      /* タスクビュー（Win+Tab） */
    MTP_ACTION_SHOW_DESKTOP = 4,   /* デスクトップの表示（Win+D） */
    MTP_ACTION_APP_SWITCH = 5,     /* 直前のアプリに切り替え（Alt+Tab） */
    MTP_ACTION_MIDDLE_CLICK = 6,   /* 中クリック */
    MTP_ACTION_BROWSER_BACK = 7,   /* 戻る（Alt+←） */
    MTP_ACTION_BROWSER_FORWARD = 8,/* 進む（Alt+→） */
    MTP_ACTION_PLAY_PAUSE = 9,     /* 再生・一時停止（メディアキー） */
    MTP_ACTION_SEARCH = 10,        /* 検索（Win+S） */
    MTP_ACTION_ZOOM_RESET = 11,    /* 拡大率を元に戻す（Ctrl+0。ブラウザーなど。Word・Excel では別の操作になる） */
    MTP_ACTION_LAUNCHER = 12,      /* ランチャー（mtplauncher.exe）を開く */
    MTP_ACTION_COUNT
} MTP_ACTION;

/* 画面表示・記録用の名前 */
const char *MtpActionName(MTP_ACTION action);

/* 操作を実行する（SendInput）。成功なら TRUE */
BOOL MtpActionRun(MTP_ACTION action);

#ifdef __cplusplus
}
#endif

#endif /* MTP_ACTIONS_H */
