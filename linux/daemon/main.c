/*
 * magitrad - MAGITRA の Linux 常駐アプリ
 *
 * 1 スレッドの poll() ループ: hidraw（マルチタッチ）、IPC のソケットとクライアント、慣性スクロールの timerfd（8ms）、
 * 再接続・再占有の timerfd（1 秒）、signalfd。
 * 使い方: magitrad [--log] [--version]
 *   --log  フレームとイベントを stderr に出す（フレームの行は agent/tests/replay_gesture.c が読める形式）
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "actions.h"
#include "device.h"
#include "mtp_momentum.h"
#include "settings.h"
#include "uinput.h"

#define VERSION "0.1.0"
#define MAX_CLIENTS 8
#define CLIENT_BUF 256

typedef struct CLIENT {
    int fd;
    char buf[CLIENT_BUF];
    size_t len;
} CLIENT;

static MAGITRA_SETTINGS g_set;
static MTP_GESTURE_CONFIG g_cfg;
static MTP_GESTURE g_gesture;
static MTP_MOMENTUM g_momentum;
static MAGITRA_DEVICE g_dev;
static MAGITRA_UINPUT g_ui;
static CLIENT g_clients[MAX_CLIENTS];
static char g_conf_path[PATH_MAX];
static char g_sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int g_inertia_fd = -1;
static int g_listen_fd = -1;
static int g_paused = 0;
static int g_quit = 0;
static int g_log = 0;
static double g_start_ms = 0;
static double g_last_step_ms = 0;

static double NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void LogFrame(const MTP_FRAME *f)
{
    int i;
    fprintf(stderr, "%9.1f n=%u btn=%u", NowMs() - g_start_ms, f->contact_count, f->button);
    for (i = 0; i < f->contact_count; i++) {
        const MTP_CONTACT *c = &f->contacts[i];
        fprintf(stderr, " | id=%u st=%u maj=%u x=%d y=%d p=%u", c->id, c->state, c->touch_major, c->x, c->y, c->pressure);
    }
    fprintf(stderr, "\n");
}

static void LogEvent(const MTP_EVENT *ev)
{
    static const char *dirs[] = { "左", "右", "上", "下" };
    switch (ev->type) {
    case MTP_EVENT_MOVE: fprintf(stderr, "[event] move dx=%d dy=%d\n", ev->dx, ev->dy); break;
    case MTP_EVENT_BUTTON:
        fprintf(stderr, "[event] %s %s\n", ev->button == MTP_BUTTON_RIGHT ? "right" : "left", ev->down ? "down" : "up");
        break;
    case MTP_EVENT_SCROLL: fprintf(stderr, "[event] scroll h=%d v=%d\n", ev->dx, ev->dy); break;
    case MTP_EVENT_FORCE: fprintf(stderr, "[event] force\n"); break;
    case MTP_EVENT_SWIPE: fprintf(stderr, "[event] swipe %d本指 %s\n", ev->fingers, dirs[ev->direction & 3]); break;
    case MTP_EVENT_FLING: fprintf(stderr, "[event] fling h=%d/s v=%d/s\n", ev->dx, ev->dy); break;
    case MTP_EVENT_TOUCH: fprintf(stderr, "[event] touch\n"); break;
    case MTP_EVENT_ZOOM: fprintf(stderr, "[event] zoom %d\n", ev->dy); break;
    case MTP_EVENT_ZOOM_END: fprintf(stderr, "[event] zoom end\n"); break;
    case MTP_EVENT_DOUBLE_TAP: fprintf(stderr, "[event] double tap\n"); break;
    default: break;
    }
}

static void SetInertiaTimer(int on)
{
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    if (on) {
        its.it_interval.tv_nsec = 8 * 1000 * 1000;
        its.it_value.tv_nsec = 8 * 1000 * 1000;
    }
    timerfd_settime(g_inertia_fd, 0, &its, NULL);
}

static void MomentumStop(void)
{
    MtpMomentumStop(&g_momentum);
    SetInertiaTimer(0);
}

static void Dispatch(const MTP_EVENT *events, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        const MTP_EVENT *ev = &events[i];
        if (g_log) LogEvent(ev);
        switch (ev->type) {
        case MTP_EVENT_MOVE:
            UinputMove(&g_ui, ev->dx, ev->dy);
            break;
        case MTP_EVENT_BUTTON:
            UinputButton(&g_ui, ev->button, ev->down);
            break;
        case MTP_EVENT_SCROLL:
            UinputScroll(&g_ui, ev->dx, ev->dy);
            break;
        case MTP_EVENT_FORCE: {
            MTP_HAPTIC h;
            MagitraSettingsHaptic(&g_set, &h);
            DeviceHaptic(&g_dev, &h);
            ActionRun(&g_set, &g_ui, g_set.force_action);
            break;
        }
        case MTP_EVENT_SWIPE:
            ActionRun(&g_set, &g_ui, MagitraSettingsSwipeAction(&g_set, ev->fingers, ev->direction));
            break;
        case MTP_EVENT_FLING:
            MtpMomentumStart(&g_momentum, (float)ev->dx, (float)ev->dy);
            if (g_momentum.active) {
                g_last_step_ms = NowMs();
                SetInertiaTimer(1);
            }
            break;
        case MTP_EVENT_TOUCH:
            MomentumStop();
            break;
        case MTP_EVENT_ZOOM:
            UinputZoom(&g_ui, ev->dy);
            break;
        case MTP_EVENT_ZOOM_END:
            UinputZoomEnd(&g_ui);
            break;
        case MTP_EVENT_DOUBLE_TAP:
            ActionRun(&g_set, &g_ui, g_set.double_tap_action);
            break;
        }
    }
}

/* 押したままのボタン・Ctrl・慣性を止める */
static void ReleaseAll(void)
{
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    Dispatch(ev, MtpGestureRelease(&g_gesture, ev, MTP_GESTURE_MAX_EVENTS));
    UinputZoomEnd(&g_ui);
    MomentumStop();
}

