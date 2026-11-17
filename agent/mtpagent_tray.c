/*
 * mtpagent_tray.c - mtpagent のタスクトレイ常駐版（普段使い用、コンソール窓を出さない）
 *
 * - 読み取りスレッドが WinUSB からフレームを読み、mtp_gesture で入力に変換して SendInput で送る
 * - USB ケーブルを抜いても終了しない。2 秒ごとに再接続を試みる
 * - 設定は mtp_settings（HKCU\Software\MTP\Agent）。設定画面（mtpsettings.exe）で変えると、
 *   MTP_MSG_SETTINGS_CHANGED が届いてその場で読み直す（再起動は要らない）
 * - トレイのメニュー: 設定... / 一時停止 / ナチュラルスクロール / Force Touch / 3・4本指ジェスチャー / ログオン時に起動 / 終了
 * - Force Touch が成立すると、設定の強さで振動させてから、割り当てた操作（既定は中クリック）を送る
 * - 3・4 本指のスワイプは、設定の割り当てを実行する
 * - ログオン時の起動は HKCU\Software\Microsoft\Windows\CurrentVersion\Run（ユーザーごと、管理者権限不要）に
 *   メニューから登録・解除する
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
#include <wtsapi32.h>

#include "mtp_actions.h"
#include "mtp_core.h"
#include "mtp_device.h"
#include "mtp_gesture.h"
#include "mtp_haptic_out.h"
#include "mtp_input.h"
#include "mtp_momentum.h"
#include "mtp_resource.h"
#include "mtp_settings.h"

#define APP_NAME            L"MAGITRA"
/* 見えないウィンドウの題名。インストーラーと設定画面がこの名前で探すので、アプリ名を変えても変えない */
#define WINDOW_TITLE        L"MTP Agent"
#define WINDOW_CLASS        MTP_AGENT_WINDOW_CLASS
#define MUTEX_NAME          L"Local\\MtpAgentSingleInstance"
#define AUTOSTART_ARG       L"/autostart"
#define RUN_KEY             L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE           L"MTP Agent"
#define SETTINGS_EXE        L"mtpsettings.exe"
#define RECONNECT_MS        2000

#define WM_TRAY             (WM_APP + 1)
#define WM_LINK_STATE       (WM_APP + 2)

#define IDM_STATUS          100
#define IDM_PAUSE           101
#define IDM_NATURAL         102
#define IDM_AUTOSTART       103
#define IDM_EXIT            104
#define IDM_FORCE           105
#define IDM_GESTURES        106
#define IDM_SETTINGS        107

typedef enum LINK_STATE {
    LINK_STARTING = 0,
    LINK_CONNECTED = 1,
    LINK_NOT_FOUND = 2,   /* WinUSB 未導入か、USB 未接続 */
    LINK_OPEN_FAILED = 3, /* 他のプログラム（mtpagent-cli など）が使っている可能性 */
    LINK_DISCONNECTED = 4,
    LINK_RELEASED = 5     /* ロック中などで、トラックパッドを Windows に返している */
} LINK_STATE;

static HWND g_hwnd;
static UINT g_taskbarCreated;
static UINT g_msgSettingsChanged;
static UINT g_msgHapticTest;
static HANDLE g_stopEvent;
static HANDLE g_worker;
static volatile LONG g_paused = 0;
/*
 * ロック中・切断中は 1。WinUSB のハンドルを閉じて、開き直さない。
 * ハンドルを開いたままだと、MtpSwitch サービスが標準の HID に切り替えても機器を止められず
 * （「再起動が必要」のまま）、WinUSB で動き続けてしまう。
 */
static volatile LONG g_released = 0;
static volatile LONG g_state = LINK_STARTING;
static MTP_HAPTIC_OUT g_haptic;   /* 読み取りスレッドだけが使う */
static MTP_HAPTIC_OUT g_uiHaptic; /* メインスレッドだけが使う（設定画面の「試す」） */

