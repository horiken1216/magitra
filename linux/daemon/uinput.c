/*
 * uinput.c - 仮想デバイスの作成と入力の注入
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "uinput.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "binding.h"

#define VIRTUAL_VENDOR  0x1209
#define VIRTUAL_PRODUCT 0x4d54

static void Emit(int fd, uint16_t type, uint16_t code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        /* 書けなくても続ける（相手が消えたときなど）。次の入力でまた試す */
    }
}

static void Syn(int fd)
{
    Emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int Create(const char *name, int pointer)
{
    static const uint16_t rels[] = { REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES };
    static const uint16_t btns[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    struct uinput_setup setup;
    size_t i;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0) {
        fprintf(stderr, "magitrad: /dev/uinput を開けません: %s\n", strerror(errno));
        return -1;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (pointer) {
        if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0) goto fail;
        for (i = 0; i < sizeof(rels) / sizeof(rels[0]); i++) {
            if (ioctl(fd, UI_SET_RELBIT, rels[i]) < 0) goto fail;
        }
        for (i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
            if (ioctl(fd, UI_SET_KEYBIT, btns[i]) < 0) goto fail;
        }
    } else {
        for (i = 0; MagitraBindingKeyAt(i) != 0; i++) {
            if (ioctl(fd, UI_SET_KEYBIT, MagitraBindingKeyAt(i)) < 0) goto fail;
        }
    }
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = VIRTUAL_VENDOR;
    setup.id.product = VIRTUAL_PRODUCT;
    snprintf(setup.name, sizeof(setup.name), "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) goto fail;
    if (ioctl(fd, UI_DEV_CREATE) < 0) goto fail;
    return fd;

fail:
    fprintf(stderr, "magitrad: 仮想デバイス %s を作れません: %s\n", name, strerror(errno));
    close(fd);
    return -1;
}

int UinputOpen(MAGITRA_UINPUT *u)
{
    memset(u, 0, sizeof(*u));
    u->ptr_fd = u->kbd_fd = -1;
    u->ptr_fd = Create("MAGITRA pointer", 1);
    if (u->ptr_fd < 0) return -1;
    u->kbd_fd = Create("MAGITRA keyboard", 0);
    if (u->kbd_fd < 0) {
        UinputClose(u);
        return -1;
    }
    return 0;
}

void UinputClose(MAGITRA_UINPUT *u)
{
    UinputZoomEnd(u);
    if (u->ptr_fd >= 0) {
        ioctl(u->ptr_fd, UI_DEV_DESTROY);
        close(u->ptr_fd);
    }
    if (u->kbd_fd >= 0) {
        ioctl(u->kbd_fd, UI_DEV_DESTROY);
        close(u->kbd_fd);
    }
    u->ptr_fd = u->kbd_fd = -1;
}

void UinputMove(MAGITRA_UINPUT *u, int dx, int dy)
{
    if (dx != 0) Emit(u->ptr_fd, EV_REL, REL_X, dx);
    if (dy != 0) Emit(u->ptr_fd, EV_REL, REL_Y, dy);
    Syn(u->ptr_fd);
}

void UinputButton(MAGITRA_UINPUT *u, MTP_BUTTON button, int down)
{
    uint16_t code = button == MTP_BUTTON_RIGHT ? BTN_RIGHT : (button == MTP_BUTTON_MIDDLE ? BTN_MIDDLE : BTN_LEFT);
    Emit(u->ptr_fd, EV_KEY, code, down ? 1 : 0);
    Syn(u->ptr_fd);
}

/* 端数を持ち越しながら、高分解能の値と整数ノッチの両方を出す */
static void Wheel(int fd, int *rem, int units, uint16_t hires_code, uint16_t code)
{
    int notches;
    if (units == 0) return;
    Emit(fd, EV_REL, hires_code, units);
    *rem += units;
    notches = *rem / 120;
    *rem -= notches * 120;
    if (notches != 0) Emit(fd, EV_REL, code, notches);
}

void UinputScroll(MAGITRA_UINPUT *u, int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    Wheel(u->ptr_fd, &u->rem_v, dy, REL_WHEEL_HI_RES, REL_WHEEL);
    Wheel(u->ptr_fd, &u->rem_h, dx, REL_HWHEEL_HI_RES, REL_HWHEEL);
    Syn(u->ptr_fd);
}

void UinputKey(MAGITRA_UINPUT *u, uint16_t code, int down)
{
    Emit(u->kbd_fd, EV_KEY, code, down ? 1 : 0);
    Syn(u->kbd_fd);
}

void UinputZoom(MAGITRA_UINPUT *u, int dy)
{
    if (!u->ctrl_held) {
        UinputKey(u, MAGITRA_KEY_LEFTCTRL, 1);
        u->ctrl_held = 1;
    }
    UinputScroll(u, 0, dy);
}

void UinputZoomEnd(MAGITRA_UINPUT *u)
{
    if (u->ctrl_held && u->kbd_fd >= 0) {
        UinputKey(u, MAGITRA_KEY_LEFTCTRL, 0);
    }
    u->ctrl_held = 0;
}
