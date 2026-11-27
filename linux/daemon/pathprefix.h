/*
 * pathprefix.h - sysfs パスの包含判定（OS に依存しないので単体テストできる）
 */
#ifndef MAGITRA_PATHPREFIX_H
#define MAGITRA_PATHPREFIX_H

/* path が prefix そのもの、または prefix の下なら 1。境界は '/' で見る（"/a/b" は "/a/bc" の親ではない） */
int MagitraPathUnder(const char *path, const char *prefix);

#endif /* MAGITRA_PATHPREFIX_H */