static void Reload(void)
{
    MagitraSettingsLoadFile(&g_set, g_conf_path);
    MagitraSettingsApply(&g_set, &g_cfg);
    g_gesture.config = g_cfg; /* 触れている指の状態は引き継ぐ */
    g_momentum.tau_ms = MagitraSettingsGlideToTau(g_set.inertia_glide);
}

static void Disconnect(void)
{
    ReleaseAll();
    DeviceClose(&g_dev);
    MtpGestureInit(&g_gesture, &g_cfg);
    fprintf(stderr, "magitrad: トラックパッドが切断されました\n");
}

static void HandleFrame(const MTP_FRAME *f)
{
    MTP_EVENT ev[MTP_GESTURE_MAX_EVENTS];
    double now = NowMs();
    if (g_log) LogFrame(f);
    if (g_paused) return;
    Dispatch(ev, MtpGestureProcessAt(&g_gesture, f, now, ev, MTP_GESTURE_MAX_EVENTS));
}

static void OnRetryTimer(void)
{
    if (g_dev.kind == DEVICE_NONE) {
        if (DeviceOpen(&g_dev) != 0) return;
        MtpGestureInit(&g_gesture, &g_cfg);
        fprintf(stderr, "magitrad: トラックパッドに接続しました（%s）\n", g_dev.kind == DEVICE_USB ? "USB" : "Bluetooth");
    }
    if (!g_paused) DeviceGrabScan(&g_dev);
}

static void OnInertiaTimer(void)
{
    double now = NowMs();
    int h = 0, v = 0;
    int active = MtpMomentumStep(&g_momentum, (float)(now - g_last_step_ms), &h, &v);
    g_last_step_ms = now;
    if (h != 0 || v != 0) UinputScroll(&g_ui, h, v);
    if (!active) SetInertiaTimer(0);
}

static void Pause(void)
{
    if (g_paused) return;
    g_paused = 1;
    ReleaseAll();
    DeviceUngrab(&g_dev);
    MtpGestureInit(&g_gesture, &g_cfg);
}

static void Resume(void)
{
    if (!g_paused) return;
    g_paused = 0;
    MtpGestureInit(&g_gesture, &g_cfg);
    if (g_dev.kind != DEVICE_NONE) DeviceGrabScan(&g_dev);
}

/* ---- IPC ---- */