/*
 * 設定。メインスレッドが書き、読み取りスレッドは版番号が変わったときだけ写しを取る。
 */
static CRITICAL_SECTION g_settingsLock;
static MTP_SETTINGS g_settings;
static volatile LONG g_settingsVersion = 0;

static void GetSettings(MTP_SETTINGS *out)
{
    EnterCriticalSection(&g_settingsLock);
    *out = g_settings;
    LeaveCriticalSection(&g_settingsLock);
}

static void ReloadSettings(void)
{
    MTP_SETTINGS s;
    MtpSettingsLoad(&s);
    EnterCriticalSection(&g_settingsLock);
    g_settings = s;
    LeaveCriticalSection(&g_settingsLock);
    InterlockedIncrement(&g_settingsVersion);
}

/* メニューの切り替え: 値を反転して保存し、読み取りスレッドへ知らせる */
static void ToggleSetting(int *field, const wchar_t *name)
{
    DWORD next;
    EnterCriticalSection(&g_settingsLock);
    *field = !*field;
    next = (DWORD)*field;
    LeaveCriticalSection(&g_settingsLock);
    MtpSettingsSaveValue(name, next);
    InterlockedIncrement(&g_settingsVersion);
}

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

/* ---- ログオン時の起動 ---- */

/*
 * Run に登録する値（実行ファイルのパスを引用符で囲み、/autostart を付ける）。
 * /autostart で起動したときは、すでに起動していれば何も出さずに終わる。
 * MtpSwitch サービスがログオン直後に起動し、そのあと Windows が Run から起動することがあるため。
 */
static BOOL BuildRunCommand(wchar_t *out, DWORD capacity, BOOL withArg)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return FALSE;
    }
    return swprintf_s(out, capacity, withArg ? L"\"%s\" " AUTOSTART_ARG : L"\"%s\"", path) > 0;
}

/* 登録済み、かつ今の実行ファイルを指しているときだけ TRUE（/autostart を付ける前の値も認める） */
static BOOL IsAutostartEnabled(void)
{
    wchar_t withArg[MAX_PATH + 32], bare[MAX_PATH + 32], actual[MAX_PATH + 32];
    DWORD size = sizeof(actual);
    if (!BuildRunCommand(withArg, ARRAYSIZE(withArg), TRUE) || !BuildRunCommand(bare, ARRAYSIZE(bare), FALSE)) {
        return FALSE;
    }
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, actual, &size) != ERROR_SUCCESS) {
        return FALSE;
    }
    return _wcsicmp(withArg, actual) == 0 || _wcsicmp(bare, actual) == 0;
}

