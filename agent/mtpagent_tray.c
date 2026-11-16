/*
 * mtpagent_tray.c - mtpagent のタスクトレイ常駐版（普段使い用、コンソール窓を出さない）
 *
 * - 読み取りスレッドが WinUSB からフレームを読み、mtp_gesture で入力に変換して SendInput で送る
 * - USB ケーブルを抜いても終了しない。2 秒ごとに再接続を試みる
 * - トレイのメニュー: 一時停止 / ナチュラルスクロール / ログオン時に起動 / 終了
 * - 設定は HKCU\Software\MTP\Agent に保存する。ログオン時の起動は
 *   HKCU\Software\Microsoft\Windows\CurrentVersion\Run（ユーザーごと、管理者権限不要）にメニューから登録・解除する
 * - 二重起動しない（2 つ目は案内を出して終了する）
 *
 * 制約（Windows の仕様）:
 * - ログオン前とロック画面では動かない（このプロセスはログオン後に起動し、保護された画面へは入力を送れない）
 * - 管理者権限で動いているウィンドウには入力を送れない
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "mtp_core.h"
#include "mtp_device.h"
#include "mtp_gesture.h"
#include "mtp_input.h"

#define APP_NAME            L"MTP Agent"
#define WINDOW_CLASS        L"MtpAgentTrayWindow"
#define MUTEX_NAME          L"Local\\MtpAgentSingleInstance"
#define SETTINGS_KEY        L"Software\\MTP\\Agent"
#define RUN_KEY             L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE           L"MTP Agent"
#define RECONNECT_MS        2000

#define WM_TRAY             (WM_APP + 1)
#define WM_LINK_STATE       (WM_APP + 2)

#define IDM_STATUS          100
#define IDM_PAUSE           101
#define IDM_NATURAL         102
#define IDM_AUTOSTART       103
#define IDM_EXIT            104

typedef enum LINK_STATE {
    LINK_STARTING = 0,
    LINK_CONNECTED = 1,
    LINK_NOT_FOUND = 2,   /* WinUSB 未導入か、USB 未接続 */
    LINK_OPEN_FAILED = 3, /* 他のプログラム（mtpagent-cli など）が使っている可能性 */
    LINK_DISCONNECTED = 4
} LINK_STATE;

static HWND g_hwnd;
static UINT g_taskbarCreated;
static HANDLE g_stopEvent;
static HANDLE g_worker;
static volatile LONG g_paused = 0;
static volatile LONG g_natural = 1;
static volatile LONG g_state = LINK_STARTING;

/*
 * 読み取りスレッドが開いているデバイス。終了時にメインスレッドから WinUsb_AbortPipe で読み取りを中断するために共有する。
 * 通常は 200ms のタイムアウトで読み取りが戻るが、戻らない状況があっても終了が待たされないようにする。
 */
static CRITICAL_SECTION g_devLock;
static MTP_DEVICE *g_openDevice = NULL;

static void PublishDevice(MTP_DEVICE *dev)
{
    EnterCriticalSection(&g_devLock);
    g_openDevice = dev;
    LeaveCriticalSection(&g_devLock);
}

static void CloseDevice(MTP_DEVICE *dev)
{
    EnterCriticalSection(&g_devLock);
    g_openDevice = NULL;
    MtpDeviceClose(dev);
    LeaveCriticalSection(&g_devLock);
}

static void AbortPendingRead(void)
{
    EnterCriticalSection(&g_devLock);
    if (g_openDevice != NULL && g_openDevice->usb != NULL) {
        WinUsb_AbortPipe(g_openDevice->usb, g_openDevice->pipe);
    }
    LeaveCriticalSection(&g_devLock);
}

/* ---- 設定 ---- */

static DWORD ReadSettingDword(const wchar_t *name, DWORD fallback)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS) {
        return value;
    }
    return fallback;
}

static void WriteSettingDword(const wchar_t *name, DWORD value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
        RegCloseKey(key);
    }
}

/* Run に登録する値（実行ファイルのパスを引用符で囲む） */
static BOOL BuildRunCommand(wchar_t *out, DWORD capacity)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return FALSE;
    }
    return swprintf_s(out, capacity, L"\"%s\"", path) > 0;
}

