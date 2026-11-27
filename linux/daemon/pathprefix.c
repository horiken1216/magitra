/*
 * pathprefix.c - sysfs パスの包含判定
 */
#include "pathprefix.h"

#include <string.h>

int MagitraPathUnder(const char *path, const char *prefix)
{
    size_t n;
    if (path == NULL || prefix == NULL || *prefix == '\0') return 0;
    n = strlen(prefix);
    while (n > 1 && prefix[n - 1] == '/') n--; /* 末尾の '/' は無視（"/" だけは残す） */
    if (strncmp(path, prefix, n) != 0) return 0;
    return path[n] == '\0' || path[n] == '/' || n == 1;
}
