/*
 * mtp_launcher.c - ランチャー（mtplauncher.exe）の起動と、表示の知らせ
 */
#include "mtp_launcher.h"

#include <wchar.h>

static volatile LONG g_lastStartTick = 0;

static HWND FindLauncher(void)
{
    return FindWindowW(NULL, MTP_LAUNCHER_WINDOW_TITLE);
}

static UINT LauncherMessage(void)
{
    static UINT msg = 0;
    if (msg == 0) {
        msg = RegisterWindowMessageW(MTP_MSG_LAUNCHER);
    }
    return msg;
}

/* 常駐アプリと同じフォルダーの mtplauncher.exe を起動する。起動したばかりなら（3 秒以内）重ねて起動しない */
static BOOL StartLauncher(const wchar_t *args)
{
    wchar_t path[MAX_PATH];
    wchar_t cmd[MAX_PATH + 32];
    wchar_t *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD len;
    DWORD now = GetTickCount();

    if (now - (DWORD)InterlockedCompareExchange(&g_lastStartTick, 0, 0) < 3000 &&
        InterlockedCompareExchange(&g_lastStartTick, 0, 0) != 0) {
        return FALSE;
    }
    len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return FALSE;
    }
    slash = wcsrchr(path, L'\\');
    if (slash == NULL) {
        return FALSE;
    }
    slash[1] = L'\0';
    if (wcslen(path) + wcslen(MTP_LAUNCHER_EXE) >= MAX_PATH) {
        return FALSE;
    }
    wcscat_s(path, MAX_PATH, MTP_LAUNCHER_EXE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return FALSE;
    }
    swprintf_s(cmd, MAX_PATH + 32, L"\"%s\" %s", path, args);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return FALSE;
    }
    InterlockedExchange(&g_lastStartTick, (LONG)(now == 0 ? 1 : now));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

void MtpLauncherStart(void)
{
    if (FindLauncher() == NULL) {
        StartLauncher(L"");
    }
}

static void Post(MTP_LAUNCHER_CMD cmd, LPARAM lParam)
{
    HWND h = FindLauncher();
    if (h != NULL && LauncherMessage() != 0) {
        PostMessageW(h, LauncherMessage(), (WPARAM)cmd, lParam);
    }
}

BOOL MtpLauncherShow(void)
{
    HWND h = FindLauncher();
    DWORD pid = 0;

    if (h == NULL || LauncherMessage() == 0) {
        return StartLauncher(L"/show");
    }
    /* ランチャーが前面に出られるように（入力を送ったのはこちらなので、こちらから許す） */
    GetWindowThreadProcessId(h, &pid);
    if (pid != 0) {
        AllowSetForegroundWindow(pid);
    }
    return PostMessageW(h, LauncherMessage(), MTP_LAUNCHER_SHOW, 0);
}

void MtpLauncherQuit(void)
{
    Post(MTP_LAUNCHER_QUIT, 0);
}