/* 登録済み、かつ今の実行ファイルを指しているときだけ TRUE */
static BOOL IsAutostartEnabled(void)
{
    wchar_t expected[MAX_PATH + 4], actual[MAX_PATH + 4];
    DWORD size = sizeof(actual);
    if (!BuildRunCommand(expected, ARRAYSIZE(expected))) {
        return FALSE;
    }
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, actual, &size) != ERROR_SUCCESS) {
        return FALSE;
    }
    return _wcsicmp(expected, actual) == 0;
}

static BOOL SetAutostart(BOOL enable)
{
    HKEY key;
    LSTATUS rc;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return FALSE;
    }
    if (enable) {
        wchar_t command[MAX_PATH + 4];
        if (!BuildRunCommand(command, ARRAYSIZE(command))) {
            RegCloseKey(key);
            return FALSE;
        }
        rc = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)command,
            (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, RUN_VALUE);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

/* ---- 読み取りスレッド ---- */

static void SetLinkState(LINK_STATE s)
{
    if (InterlockedExchange(&g_state, (LONG)s) != (LONG)s) {
        PostMessageW(g_hwnd, WM_LINK_STATE, 0, 0);
    }
}

static void SendEvents(const MTP_EVENT *ev, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        MtpInputSend(&ev[i]);
    }
}

static DWORD WINAPI WorkerMain(LPVOID param)
{
    MTP_DEVICE dev;
    int connected = 0;
    int wasPaused = 0;
    MTP_GESTURE gesture;
    MTP_GESTURE_CONFIG config;
    MTP_EVENT events[MTP_GESTURE_MAX_EVENTS];
    size_t n;

    UNREFERENCED_PARAMETER(param);
    memset(&dev, 0, sizeof(dev));
    MtpGestureDefaultConfig(&config);

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        UCHAR raw[1024];
        ULONG rawLength = 0;
        MTP_FRAME frame;
        MTP_READ_RESULT rr;
        DWORD err = 0;
        int paused;

        if (!connected) {
            MTP_DEVICE_STATUS ds = MtpDeviceOpen(&dev, &err);
            if (ds != MTP_DEVICE_OK) {
                SetLinkState(ds == MTP_DEVICE_NOT_FOUND ? LINK_NOT_FOUND : LINK_OPEN_FAILED);
                WaitForSingleObject(g_stopEvent, RECONNECT_MS);
                continue;
            }
            connected = 1;
            PublishDevice(&dev);
            config.natural_scroll = (int)InterlockedCompareExchange(&g_natural, 0, 0);
            MtpGestureInit(&gesture, &config);
            SetLinkState(LINK_CONNECTED);
        }

        rr = MtpDeviceRead(&dev, &frame, raw, sizeof(raw), &rawLength, &err);
        if (rr == MTP_READ_ERROR) {
            n = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
            SendEvents(events, n);
            CloseDevice(&dev);
            connected = 0;
            if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
                break; /* 終了時の中断で読み取りが失敗しただけ */
            }
            SetLinkState(LINK_DISCONNECTED);
            WaitForSingleObject(g_stopEvent, RECONNECT_MS);
            continue;
        }

        /* 一時停止に入った瞬間に、押したままのボタンを離す */
        paused = (int)InterlockedCompareExchange(&g_paused, 0, 0);
        if (paused && !wasPaused) {
            n = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
            SendEvents(events, n);
        }
        if (!paused && wasPaused) {
            MtpGestureInit(&gesture, &config); /* 停止中の指の動きを引き継がない */
        }
        wasPaused = paused;

        if (rr != MTP_READ_FRAME || paused) {
            continue;
        }
        gesture.config.natural_scroll = (int)InterlockedCompareExchange(&g_natural, 0, 0);
        config.natural_scroll = gesture.config.natural_scroll;
        n = MtpGestureProcess(&gesture, &frame, events, MTP_GESTURE_MAX_EVENTS);
        SendEvents(events, n);
    }

    if (connected) {
        n = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
        SendEvents(events, n);
        CloseDevice(&dev);
    }
    return 0;
}

/* ---- タスクトレイ ---- */

