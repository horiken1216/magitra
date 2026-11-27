/*
 * device.c - デバイス探索と hidraw / input の操作（libudev は使わず sysfs を直接読む）
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "device.h"
#include "pathprefix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_CANDIDATES 32

typedef struct CANDIDATE {
    int num;                 /* hidrawN の N */
    int bus, vid, pid;
    int ifnum;               /* USB のインターフェイス番号（BT は -1） */
    char usbdev[PATH_MAX];   /* USB: インターフェイスの親（同じ機器か見分ける） */
    char sysdir[PATH_MAX];   /* USB: usbdev、BT: HID デバイスのディレクトリ */
} CANDIDATE;

static long long NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void DeviceInit(MAGITRA_DEVICE *d)
{
    int i;
    memset(d, 0, sizeof(*d));
    d->mt_fd = -1;
    d->haptic_fd = -1;
    for (i = 0; i < DEVICE_MAX_GRABS; i++) {
        d->grab_fd[i] = -1;
        d->grab_num[i] = -1;
    }
}

static int ReadHex(const char *path, int *value)
{
    char buf[32];
    FILE *fp = fopen(path, "r");
    int ok;
    if (fp == NULL) return -1;
    ok = fgets(buf, sizeof(buf), fp) != NULL && sscanf(buf, "%x", value) == 1;
    fclose(fp);
    return ok ? 0 : -1;
}

/* /sys/class/hidraw を調べて、対応するトラックパッドの hidraw を集める */
static int Collect(CANDIDATE *out, int max)
{
    DIR *dir = opendir("/sys/class/hidraw");
    struct dirent *ent;
    int n = 0;

    if (dir == NULL) return 0;
    while (n < max && (ent = readdir(dir)) != NULL) {
        char link[PATH_MAX], hiddir[PATH_MAX], parent[PATH_MAX];
        unsigned bus, vid, pid;
        int num;
        const char *base, *slash;
        CANDIDATE *c;

        if (sscanf(ent->d_name, "hidraw%d", &num) != 1) continue;
        snprintf(link, sizeof(link), "/sys/class/hidraw/%s/device", ent->d_name);
        if (realpath(link, hiddir) == NULL) continue;
        slash = strrchr(hiddir, '/');
        base = slash ? slash + 1 : hiddir;
        if (sscanf(base, "%x:%x:%x.", &bus, &vid, &pid) != 3) continue;
        if (!MtpIsSupportedProduct((uint16_t)pid)) continue;
        if (!((bus == 0x0003 && vid == MTP_VID_APPLE_USB) ||
              (bus == 0x0005 && (vid == MTP_VID_APPLE_BT || vid == MTP_VID_APPLE_USB)))) {
            continue;
        }

        c = &out[n];
        memset(c, 0, sizeof(*c));
        c->num = num;
        c->bus = (int)bus;
        c->vid = (int)vid;
        c->pid = (int)pid;
        c->ifnum = -1;
        snprintf(c->sysdir, sizeof(c->sysdir), "%s", hiddir);
        if (bus == 0x0003) {
            /* HID の親が USB インターフェイス "<port>:<cfg>.<ifnum>"、その親が機器 */
            const char *dot;
            snprintf(parent, sizeof(parent), "%s", hiddir);
            *strrchr(parent, '/') = '\0';
            slash = strrchr(parent, '/');
            dot = slash ? strrchr(slash, '.') : NULL;
            if (dot == NULL) continue;
            c->ifnum = atoi(dot + 1);
            *strrchr(parent, '/') = '\0';
            snprintf(c->usbdev, sizeof(c->usbdev), "%s", parent);
            snprintf(c->sysdir, sizeof(c->sysdir), "%s", parent);
        }
        n++;
    }
    closedir(dir);
    return n;
}

static int OpenHidraw(int num)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/hidraw%d", num);
    return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