static BOOL SetAutostart(BOOL enable)
{
    HKEY key;
    LSTATUS rc;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return FALSE;
    }
    if (enable) {
        wchar_t command[MAX_PATH + 32];
        if (!BuildRunCommand(command, ARRAYSIZE(command), TRUE)) {
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

/* ---- 慣性スクロール ---- */

/*
 * 指を離した後もスクロールを続けるので、トラックパッドのフレームとは別のスレッドで時間を刻む
 * （指が触れていない間、フレームが届き続けるとは限らない）。約 8ms ごとに進める。
 */
#define MOMENTUM_TICK_MS 8

static CRITICAL_SECTION g_momLock;
static MTP_MOMENTUM g_momentum;
static HANDLE g_momWake;   /* 始めた合図（自動リセット） */
static HANDLE g_momThread;

static void MomentumStart(float vx, float vy)
{
    EnterCriticalSection(&g_momLock);
    MtpMomentumStart(&g_momentum, vx, vy);
    LeaveCriticalSection(&g_momLock);
    SetEvent(g_momWake);
}

static void MomentumStop(void)
{
    EnterCriticalSection(&g_momLock);
    MtpMomentumStop(&g_momentum);
    LeaveCriticalSection(&g_momLock);
}

static DWORD WINAPI MomentumMain(LPVOID param)
{
    HANDLE timer;
    HANDLE wait[2];
    LARGE_INTEGER freq;

    UNREFERENCED_PARAMETER(param);
    QueryPerformanceFrequency(&freq);
    /* 高精度のタイマー（Windows 10 1803 以降）。使えなければ普通のタイマー（約 15ms 刻みになる） */
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == NULL) {
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    wait[0] = g_stopEvent;
    wait[1] = g_momWake;
    while (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        LARGE_INTEGER last, now;
        QueryPerformanceCounter(&last);
        for (;;) {
            int h = 0, v = 0, active;
            MTP_EVENT ev;
            if (timer != NULL) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)MOMENTUM_TICK_MS * 10000; /* 100ns 単位の相対時間 */
                SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                wait[1] = timer;
                if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    break;
                }
                wait[1] = g_momWake;
            } else if (WaitForSingleObject(g_stopEvent, MOMENTUM_TICK_MS) == WAIT_OBJECT_0) {
                break;
            }
            QueryPerformanceCounter(&now);
            EnterCriticalSection(&g_momLock);
            active = MtpMomentumStep(&g_momentum, (float)((double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart), &h, &v);
            LeaveCriticalSection(&g_momLock);
            last = now;
            if (h != 0 || v != 0) {
                memset(&ev, 0, sizeof(ev));
                ev.type = MTP_EVENT_SCROLL;
                ev.dx = h;
                ev.dy = v;
                MtpInputSend(&ev);
            }
            if (!active) {
                break;
            }
        }
        wait[1] = g_momWake;
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }
    }
    if (timer != NULL) {
        CloseHandle(timer);
    }
    return 0;
}

static void SendEvents(const MTP_SETTINGS *s, const MTP_EVENT *ev, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (ev[i].type == MTP_EVENT_FORCE) {
            MTP_HAPTIC feel;
            MtpSettingsHaptic(s, &feel);
            MtpHapticOutPlay(&g_haptic, &feel);
            MtpActionRun((MTP_ACTION)s->force_action);
        } else if (ev[i].type == MTP_EVENT_SWIPE) {
            MtpActionRun(MtpSettingsSwipeAction(s, ev[i].fingers, ev[i].direction));
        } else if (ev[i].type == MTP_EVENT_DOUBLE_TAP) {
            MtpActionRun((MTP_ACTION)s->double_tap_action);
        } else if (ev[i].type == MTP_EVENT_FLING) {
            MomentumStart((float)ev[i].dx, (float)ev[i].dy);
        } else if (ev[i].type == MTP_EVENT_TOUCH) {
            MomentumStop(); /* 触れたら止める（Mac と同じ） */
        } else {
            MtpInputSend(&ev[i]);
        }
    }
}