static const wchar_t *StateText(void)
{
    if (InterlockedCompareExchange(&g_paused, 0, 0)) {
        return L"一時停止中";
    }
    switch ((LINK_STATE)InterlockedCompareExchange(&g_state, 0, 0)) {
    case LINK_CONNECTED: return L"接続中";
    case LINK_NOT_FOUND: return L"トラックパッドが見つかりません（USB 未接続か WinUSB 未導入）";
    case LINK_OPEN_FAILED: return L"トラックパッドを開けません（mtpagent-cli など別のプログラムが使用中）";
    case LINK_DISCONNECTED: return L"切断されました。再接続を待っています";
    default: return L"起動中";
    }
}

static void UpdateTray(DWORD message)
{
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    swprintf_s(nid.szTip, ARRAYSIZE(nid.szTip), L"MTP: %s", StateText());
    Shell_NotifyIconW(message, &nid);
}

static void RemoveTray(void)
{
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    wchar_t status[160];

    swprintf_s(status, ARRAYSIZE(status), L"状態: %s", StateText());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (InterlockedCompareExchange(&g_paused, 0, 0) ? MF_CHECKED : 0), IDM_PAUSE, L"一時停止");
    AppendMenuW(menu, MF_STRING | (InterlockedCompareExchange(&g_natural, 0, 0) ? MF_CHECKED : 0), IDM_NATURAL,
        L"ナチュラルスクロール");
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"ログオン時に起動");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"終了");

    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); /* これがないとメニュー外をクリックしても閉じない */
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void OnCommand(HWND hwnd, UINT id)
{
    switch (id) {
    case IDM_PAUSE:
        InterlockedExchange(&g_paused, !InterlockedCompareExchange(&g_paused, 0, 0));
        UpdateTray(NIM_MODIFY);
        break;
    case IDM_NATURAL: {
        LONG next = !InterlockedCompareExchange(&g_natural, 0, 0);
        InterlockedExchange(&g_natural, next);
        WriteSettingDword(L"NaturalScroll", (DWORD)next);
        break;
    }
    case IDM_AUTOSTART:
        if (!SetAutostart(!IsAutostartEnabled())) {
            MessageBoxW(hwnd, L"ログオン時の起動の設定を変更できませんでした。", APP_NAME, MB_ICONWARNING | MB_OK);
        }
        break;
    case IDM_EXIT:
        DestroyWindow(hwnd);
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        UpdateTray(NIM_ADD); /* エクスプローラーが再起動したらアイコンを出し直す */
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowMenu(hwnd);
        }
        return 0;
    case WM_LINK_STATE:
        UpdateTray(NIM_MODIFY);
        return 0;
    case WM_COMMAND:
        OnCommand(hwnd, LOWORD(wParam));
        return 0;
    case WM_DESTROY:
        SetEvent(g_stopEvent);
        AbortPendingRead(); /* 読み取り中なら中断して、すぐに停止要求を見させる */
        if (g_worker != NULL) {
            WaitForSingleObject(g_worker, 5000);
        }
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev, _In_ PWSTR cmdLine, _In_ int show)
{
    HANDLE mutex;
    WNDCLASSEXW wc;
    MSG msg;

    UNREFERENCED_PARAMETER(prev);
    UNREFERENCED_PARAMETER(cmdLine);
    UNREFERENCED_PARAMETER(show);

    mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"MTP Agent はすでに起動しています。タスクトレイのアイコンから操作してください。", APP_NAME,
            MB_ICONINFORMATION | MB_OK);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    InitializeCriticalSection(&g_devLock);
    g_natural = (LONG)(ReadSettingDword(L"NaturalScroll", 1) != 0);
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc)) {
        return 1;
    }
    /* 見えないウィンドウ（トレイの通知とメニューの受け口） */
    g_hwnd = CreateWindowExW(0, WINDOW_CLASS, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (g_hwnd == NULL || g_stopEvent == NULL) {
        return 1;
    }
    UpdateTray(NIM_ADD);

    g_worker = CreateThread(NULL, 0, WorkerMain, NULL, 0, NULL);
    if (g_worker == NULL) {
        MessageBoxW(g_hwnd, L"読み取りスレッドを開始できませんでした。", APP_NAME, MB_ICONERROR | MB_OK);
        DestroyWindow(g_hwnd);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_worker) CloseHandle(g_worker);
    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_devLock);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