static void SendEnable(MAGITRA_DEVICE *d)
{
    const uint8_t *report;
    size_t length;
    uint8_t buf[64];

    if (MtpGetEnableReport(d->transport, &report, &length) == MTP_OK && length <= sizeof(buf)) {
        memcpy(buf, report, length);
        if (ioctl(d->mt_fd, HIDIOCSFEATURE(length), buf) < 0) {
            fprintf(stderr, "magitrad: 有効化レポートを送れませんでした: %s\n", strerror(errno));
        }
    }
}

int DeviceOpen(MAGITRA_DEVICE *d)
{
    CANDIDATE cand[MAX_CANDIDATES];
    int n = Collect(cand, MAX_CANDIDATES);
    const CANDIDATE *mt = NULL, *act = NULL;
    int i;

    /* USB を優先。マルチタッチは interface 1、振動は同じ機器の interface 2 */
    for (i = 0; i < n && mt == NULL; i++) {
        if (cand[i].bus == 0x0003 && cand[i].ifnum == 1) mt = &cand[i];
    }
    if (mt != NULL) {
        for (i = 0; i < n; i++) {
            if (cand[i].bus == 0x0003 && cand[i].ifnum == 2 && strcmp(cand[i].usbdev, mt->usbdev) == 0) {
                act = &cand[i];
            }
        }
    } else {
        for (i = 0; i < n && mt == NULL; i++) {
            if (cand[i].bus == 0x0005) mt = &cand[i];
        }
    }
    if (mt == NULL) return -1;

    d->mt_fd = OpenHidraw(mt->num);
    if (d->mt_fd < 0) {
        fprintf(stderr, "magitrad: /dev/hidraw%d を開けません: %s\n", mt->num, strerror(errno));
        return -1;
    }
    d->kind = mt->bus == 0x0003 ? DEVICE_USB : DEVICE_BT;
    d->transport = d->kind == DEVICE_USB ? MTP_TRANSPORT_USB : MTP_TRANSPORT_BT;
    d->bustype = mt->bus;
    d->vendor = mt->vid;
    d->product = mt->pid;
    snprintf(d->sysdir, sizeof(d->sysdir), "%s", mt->sysdir);
    d->got_frame = 0;
    d->enable_warned = 0;
    if (d->kind == DEVICE_BT) {
        d->haptic_fd = d->mt_fd;
    } else if (act != NULL) {
        d->haptic_fd = OpenHidraw(act->num); /* 開けなければ振動なし（-1 のまま） */
    }

    /* マルチタッチモードへ。失敗しても続ける（カーネルが先に切り替えていることがある） */
    d->last_enable_ms = NowMs();
    SendEnable(d);
    return 0;
}

void DeviceEnsureEnabled(MAGITRA_DEVICE *d)
{
    long long now;
    if (d->kind == DEVICE_NONE || d->got_frame) return;
    now = NowMs();
    if (now - d->last_enable_ms < 3000) return;
    if (!d->enable_warned) {
        fprintf(stderr, "magitrad: 接続から 3 秒たってもフレームが来ません。有効化レポートを送り直します\n");
        d->enable_warned = 1;
    }
    d->last_enable_ms = now;
    SendEnable(d);
}

void DeviceClose(MAGITRA_DEVICE *d)
{
    DeviceUngrab(d);
    if (d->haptic_fd >= 0 && d->haptic_fd != d->mt_fd) close(d->haptic_fd);
    if (d->mt_fd >= 0) close(d->mt_fd);
    d->mt_fd = d->haptic_fd = -1;
    d->kind = DEVICE_NONE;
    d->got_frame = 0;
    d->enable_warned = 0;
}

DEVICE_READ DeviceRead(MAGITRA_DEVICE *d, MTP_FRAME *frame)
{
    uint8_t buf[512];
    ssize_t n = read(d->mt_fd, buf, sizeof(buf));

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return DEVICE_READ_AGAIN;
        if (errno == EINTR) return DEVICE_READ_IGNORED;
        return DEVICE_READ_LOST;
    }
    if (n == 0) return DEVICE_READ_AGAIN;
    if (buf[0] != MtpInputReportId(d->transport)) return DEVICE_READ_IGNORED;
    if (MtpParseReport(d->transport, buf, (size_t)n, frame) != MTP_OK) return DEVICE_READ_IGNORED;
    d->got_frame = 1;
    return DEVICE_READ_FRAME;
}

