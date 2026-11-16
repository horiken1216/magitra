/*
 * replay_gesture.c - mtpagent-cli --log の記録を mtp_gesture に流し直し、作られるイベントを数える
 *
 * 実機で記録したフレームで判定の変更を確かめるための道具（追加の実機試験をせずに済ませる）。
 * 記録には id / state / touch_major / x / y / pressure と button があり、判定に必要な項目はそろっている。
 *
 * 使い方: replay_gesture LOGFILE [--no-force] [--verbose]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp_gesture.h"

static int ParseFrame(const char *line, MTP_FRAME *f, double *t_ms)
{
    unsigned n = 0, btn = 0;
    const char *p;
    int consumed = 0;

    if (sscanf_s(line, " %lf n=%u btn=%u%n", t_ms, &n, &btn, &consumed) != 3) {
        return 0;
    }
    memset(f, 0, sizeof(*f));
    f->button = (uint8_t)btn;
    p = line + consumed;
    while ((p = strstr(p, "| id=")) != NULL && f->contact_count < MTP_MAX_CONTACTS) {
        unsigned id, st, maj, pr;
        int x, y;
        if (sscanf_s(p, "| id=%u st=%u maj=%u x=%d y=%d p=%u", &id, &st, &maj, &x, &y, &pr) == 6) {
            MTP_CONTACT *c = &f->contacts[f->contact_count++];
            c->id = (uint8_t)id;
            c->state = (uint8_t)st;
            c->touch_major = (uint8_t)maj;
            c->x = (int16_t)x;
            c->y = (int16_t)y;
            c->pressure = (uint8_t)pr;
        }
        p += 5;
    }
    return 1;
}

int main(int argc, char **argv)
{
    FILE *fp;
    char line[4096];
    MTP_GESTURE g;
    MTP_GESTURE_CONFIG cfg;
    int verbose = 0;
    unsigned long frames = 0, moves = 0, scrolls = 0, forces = 0, swipes = 0;
    unsigned long left_down = 0, left_up = 0, right_down = 0, right_up = 0;
    int i;

    if (argc < 2) {
        printf("usage: replay_gesture LOGFILE [--no-force] [--verbose]\n");
        return 2;
    }
    MtpGestureDefaultConfig(&cfg);
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-force") == 0) cfg.force_enabled = 0;
        else if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
    }
    if (fopen_s(&fp, argv[1], "r") != 0) {
        printf("cannot open %s\n", argv[1]);
        return 2;
    }
    MtpGestureInit(&g, &cfg);
    while (fgets(line, sizeof(line), fp) != NULL) {
        MTP_FRAME f;
        MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
        double t;
        size_t n, k;
        if (!ParseFrame(line, &f, &t)) {
            continue;
        }
        frames++;
        n = MtpGestureProcess(&g, &f, ev, MTP_GESTURE_MAX_EVENTS);
        for (k = 0; k < n; k++) {
            switch (ev[k].type) {
            case MTP_EVENT_MOVE: moves++; break;
            case MTP_EVENT_SCROLL: scrolls++; break;
            case MTP_EVENT_FORCE:
                forces++;
                if (verbose) printf("%9.1f ms FORCE\n", t);
                break;
            case MTP_EVENT_SWIPE: {
                static const char *names[] = { "left", "right", "up", "down" };
                swipes++;
                printf("%9.1f ms SWIPE %d fingers %s\n", t, ev[k].fingers, names[ev[k].direction & 3]);
                break;
            }
            case MTP_EVENT_BUTTON:
                if (ev[k].button == MTP_BUTTON_RIGHT) { if (ev[k].down) right_down++; else right_up++; }
                else { if (ev[k].down) left_down++; else left_up++; }
                if (verbose) printf("%9.1f ms %s %s\n", t, ev[k].button == MTP_BUTTON_RIGHT ? "right" : "left", ev[k].down ? "down" : "up");
                break;
            default: break;
            }
        }
    }
    fclose(fp);
    printf("frames=%lu left=%lu/%lu right=%lu/%lu force=%lu swipe=%lu move=%lu scroll=%lu\n",
        frames, left_down, left_up, right_down, right_up, forces, swipes, moves, scrolls);
    return 0;
}
