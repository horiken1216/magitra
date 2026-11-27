/*
 * binding.c - キー割り当ての解析
 */
#include "binding.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

typedef struct KEY_NAME {
    const char *name;
    uint16_t code;
} KEY_NAME;

/* 名前は小文字。コードは linux/input-event-codes.h の KEY_* */
static const KEY_NAME kKeys[] = {
    { "ctrl", 29 }, { "alt", 56 }, { "shift", 42 }, { "super", 125 },
    { "a", 30 }, { "b", 48 }, { "c", 46 }, { "d", 32 }, { "e", 18 }, { "f", 33 }, { "g", 34 },
    { "h", 35 }, { "i", 23 }, { "j", 36 }, { "k", 37 }, { "l", 38 }, { "m", 50 }, { "n", 49 },
    { "o", 24 }, { "p", 25 }, { "q", 16 }, { "r", 19 }, { "s", 31 }, { "t", 20 }, { "u", 22 },
    { "v", 47 }, { "w", 17 }, { "x", 45 }, { "y", 21 }, { "z", 44 },
    { "1", 2 }, { "2", 3 }, { "3", 4 }, { "4", 5 }, { "5", 6 }, { "6", 7 }, { "7", 8 },
    { "8", 9 }, { "9", 10 }, { "0", 11 },
    { "f1", 59 }, { "f2", 60 }, { "f3", 61 }, { "f4", 62 }, { "f5", 63 }, { "f6", 64 },
    { "f7", 65 }, { "f8", 66 }, { "f9", 67 }, { "f10", 68 }, { "f11", 87 }, { "f12", 88 },
    { "left", 105 }, { "right", 106 }, { "up", 103 }, { "down", 108 },
    { "tab", 15 }, { "escape", 1 }, { "return", 28 }, { "space", 57 },
    { "pageup", 104 }, { "pagedown", 109 }, { "home", 102 }, { "end", 107 },
    { "playpause", 164 }, { "back", 158 }, { "forward", 159 },
};

#define KEY_COUNT (sizeof(kKeys) / sizeof(kKeys[0]))

uint16_t MagitraBindingKeyAt(size_t i)
{
    return i < KEY_COUNT ? kKeys[i].code : 0;
}

static int FindKey(const char *token, size_t len)
{
    size_t i;
    for (i = 0; i < KEY_COUNT; i++) {
        if (strlen(kKeys[i].name) == len && strncasecmp(kKeys[i].name, token, len) == 0) {
            return kKeys[i].code;
        }
    }
    return -1;
}

int MagitraBindingParse(const char *text, MAGITRA_BINDING *b)
{
    const char *p;

    memset(b, 0, sizeof(*b));
    if (text == NULL) {
        return -1;
    }
    while (isspace((unsigned char)*text)) text++;

    if (strncasecmp(text, "exec:", 5) == 0) {
        p = text + 5;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || strlen(p) >= sizeof(b->command)) {
            return -1;
        }
        strcpy(b->command, p);
        b->type = MAGITRA_BINDING_EXEC;
        return 0;
    }

    p = text;
    for (;;) {
        const char *end = strchr(p, '+');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        int code;
        while (len > 0 && isspace((unsigned char)p[len - 1])) len--;
        while (len > 0 && isspace((unsigned char)*p)) { p++; len--; }
        code = len > 0 ? FindKey(p, len) : -1;
        if (code < 0 || b->nkeys >= MAGITRA_BINDING_MAX_KEYS) {
            memset(b, 0, sizeof(*b));
            return -1;
        }
        b->keys[b->nkeys++] = (uint16_t)code;
        if (end == NULL) break;
        p = end + 1;
    }
    b->type = MAGITRA_BINDING_KEYS;
    return 0;
}
