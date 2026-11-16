/*
 * mtp_actions.h - ジェスチャーに割り当てる操作（キー操作・クリック）と、その既定の割り当て
 *
 * 割り当ては MtpActionForSwipe の表 1 か所で決める。設定画面ができたら、ここを設定値から引くように変える。
 */
#ifndef MTP_ACTIONS_H
#define MTP_ACTIONS_H

#include <windows.h>

#include "mtp_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MTP_ACTION {
    MTP_ACTION_NONE = 0,
    MTP_ACTION_DESKTOP_NEXT,  /* 右隣の仮想デスクトップへ（Ctrl+Win+→） */
    MTP_ACTION_DESKTOP_PREV,  /* 左隣の仮想デスクトップへ（Ctrl+Win+←） */
    MTP_ACTION_TASK_VIEW,     /* タスクビュー（Win+Tab） */
    MTP_ACTION_SHOW_DESKTOP,  /* デスクトップの表示（Win+D） */
    MTP_ACTION_APP_SWITCH,    /* 直前のアプリに切り替え（Alt+Tab） */
    MTP_ACTION_MIDDLE_CLICK   /* 中クリック */
} MTP_ACTION;

/* 既定の割り当て（Mac の操作に近いものを Windows の機能に対応させた） */
MTP_ACTION MtpActionForSwipe(int fingers, MTP_DIRECTION direction);

/* 画面表示・記録用の名前 */
const char *MtpActionName(MTP_ACTION action);

/* 操作を実行する（SendInput）。成功なら TRUE */
BOOL MtpActionRun(MTP_ACTION action);

#ifdef __cplusplus
}
#endif

#endif /* MTP_ACTIONS_H */
