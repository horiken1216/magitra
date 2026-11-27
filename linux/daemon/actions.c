/*
 * actions.c - 操作の実行
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "actions.h"

#include <stdio.h>

#include "binding.h"

static void RunBinding(MAGITRA_UINPUT *u, const char *text, int action)
{
    MAGITRA_BINDING b;
    int i;

    if (text == NULL || MagitraBindingParse(text, &b) != 0) {
        fprintf(stderr, "magitrad: 操作 %d の割り当てを解釈できません: %s\n", action, text ? text : "(なし)");
        return;
    }
    if (b.type == MAGITRA_BINDING_EXEC) {
        ActionForward(action); /* コマンドはデーモンでは実行せず、ユーザー側のトレイに任せる */
        return;
    }
    if (!ActionSessionOk()) return;
    if (g_log) fprintf(stderr, "magitrad: 操作 %d: キー %s を送ります\n", action, text);
    for (i = 0; i < b.nkeys; i++) UinputKey(u, b.keys[i], 1);
    for (i = b.nkeys - 1; i >= 0; i--) UinputKey(u, b.keys[i], 0);
}

void ActionRun(const MAGITRA_SETTINGS *s, MAGITRA_UINPUT *u, int action)
{
    switch (action) {
    case MAGITRA_ACTION_NONE:
        break;
    case MAGITRA_ACTION_MIDDLE_CLICK:
        UinputButton(u, MTP_BUTTON_MIDDLE, 1);
        UinputButton(u, MTP_BUTTON_MIDDLE, 0);
        break;
    case MAGITRA_ACTION_LAUNCHER:
        ActionForward(action);
        break;
    default:
        RunBinding(u, MagitraSettingsBinding(s, action), action);
        break;
    }
}
