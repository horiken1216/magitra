/*
 * binding.h - 操作に割り当てるキーの書式（"ctrl+alt+right" か "exec:<コマンド>"）の解析
 *
 * /dev にも Linux のヘッダーにも依存しないので、どこでも単体テストできる。
 * キーコードは Linux の input-event-codes.h の値（ABI として固定）。
 */
#ifndef MAGITRA_BINDING_H
#define MAGITRA_BINDING_H

#include <stddef.h>
#include <stdint.h>

#define MAGITRA_BINDING_MAX_KEYS 6
#define MAGITRA_BINDING_MAX_LEN  256

typedef enum MAGITRA_BINDING_TYPE {
    MAGITRA_BINDING_NONE = 0,
    MAGITRA_BINDING_KEYS = 1,
    MAGITRA_BINDING_EXEC = 2
} MAGITRA_BINDING_TYPE;

typedef struct MAGITRA_BINDING {
    MAGITRA_BINDING_TYPE type;
    int nkeys;
    uint16_t keys[MAGITRA_BINDING_MAX_KEYS]; /* 押す順。離すのは逆順 */
    char command[MAGITRA_BINDING_MAX_LEN];   /* type == EXEC */
} MAGITRA_BINDING;

#define MAGITRA_KEY_LEFTCTRL  29
#define MAGITRA_KEY_LEFTSHIFT 42
#define MAGITRA_KEY_LEFTALT   56
#define MAGITRA_KEY_LEFTMETA  125

/* 解析する。書式が正しければ 0、そうでなければ -1（b は NONE） */
int MagitraBindingParse(const char *text, MAGITRA_BINDING *b);

/* 解析できるキーコードの一覧（uinput のキーボードに登録する）。i が範囲外なら 0 */
uint16_t MagitraBindingKeyAt(size_t i);

#endif /* MAGITRA_BINDING_H */