static DWORD WINAPI WorkerMain(LPVOID param)
{
    MTP_DEVICE dev;
    int connected = 0;
    int wasPaused = 0;
    MTP_GESTURE gesture;
    MTP_GESTURE_CONFIG config;
    MTP_SETTINGS settings;
    LONG seenVersion;
    MTP_EVENT events[MTP_GESTURE_MAX_EVENTS];
    size_t n;

    UNREFERENCED_PARAMETER(param);
    memset(&dev, 0, sizeof(dev));
    seenVersion = InterlockedCompareExchange(&g_settingsVersion, 0, 0);
    GetSettings(&settings);
    MtpSettingsApply(&settings, &config);
    MtpGestureInit(&gesture, &config);
    MtpHapticOutInit(&g_haptic);
    EnterCriticalSection(&g_momLock);
    g_momentum.tau_ms = MtpSettingsGlideToTau(settings.inertia_glide);
    LeaveCriticalSection(&g_momLock);

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        UCHAR raw[1024];
        ULONG rawLength = 0;
        MTP_FRAME frame;
        MTP_READ_RESULT rr;
        DWORD err = 0;
        int paused;
        LONG version;

        if (InterlockedCompareExchange(&g_released, 0, 0)) {
            MomentumStop();
            if (connected) {
                n = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
                SendEvents(&settings, events, n);
                CloseDevice(&dev);
                connected = 0;
            }
            SetLinkState(LINK_RELEASED);
            WaitForSingleObject(g_stopEvent, 500);
            continue;
        }

        if (!connected) {
            MTP_DEVICE_STATUS ds = MtpDeviceOpen(&dev, &err);
            if (ds != MTP_DEVICE_OK) {
                SetLinkState(ds == MTP_DEVICE_NOT_FOUND ? LINK_NOT_FOUND : LINK_OPEN_FAILED);
                WaitForSingleObject(g_stopEvent, RECONNECT_MS);
                continue;
            }
            connected = 1;
            PublishDevice(&dev);
            MtpGestureInit(&gesture, &config);
            SetLinkState(LINK_CONNECTED);
        }

        rr = MtpDeviceRead(&dev, &frame, raw, sizeof(raw), &rawLength, &err);
        if (rr == MTP_READ_ERROR) {
            n = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
            SendEvents(&settings, events, n);
            CloseDevice(&dev);
            connected = 0;
            if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
                break; /* 終了時の中断で読み取りが失敗しただけ */
            }
            if (InterlockedCompareExchange(&g_released, 0, 0)) {
                continue; /* ロックで中断しただけ。すぐ休止に入る */
            }
            SetLinkState(LINK_DISCONNECTED);
            WaitForSingleObject(g_stopEvent, RECONNECT_MS);
            continue;
        }

        /*
         * 設定が変わっていたら写しを取り直す。判定の状態（押し込みの見極め中など）はそのまま残し、
         * 設定値だけを差し替える。見極め中の押し込みは最後まで見極める。
         */
        version = InterlockedCompareExchange(&g_settingsVersion, 0, 0);
        if (version != seenVersion) {
            seenVersion = version;
            GetSettings(&settings);
            MtpSettingsApply(&settings, &config);
            gesture.config = config;
            EnterCriticalSection(&g_momLock);
            g_momentum.tau_ms = MtpSettingsGlideToTau(settings.inertia_glide);
            LeaveCriticalSection(&g_momLock);
        }

        /* 一時停止に入った瞬間に、押したままのボタンを離す */
        paused = (int)InterlockedCompareExchange(&g_paused, 0, 0);
        if (paused && !wasPaused) {
            MomentumStop();
            n = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
            SendEvents(&settings, events, n);
        }
        if (!paused && wasPaused) {
            MtpGestureInit(&gesture, &config); /* 停止中の指の動きを引き継がない */
        }
        wasPaused = paused;

        if (rr != MTP_READ_FRAME || paused) {
            continue;
        }
        n = MtpGestureProcessAt(&gesture, &frame, (double)GetTickCount64(), events, MTP_GESTURE_MAX_EVENTS);
        SendEvents(&settings, events, n);
    }

    if (connected) {
        n = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
        SendEvents(&settings, events, n);
        CloseDevice(&dev);
    }
    MtpHapticOutClose(&g_haptic);
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
    case LINK_RELEASED: return L"ロック中のため休止しています";
    default: return L"起動中";
    }
}

/* タスクバーが明るい表示か（設定 → 個人用設定 → 色 の「既定の Windows モード」）。読めなければ暗いとみなす */
static int TaskbarIsLight(void)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return value != 0;
}

