/*
 * actions.c - 操作の実行
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "actions.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "binding.h"

/* sh -c でコマンドを実行する。孫プロセスにしてゾンビを残さない */
static void Spawn(const char *command)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("magitrad: fork");
        return;
    }
    if (pid == 0) {
        sigset_t none;
        int fd, maxfd = (int)sysconf(_SC_OPEN_MAX);
        setsid();
        if (fork() != 0) _exit(0);
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL); /* signalfd のためにブロックしたシグナルを引き継がない */
        fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
        }
        if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
        for (fd = 3; fd < maxfd; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

static void RunBinding(MAGITRA_UINPUT *u, const char *text, int action)
{
    MAGITRA_BINDING b;
    int i;

    if (text == NULL || MagitraBindingParse(text, &b) != 0) {
        fprintf(stderr, "magitrad: 操作 %d の割り当てを解釈できません: %s\n", action, text ? text : "(なし)");
        return;
    }
    if (b.type == MAGITRA_BINDING_EXEC) {
        Spawn(b.command);
        return;
    }
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
        Spawn(s->launcher_command);
        break;
    default:
        RunBinding(u, MagitraSettingsBinding(s, action), action);
        break;
    }
}