static void Command(char *line, char *reply, size_t size)
{
    int a, b, c;
    char extra;

    if (strcmp(line, "RELOAD") == 0) {
        Reload();
        snprintf(reply, size, "OK");
    } else if (strncmp(line, "HAPTIC ", 7) == 0) {
        if (sscanf(line + 7, "%d %d %d %c", &a, &b, &c, &extra) != 3 ||
            a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255) {
            snprintf(reply, size, "ERR bad arguments");
        } else {
            MTP_HAPTIC h;
            int rc;
            h.strength = (uint8_t)a;
            h.sharpness = (uint8_t)b;
            h.depth = (uint8_t)c;
            rc = DeviceHaptic(&g_dev, &h);
            if (rc == 0) snprintf(reply, size, "OK");
            else if (g_dev.kind == DEVICE_NONE) snprintf(reply, size, "ERR not connected");
            else if (rc == -1) snprintf(reply, size, "ERR no actuator");
            else if (rc == -2) snprintf(reply, size, "ERR too fast");
            else snprintf(reply, size, "ERR write failed");
        }
    } else if (strcmp(line, "PAUSE") == 0) {
        Pause();
        snprintf(reply, size, "OK");
    } else if (strcmp(line, "RESUME") == 0) {
        Resume();
        snprintf(reply, size, "OK");
    } else if (strcmp(line, "STATUS") == 0) {
        snprintf(reply, size, "OK connected=%s paused=%d",
            g_dev.kind == DEVICE_USB ? "usb" : (g_dev.kind == DEVICE_BT ? "bt" : "none"), g_paused);
    } else if (strcmp(line, "QUIT") == 0) {
        g_quit = 1;
        snprintf(reply, size, "OK");
    } else {
        snprintf(reply, size, "ERR unknown command");
    }
}

static void CloseClient(CLIENT *c)
{
    close(c->fd);
    c->fd = -1;
    c->len = 0;
}

static void OnClientReadable(CLIENT *c)
{
    ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
    char *nl;

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        CloseClient(c);
        return;
    }
    if (n > 0) c->len += (size_t)n;
    while (c->fd >= 0 && (nl = memchr(c->buf, '\n', c->len)) != NULL) {
        char reply[96];
        size_t used = (size_t)(nl - c->buf) + 1;
        *nl = '\0';
        if (nl > c->buf && nl[-1] == '\r') nl[-1] = '\0';
        Command(c->buf, reply, sizeof(reply) - 1);
        strcat(reply, "\n");
        if (send(c->fd, reply, strlen(reply), MSG_NOSIGNAL) < 0) {
            CloseClient(c);
            return;
        }
        memmove(c->buf, c->buf + used, c->len - used);
        c->len -= used;
    }
    if (c->fd >= 0 && c->len == sizeof(c->buf)) CloseClient(c); /* 改行のない長すぎる行 */
}

static void OnAccept(void)
{
    int fd = accept4(g_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int i;
    if (fd < 0) return;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0) {
            g_clients[i].fd = fd;
            g_clients[i].len = 0;
            return;
        }
    }
    close(fd);
}

static int Listen(void)
{
    const char *run = getenv("XDG_RUNTIME_DIR");
    char dir[PATH_MAX], fallback[64];
    struct sockaddr_un addr;
    mode_t old;
    int probe;

    if (run == NULL || run[0] != '/') {
        snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)getuid());
        run = fallback;
    }
    snprintf(dir, sizeof(dir), "%s/magitra", run);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "magitrad: %s を作れません: %s\n", dir, strerror(errno));
        return -1;
    }
    chmod(dir, 0700);
    if (snprintf(g_sock_path, sizeof(g_sock_path), "%s/agent.sock", dir) >= (int)sizeof(g_sock_path)) {
        fprintf(stderr, "magitrad: ソケットのパスが長すぎます\n");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, g_sock_path);

    /* すでに動いているなら二重起動しない。残骸のソケットなら消す */
    probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            fprintf(stderr, "magitrad: すでに動いています\n");
            close(probe);
            return -1;
        }
        close(probe);
    }
    unlink(g_sock_path);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_listen_fd < 0) return -1;
    old = umask(0177); /* ソケットは 0600 */
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(g_listen_fd, 4) != 0) {
        fprintf(stderr, "magitrad: %s を待ち受けできません: %s\n", g_sock_path, strerror(errno));
        umask(old);
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }
    umask(old);
    return 0;
}

/* ---- 主ループ ---- */

enum { P_SIGNAL, P_LISTEN, P_INERTIA, P_RETRY, P_MT, P_GRAB, P_CLIENT = P_GRAB + DEVICE_MAX_GRABS, P_COUNT = P_CLIENT + MAX_CLIENTS };

static void Drain(int fd)
{
    uint64_t v;
    if (read(fd, &v, sizeof(v)) < 0) {
        /* 期限切れが重なっていなければ EAGAIN。何もしない */
    }
}

