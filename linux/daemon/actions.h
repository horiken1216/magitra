/*
 * actions.h - 操作（MAGITRA_ACTION）の実行: キー操作・クリック・コマンド
 */
#ifndef MAGITRA_ACTIONS_H
#define MAGITRA_ACTIONS_H

#include "settings.h"
#include "uinput.h"

/* exec: の割り当てとランチャーは実行せず、購読しているクライアントへ ACTION <n> として知らせる（main.c が実装） */
void ActionForward(int action);

/* キー操作を打つ前の確認。セッションが切り替わっていたら状態を初期化して 0 を返す（main.c が実装） */
int ActionSessionOk(void);

/* 操作を実行する。割り当てがない・書式が誤りなら記録して何もしない */
void ActionRun(const MAGITRA_SETTINGS *s, MAGITRA_UINPUT *u, int action);

#endif /* MAGITRA_ACTIONS_H */