/* トラックパッドを使っているときは濃い虎、使っていないとき（未接続・一時停止・ロック中）は薄い虎 */
static HICON TrayIconFor(void)
{
    static HICON cache[4];
    static const WORD ids[4] = { IDI_TRAY_DARK, IDI_TRAY_LIGHT, IDI_TRAY_DARK_IDLE, IDI_TRAY_LIGHT_IDLE };
    int idle = InterlockedCompareExchange(&g_paused, 0, 0) ||
        (LINK_STATE)InterlockedCompareExchange(&g_state, 0, 0) != LINK_CONNECTED;
    int index = (idle ? 2 : 0) + (TaskbarIsLight() ? 1 : 0);
    if (cache[index] == NULL) {
        cache[index] = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(ids[index]), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    }
    return cache[index] != NULL ? cache[index] : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
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
    nid.hIcon = TrayIconFor();
    swprintf_s(nid.szTip, ARRAYSIZE(nid.szTip), APP_NAME L": %s", StateText());
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

/* 同じフォルダーの設定画面を開く */
static void OpenSettingsWindow(HWND hwnd)
{
    wchar_t path[MAX_PATH];
    wchar_t *slash;
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    slash = wcsrchr(path, L'\\');
    if (slash == NULL || (size_t)(slash - path) + 1 + wcslen(SETTINGS_EXE) >= MAX_PATH) {
        return;
    }
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash - path) - 1, SETTINGS_EXE);
    if ((INT_PTR)ShellExecuteW(hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        MessageBoxW(hwnd, L"設定画面（mtpsettings.exe）を開けませんでした。mtpagent.exe と同じフォルダーにあるか確認してください。",
            APP_NAME, MB_ICONWARNING | MB_OK);
    }
}

static void ShowMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    wchar_t status[160];
    MTP_SETTINGS s;

    GetSettings(&s);
    swprintf_s(status, ARRAYSIZE(status), L"状態: %s", StateText());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"設定...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (InterlockedCompareExchange(&g_paused, 0, 0) ? MF_CHECKED : 0), IDM_PAUSE, L"一時停止");
    AppendMenuW(menu, MF_STRING | (s.natural_scroll ? MF_CHECKED : 0), IDM_NATURAL, L"ナチュラルスクロール");
    AppendMenuW(menu, MF_STRING | (s.force_enabled ? MF_CHECKED : 0), IDM_FORCE, L"Force Touch");
    AppendMenuW(menu, MF_STRING | (s.gestures_enabled ? MF_CHECKED : 0), IDM_GESTURES, L"3・4本指ジェスチャー");
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
    case IDM_SETTINGS:
        OpenSettingsWindow(hwnd);
        break;
    case IDM_PAUSE:
        InterlockedExchange(&g_paused, !InterlockedCompareExchange(&g_paused, 0, 0));
        UpdateTray(NIM_MODIFY);
        break;
    case IDM_NATURAL:
        ToggleSetting(&g_settings.natural_scroll, L"NaturalScroll");
        break;
    case IDM_FORCE:
        ToggleSetting(&g_settings.force_enabled, L"ForceTouch");
        break;
    case IDM_GESTURES:
        ToggleSetting(&g_settings.gestures_enabled, L"Gestures");
        break;
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

/* この利用者のセッションがロックされているか（調べられなければ、ロックされていないとみなす） */
static int IsOwnSessionLocked(void)
{
    WTSINFOEXW *info = NULL;
    DWORD bytes = 0;
    int locked = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, (LPWSTR *)&info, &bytes) &&
        info != NULL && info->Level == 1) {
        /* Windows 10 以降: WTS_SESSIONSTATE_LOCK = 0 */
        locked = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    }
    if (info != NULL) {
        WTSFreeMemory(info);
    }
    return locked;
}

static void SetReleased(int released)
{
    InterlockedExchange(&g_released, released ? 1 : 0);
    if (released) {
        AbortPendingRead(); /* 読み取り待ちを中断して、すぐにハンドルを閉じさせる */
    }
}