static int IsGrabbed(const MAGITRA_DEVICE *d, int num)
{
    int i;
    for (i = 0; i < DEVICE_MAX_GRABS; i++) {
        if (d->grab_fd[i] >= 0 && d->grab_num[i] == num) return 1;
    }
    return 0;
}

void DeviceGrabScan(MAGITRA_DEVICE *d)
{
    DIR *dir = opendir("/sys/class/input");
    struct dirent *ent;

    if (dir == NULL) return;
    if (!d->got_frame) { /* 有効化に失敗していると、占有だけが残ってトラックパッドが止まる */
        closedir(dir);
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char path[PATH_MAX], real[PATH_MAX];
        int num, bus, vid, pid, i, fd;

        if (sscanf(ent->d_name, "event%d", &num) != 1 || IsGrabbed(d, num)) continue;
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/id/bustype", ent->d_name);
        if (ReadHex(path, &bus) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/id/vendor", ent->d_name);
        if (ReadHex(path, &vid) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/id/product", ent->d_name);
        if (ReadHex(path, &pid) != 0) continue;
        /* 同じ型の別の機器は、hidraw を読んでいない（占有すると止まる）ので sysfs の位置で絞る */
        snprintf(path, sizeof(path), "/sys/class/input/%s/device", ent->d_name);
        if (realpath(path, real) == NULL || !MagitraPathUnder(real, d->sysdir)) continue;
        if (bus != d->bustype || vid != d->vendor || pid != d->product) continue; /* 自前の uinput は ID が違うので当たらない */

        for (i = 0; i < DEVICE_MAX_GRABS && d->grab_fd[i] >= 0; i++) {
        }
        if (i == DEVICE_MAX_GRABS) break;
        snprintf(path, sizeof(path), "/dev/input/event%d", num);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
            fprintf(stderr, "magitrad: %s を占有できません: %s\n", path, strerror(errno));
            close(fd);
            continue;
        }
        d->grab_fd[i] = fd;
        d->grab_num[i] = num;
    }
    closedir(dir);
}

static void CloseGrab(MAGITRA_DEVICE *d, int i)
{
    if (d->grab_fd[i] >= 0) {
        ioctl(d->grab_fd[i], EVIOCGRAB, (void *)0);
        close(d->grab_fd[i]);
    }
    d->grab_fd[i] = -1;
    d->grab_num[i] = -1;
}

void DeviceUngrab(MAGITRA_DEVICE *d)
{
    int i;
    for (i = 0; i < DEVICE_MAX_GRABS; i++) CloseGrab(d, i);
}

void DeviceGrabDrain(MAGITRA_DEVICE *d, int i)
{
    struct input_event ev[32];
    for (;;) {
        ssize_t n = read(d->grab_fd[i], ev, sizeof(ev));
        if (n > 0) continue;
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        CloseGrab(d, i); /* ENODEV: ノードが消えた */
        return;
    }
}

int DeviceHaptic(MAGITRA_DEVICE *d, const MTP_HAPTIC *haptic)
{
    uint8_t buf[MTP_ACTUATOR_REPORT_LENGTH];
    size_t len;
    long long now = NowMs();
    MTP_STATUS st;

    if (d->haptic_fd < 0) return -1;
    if (d->last_haptic_ms != 0 && now - d->last_haptic_ms < MTP_ACTUATOR_MIN_INTERVAL_MS) return -2;
    memset(buf, 0, sizeof(buf));
    if (d->kind == DEVICE_BT) {
        len = MTP_ACTUATOR_BT_REPORT_LENGTH;
        st = MtpBuildActuatorReportBt(haptic, buf, sizeof(buf), NULL);
    } else {
        len = MTP_ACTUATOR_REPORT_LENGTH;
        st = MtpBuildActuatorReport(haptic, buf, sizeof(buf), NULL);
    }
    if (st != MTP_OK) return -3;
    if (write(d->haptic_fd, buf, len) != (ssize_t)len) return -3;
    d->last_haptic_ms = now;
    return 0;
}