int main(int argc, char **argv)
{
    struct pollfd pfds[P_COUNT];
    struct itimerspec retry;
    sigset_t mask;
    int sig_fd, retry_fd, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0) {
            g_log = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("magitrad %s\n", VERSION);
            return 0;
        } else {
            fprintf(stderr, "使い方: magitrad [--log] [--version]\n");
            return 2;
        }
    }

    if (MagitraSettingsPath(g_conf_path, sizeof(g_conf_path)) != 0) g_conf_path[0] = '\0';
    DeviceInit(&g_dev);
    MtpMomentumInit(&g_momentum);
    for (i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;
    g_start_ms = NowMs();

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    g_inertia_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    retry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (sig_fd < 0 || g_inertia_fd < 0 || retry_fd < 0) {
        perror("magitrad: signalfd/timerfd");
        return 1;
    }

    if (UinputOpen(&g_ui) != 0) return 1;
    if (Listen() != 0) {
        UinputClose(&g_ui);
        return 1;
    }
    Reload();
    MtpGestureInit(&g_gesture, &g_cfg);

    memset(&retry, 0, sizeof(retry));
    retry.it_interval.tv_sec = 1;
    retry.it_value.tv_nsec = 1; /* すぐ 1 回目 */
    timerfd_settime(retry_fd, 0, &retry, NULL);

    while (!g_quit) {
        int k;
        for (k = 0; k < P_COUNT; k++) {
            pfds[k].fd = -1;
            pfds[k].events = POLLIN;
            pfds[k].revents = 0;
        }
        pfds[P_SIGNAL].fd = sig_fd;
        pfds[P_LISTEN].fd = g_listen_fd;
        pfds[P_INERTIA].fd = g_inertia_fd;
        pfds[P_RETRY].fd = retry_fd;
        pfds[P_MT].fd = g_dev.mt_fd;
        for (k = 0; k < DEVICE_MAX_GRABS; k++) pfds[P_GRAB + k].fd = g_dev.grab_fd[k];
        for (k = 0; k < MAX_CLIENTS; k++) pfds[P_CLIENT + k].fd = g_clients[k].fd;

        if (poll(pfds, P_COUNT, -1) < 0) {
            if (errno == EINTR) continue;
            perror("magitrad: poll");
            break;
        }

        if (pfds[P_SIGNAL].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sig_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                if (si.ssi_signo == SIGHUP) Reload();
                else g_quit = 1;
            }
        }
        if (pfds[P_LISTEN].revents & POLLIN) OnAccept();
        if (pfds[P_RETRY].revents & POLLIN) {
            Drain(retry_fd);
            OnRetryTimer();
        }
        if (pfds[P_INERTIA].revents & POLLIN) {
            Drain(g_inertia_fd);
            OnInertiaTimer();
        }
        if (pfds[P_MT].revents != 0 && pfds[P_MT].fd == g_dev.mt_fd && g_dev.mt_fd >= 0) {
            int lost = 0, n;
            for (n = 0; n < 64 && !lost; n++) {
                MTP_FRAME frame;
                DEVICE_READ r = DeviceRead(&g_dev, &frame);
                if (r == DEVICE_READ_FRAME) HandleFrame(&frame);
                else if (r == DEVICE_READ_LOST) lost = 1;
                else if (r == DEVICE_READ_AGAIN) break;
            }
            if (lost || (pfds[P_MT].revents & (POLLERR | POLLHUP | POLLNVAL))) Disconnect();
        }
        for (k = 0; k < DEVICE_MAX_GRABS; k++) {
            if (pfds[P_GRAB + k].revents != 0 && pfds[P_GRAB + k].fd == g_dev.grab_fd[k] && g_dev.grab_fd[k] >= 0) {
                DeviceGrabDrain(&g_dev, k);
            }
        }
        for (k = 0; k < MAX_CLIENTS; k++) {
            if (pfds[P_CLIENT + k].revents != 0 && pfds[P_CLIENT + k].fd == g_clients[k].fd && g_clients[k].fd >= 0) {
                OnClientReadable(&g_clients[k]);
            }
        }
    }

    ReleaseAll();
    DeviceClose(&g_dev);
    UinputClose(&g_ui);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd >= 0) close(g_clients[i].fd);
    }
    close(g_listen_fd);
    unlink(g_sock_path);
    return 0;
}