static void OnSessionChange(WPARAM code)
{
    switch (code) {
    case WTS_SESSION_LOCK:
    case WTS_CONSOLE_DISCONNECT:
    case WTS_REMOTE_DISCONNECT:
    case WTS_SESSION_LOGOFF:
        SetReleased(1);
        break;
    case WTS_SESSION_UNLOCK:
    case WTS_SESSION_LOGON:
        SetReleased(0);
        break;
    case WTS_CONSOLE_CONNECT:
    case WTS_REMOTE_CONNECT:
        SetReleased(IsOwnSessionLocked()); /* つなぎ直した先がロック画面なら、解除を待つ */
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
    if (msg == g_msgSettingsChanged && g_msgSettingsChanged != 0) {
        ReloadSettings();
        return 0;
    }
    if (msg == g_msgHapticTest && g_msgHapticTest != 0) {
        /* 値の上限は MtpBuildActuatorReport が必ず丸める */
        MTP_HAPTIC h;
        h.strength = (uint8_t)(wParam & 0xFF);
        h.sharpness = (uint8_t)((wParam >> 8) & 0xFF);
        h.depth = (uint8_t)((wParam >> 16) & 0xFF);
        return MtpHapticOutPlay(&g_uiHaptic, &h) ? 1 : 0;
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
    case WM_SETTINGCHANGE:
        if (lParam != 0 && wcscmp((const wchar_t *)lParam, L"ImmersiveColorSet") == 0) {
            UpdateTray(NIM_MODIFY); /* 明るい表示・暗い表示が切り替わった */
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_WTSSESSION_CHANGE:
        OnSessionChange(wParam);
        return 0;
    case WM_COMMAND:
        OnCommand(hwnd, LOWORD(wParam));
        return 0;
    case WM_DESTROY:
        WTSUnRegisterSessionNotification(hwnd);
        SetEvent(g_stopEvent);
        AbortPendingRead(); /* 読み取り中なら中断して、すぐに停止要求を見させる */
        if (g_worker != NULL) {
            WaitForSingleObject(g_worker, 5000);
        }
        MtpHapticOutClose(&g_uiHaptic);
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
    UNREFERENCED_PARAMETER(show);

    mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (cmdLine != NULL && wcsstr(cmdLine, AUTOSTART_ARG) != NULL) {
            if (mutex) CloseHandle(mutex);
            return 0; /* 自動起動が重なっただけ。黙って終わる */
        }
        MessageBoxW(NULL, APP_NAME L" はすでに起動しています。タスクトレイのアイコンから操作してください。", APP_NAME,
            MB_ICONINFORMATION | MB_OK);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    InitializeCriticalSection(&g_devLock);
    InitializeCriticalSection(&g_momLock);
    MtpMomentumInit(&g_momentum);
    g_momWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&g_settingsLock);
    MtpSettingsLoad(&g_settings);
    MtpHapticOutInit(&g_uiHaptic);
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_msgSettingsChanged = RegisterWindowMessageW(MTP_MSG_SETTINGS_CHANGED);
    g_msgHapticTest = RegisterWindowMessageW(MTP_MSG_HAPTIC_TEST);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc)) {
        return 1;
    }
    /* 見えないウィンドウ（トレイの通知とメニュー、設定画面からの知らせの受け口） */
    g_hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, 0, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (g_hwnd == NULL || g_stopEvent == NULL) {
        return 1;
    }
    UpdateTray(NIM_ADD);
    /* ロックされたらトラックパッドを手放す（MtpSwitch サービスが標準の HID に切り替えられるように） */
    WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);
    InterlockedExchange(&g_released, IsOwnSessionLocked());

    g_worker = CreateThread(NULL, 0, WorkerMain, NULL, 0, NULL);
    g_momThread = CreateThread(NULL, 0, MomentumMain, NULL, 0, NULL);
    if (g_worker == NULL) {
        MessageBoxW(g_hwnd, L"読み取りスレッドを開始できませんでした。", APP_NAME, MB_ICONERROR | MB_OK);
        DestroyWindow(g_hwnd);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_worker) CloseHandle(g_worker);
    if (g_momThread) {
        WaitForSingleObject(g_momThread, 1000);
        CloseHandle(g_momThread);
    }
    if (g_momWake) CloseHandle(g_momWake);
    DeleteCriticalSection(&g_momLock);
    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_settingsLock);
    DeleteCriticalSection(&g_devLock);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
