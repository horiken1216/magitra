/*
 * mtpswitchsvc.c - ロック中・ログオン前だけ、Magic Trackpad を Windows 標準の HID に戻すサービス（MagitraSwitch）
 *
 * WinUSB を当てた MI_01 は Windows のマウスとして扱われないため、常駐アプリが動かないログオン画面・ロック画面では
 * トラックパッドが使えない。このサービスは、その間だけ MI_01 を標準の HidUsb に戻し、普通のマウスとして使えるようにする
 * （ポインター移動・左右のクリック。ジェスチャーなどは使えない）。
 *
 * Bluetooth も同じで、テスト署名モードで MtpBth を入れているときは、その間だけ Windows 標準の HidBth に戻す
 * （MtpBth が入っていなければ Bluetooth の機器には触らない）。切り替えの処理は agent/mtp_switch.c。
 *
 * あるべき状態:
 *   コンソールのセッションにユーザーがログオンしていて、ロックされていない → MAGITRA（WinUSB / MtpBth。常駐アプリがフル機能で使う）
 *   それ以外（起動直後のログオン画面、ロック中、ログオフ、ユーザーの切り替え中） → 標準の HID
 * シャットダウン中は切り替えない。PnP がドライバーの導入を受け付けず、約 1 分待たされた末に
 * ERROR_NO_CONFIGMGR_SERVICES で失敗し、再起動を遅らせた（実機、2026-11-16）。WinUSB のまま再起動しても、
 * 起動時にこのサービスがすぐ標準の HID に戻す（実機で、サービスの開始から約 5 秒）。
 *
 * 切り替えには 2〜5 秒かかる（実機、2026-11-16）。切り替えは作業スレッドで 1 つずつ行い、知らせが重なったら最新の状態に合わせる。
 * トラックパッドを挿し直すと Windows は WinUSB を優先して当てるので、機器の到着の知らせでも見直す。念のため 30 秒ごとにも見直す。
 * 記録: %ProgramData%\MAGITRA\switch.log（1 MB を超えたら .old に回す）
 *
 * 使い方: サービスとして登録して使う（インストーラーが登録する）。
 *   mtpswitchsvc.exe --once   今の状態に一度だけ合わせて終わる（試験用。管理者権限で）
 */
#include <windows.h>
#include <dbt.h>
#include <initguid.h>
#include <usbiodef.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <aclapi.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include "mtp_switch.h"

#define SERVICE_NAME      L"MagitraSwitch"
#define RECHECK_MS        30000
#define DEBOUNCE_MS       500
/*
 * ログオフの後は、切り替える前にこれだけ待つ。再起動・シャットダウンでもまずログオフが起きるが、
 * その最中の切り替えは PnP に断られ、約 16 秒待たされてシャットダウンを遅らせる（実機、2026-11-16）。
 * 待つ間にシャットダウンの知らせ（SERVICE_CONTROL_SHUTDOWN）が来れば、切り替えずに終わる。
 */
#define LOGOFF_GRACE_MS   5000
#define LOG_MAX_BYTES     (1024 * 1024)

static SERVICE_STATUS_HANDLE g_statusHandle;
static SERVICE_STATUS g_status;
static HANDLE g_wake;      /* 見直しの合図 */
static HANDLE g_stop;      /* 停止の合図 */
static volatile LONG g_loggedOff; /* 前回の見直しの後にログオフがあった */
static volatile LONG g_logonSession = -1; /* ログオンがあったセッション（常駐アプリを起動する）。なければ -1 */
static HDEVNOTIFY g_devNotify;
static CRITICAL_SECTION g_logLock;
static BOOL g_console = FALSE; /* --once のときは画面にも出す */

/* ---- 記録 ---- */

static void LogPath(wchar_t *path, DWORD len, BOOL old)
{
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH) == 0) {
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    }
    swprintf_s(path, len, L"%s\\MAGITRA\\switch.log%s", base, old ? L".old" : L"");
}

/*
 * 記録のフォルダーは、管理者と SYSTEM だけが書けるようにする（継承を切る）。%ProgramData% の既定の権限では
 * 一般ユーザーも中にファイルやリンクを作れるので、SYSTEM で書く記録を別のファイルへ向けられてしまう。
 * 一般ユーザーが先に作ったフォルダー（所有者が SYSTEM・Administrators でない）やリンクになったフォルダーには書かない。
 * インストーラーも同じ権限で作る（setup-core.ps1 の Initialize-LogDirectory）。
 */
#define LOG_DIR_SDDL L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)"

static int g_logDirState; /* 0 = まだ見ていない、1 = 使える、-1 = 使わない */

static BOOL PrepareLogDir(const wchar_t *dir)
{
    PSECURITY_DESCRIPTOR sd = NULL, current = NULL;
    SECURITY_ATTRIBUTES sa;
    PSID owner = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE, ok = FALSE;
    DWORD attr;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(LOG_DIR_SDDL, SDDL_REVISION_1, &sd, NULL)) {
        return FALSE;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    if (!CreateDirectoryW(dir, &sa) && GetLastError() != ERROR_ALREADY_EXISTS) {
        goto done;
    }
    attr = GetFileAttributesW(dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        goto done;
    }
    if (GetNamedSecurityInfoW(dir, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &current) != ERROR_SUCCESS ||
        owner == NULL ||
        (!IsWellKnownSid(owner, WinLocalSystemSid) && !IsWellKnownSid(owner, WinBuiltinAdministratorsSid))) {
        goto done;
    }
    /* 前の版が既定の権限で作ったフォルダーでも、ここで権限を絞り直す */
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present ||
        SetNamedSecurityInfoW((LPWSTR)dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            NULL, NULL, dacl, NULL) != ERROR_SUCCESS) {
        goto done;
    }
    ok = TRUE;
done:
    if (current != NULL) {
        LocalFree(current);
    }
    LocalFree(sd);
    return ok;
}

/* 開いた記録がリンク（リパースポイント・ハードリンク）でなければ TRUE */
static BOOL IsPlainFile(HANDLE f)
{
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileInformationByHandle(f, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        info.nNumberOfLinks == 1;
}

static void WriteLog(const wchar_t *message)
{
    wchar_t path[MAX_PATH], dir[MAX_PATH], line[1200];
    char utf8[3600];
    SYSTEMTIME st;
    HANDLE f;
    int n;
    LARGE_INTEGER size;

    GetLocalTime(&st);
    swprintf_s(line, ARRAYSIZE(line), L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);
    if (g_console) {
        wprintf(L"%s", line);
    }
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 1) {
        return;
    }

    EnterCriticalSection(&g_logLock);
    LogPath(path, MAX_PATH, FALSE);
    wcscpy_s(dir, MAX_PATH, path);
    *wcsrchr(dir, L'\\') = L'\0';
    if (g_logDirState == 0) {
        g_logDirState = PrepareLogDir(dir) ? 1 : -1;
    }
    if (g_logDirState < 0) {
        LeaveCriticalSection(&g_logLock);
        return;
    }
    /* リンクをたどらずに開き、リンクだったら書かない */
    f = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (f != INVALID_HANDLE_VALUE && !IsPlainFile(f)) {
        CloseHandle(f);
        f = INVALID_HANDLE_VALUE;
    }
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written;
        BOOL rotate = GetFileSizeEx(f, &size) && size.QuadPart > LOG_MAX_BYTES;
        WriteFile(f, utf8, (DWORD)(n - 1), &written, NULL);
        CloseHandle(f);
        if (rotate) {
            wchar_t old[MAX_PATH];
            LogPath(old, MAX_PATH, TRUE);
            MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
        }
    }
    LeaveCriticalSection(&g_logLock);
}

static void LogF(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, ARRAYSIZE(buf), fmt, ap);
    va_end(ap);
    WriteLog(buf);
}

/* ---- あるべき状態 ---- */

/* コンソールのセッションにユーザーがログオンしていて、ロックされていなければ TRUE */
static BOOL ConsoleUserActiveAndUnlocked(void)
{
    DWORD session = WTSGetActiveConsoleSessionId();
    WTSINFOEXW *info = NULL;
    DWORD bytes = 0;
    BOOL result = FALSE;

    if (session == 0xFFFFFFFF) {
        return FALSE; /* コンソールにセッションがない（切り替え中など） */
    }
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, (LPWSTR *)&info, &bytes) ||
        info == NULL) {
        return FALSE;
    }
    if (info->Level == 1) {
        WTSINFOEX_LEVEL1_W *l1 = &info->Data.WTSInfoExLevel1;
        /* Windows 10 以降: WTS_SESSIONSTATE_UNLOCK = 1、WTS_SESSIONSTATE_LOCK = 0 */
        result = l1->SessionState == WTSActive && l1->UserName[0] != L'\0' && l1->SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    }
    WTSFreeMemory(info);
    return result;
}

static MTP_DRIVER_MODE DesiredMode(void)
{
    return ConsoleUserActiveAndUnlocked() ? MTP_MODE_MAGITRA : MTP_MODE_HID;
}

static const wchar_t *ModeName(MTP_DRIVER_MODE m)
{
    return m == MTP_MODE_HID ? L"標準の HID" : m == MTP_MODE_MAGITRA ? L"MAGITRA（WinUSB / MtpBth）" : L"不明";
}

/* 今の状態をあるべき状態に合わせる */
static void Reconcile(const wchar_t *reason)
{
    MTP_DRIVER_MODE desired = DesiredMode();
    if (MtpSwitchIsSettled(desired)) {
        return; /* 対象の機器がない場合も含む */
    }
    LogF(L"%s: あるべき状態は %s", reason, ModeName(desired));
    MtpSwitchTo(desired, WriteLog);
}

/* ---- ログオン時の常駐アプリの起動 ---- */

/*
 * Windows の自動起動（Run）は、登録されたアプリを 1 つずつ順に起動するので、ほかのアプリが多いと
 * 常駐アプリの番が来るまで 40 秒以上かかる（実機、2026-11-16）。そこで、ログオンを知った時点でこのサービスが起動する。
 * 起動するのは、利用者が「ログオン時に起動」を選んでいる（Run に MAGITRA がある）ときだけ。
 * あとで Run からも起動されるが、/autostart 付きなので、2 つ目は何も出さずに終わる。
 */

/* 利用者の Run に MAGITRA があるか（HKEY_USERS\<SID> から読む） */
static BOOL UserWantsAutostart(HANDLE token)
{
    BYTE buf[256];
    DWORD len = 0;
    LPWSTR sid = NULL;
    wchar_t key[512];
    BOOL wants = FALSE;

    if (!GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len) ||
        !ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid)) {
        return FALSE;
    }
    swprintf_s(key, ARRAYSIZE(key), L"%s\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", sid);
    wants = RegGetValueW(HKEY_USERS, key, L"MAGITRA", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
    LocalFree(sid);
    return wants;
}

/* インストーラーが UIAccess 版を入れたか */
static BOOL InstalledAsUiAccess(void)
{
    wchar_t mode[32];
    DWORD size = sizeof(mode);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MAGITRA\\Installer", L"Mode", RRF_RT_REG_SZ, NULL, mode, &size) == ERROR_SUCCESS &&
        _wcsicmp(mode, L"UIAccess") == 0;
}

/* 常駐アプリの起動の結果。LAUNCH_RETRY は一時的な失敗（少し後でもう一度試す） */
typedef enum { LAUNCH_DONE, LAUNCH_RETRY } LAUNCH_RESULT;

static LAUNCH_RESULT LaunchAgent(DWORD session)
{
    HANDLE user = NULL, primary = NULL;
    LPVOID env = NULL;
    wchar_t dir[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH + 32];
    wchar_t *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD n;
    LAUNCH_RESULT result;

    n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || (slash = wcsrchr(dir, L'\\')) == NULL) {
        return LAUNCH_DONE;
    }
    *slash = L'\0';
    swprintf_s(exe, MAX_PATH, L"%s\\mtpagent.exe", dir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        return LAUNCH_DONE;
    }
    if (!WTSQueryUserToken(session, &user)) {
        LogF(L"常駐アプリを起動できません（利用者の資格を得られない、エラー %lu）", GetLastError());
        return LAUNCH_RETRY; /* ログオンの直後はまだ得られないことがある */
    }
    if (!UserWantsAutostart(user)) {
        CloseHandle(user);
        return LAUNCH_DONE; /* 「ログオン時に起動」を選んでいない */
    }
    if (!DuplicateTokenEx(user, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &primary)) {
        CloseHandle(user);
        return LAUNCH_RETRY;
    }
    CloseHandle(user);
    if (InstalledAsUiAccess()) {
        /*
         * uiAccess="true" の実行ファイルは、UIAccess の付いた資格でないと起動できない（ERROR_ELEVATION_REQUIRED）。
         * 普段は Windows（AppInfo）が付けるが、ここではシステム権限のこのサービスが付ける。
         * 付けるのは、インストーラーが署名して Program Files に置いた UIAccess 版だけ。
         */
        DWORD on = 1;
        if (!SetTokenInformation(primary, TokenUIAccess, &on, sizeof(on))) {
            LogF(L"常駐アプリを起動できません（UIAccess を付けられない、エラー %lu）", GetLastError());
            CloseHandle(primary);
            return LAUNCH_DONE; /* 権限の問題なので、繰り返しても変わらない */
        }
    }
    if (!CreateEnvironmentBlock(&env, primary, FALSE)) {
        env = NULL;
    }
    swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\" /autostart", exe);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = (LPWSTR)L"winsta0\\default";
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessAsUserW(primary, exe, cmd, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT | NORMAL_PRIORITY_CLASS,
            env, dir, &si, &pi)) {
        LogF(L"常駐アプリを起動しました（セッション %lu）", session);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        result = LAUNCH_DONE;
    } else {
        LogF(L"常駐アプリを起動できませんでした（エラー %lu）", GetLastError());
        result = LAUNCH_RETRY;
    }
    if (env != NULL) {
        DestroyEnvironmentBlock(env);
    }
    CloseHandle(primary);
    return result;
}

/* ---- 作業スレッド ---- */

/*
 * ログオンの知らせ（WTS_SESSION_LOGON）は、ログオンから 1 分近く遅れて届くことがあった（実機、2026-11-16）。
 * そこで 1 秒ごとにコンソールのセッションを見て、利用者が使い始めたら（ログオン・ロック解除）すぐ見直す。
 * 使えなくなった側（ロック・ログオフ）は知らせで扱う（ログオフ後の待ちを効かせるため）。
 */
#define POLL_MS 1000

#define LAUNCH_MAX_TRIES 10 /* 一時的な失敗のとき、1 秒おきに試す回数 */

/* 作業スレッドだけが使う */
static DWORD g_launchedSession = 0xFFFFFFFF; /* 常駐アプリの起動を済ませたセッション（同じセッションでは一度だけ） */
static DWORD g_retrySession = 0xFFFFFFFF;    /* 起動に一時的に失敗し、もう一度試すセッション */
static int g_retryCount;

static void LaunchAgentOnce(DWORD session)
{
    if (session == 0xFFFFFFFF || session == g_launchedSession) {
        return;
    }
    if (session != g_retrySession) {
        g_retrySession = session;
        g_retryCount = 0;
    }
    if (LaunchAgent(session) == LAUNCH_RETRY && ++g_retryCount < LAUNCH_MAX_TRIES) {
        return; /* 作業スレッドの見回り（1 秒ごと）で、もう一度試す */
    }
    if (g_retryCount >= LAUNCH_MAX_TRIES) {
        LogF(L"常駐アプリの起動をあきらめました（セッション %lu、%d 回）", session, g_retryCount);
    }
    g_launchedSession = session;
    g_retrySession = 0xFFFFFFFF;
}

/* ログオフしたら記録を消す（同じセッション番号が次のログオンで使い回されても、起動できるように） */
static void ForgetLaunchedSession(void)
{
    g_launchedSession = 0xFFFFFFFF;
    g_retrySession = 0xFFFFFFFF;
}

static DWORD WINAPI Worker(LPVOID param)
{
    HANDLE handles[2];
    BOOL wasActive;
    ULONGLONG lastFull;
    UNREFERENCED_PARAMETER(param);
    handles[0] = g_stop;
    handles[1] = g_wake;

    /* サービスの開始時（導入直後など）に、すでに使っている人の常駐アプリは起動しない */
    wasActive = ConsoleUserActiveAndUnlocked();
    if (wasActive) {
        g_launchedSession = WTSGetActiveConsoleSessionId();
    }
    Reconcile(L"開始");
    lastFull = GetTickCount64();
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, POLL_MS);
        BOOL active;
        if (w == WAIT_OBJECT_0) {
            break;
        }
        if (w == WAIT_OBJECT_0 + 1) {
            /* 知らせが立て続けに来ることがある（ロック直後の解除など）。少し待って最新の状態に合わせる */
            if (WaitForSingleObject(g_stop, DEBOUNCE_MS) == WAIT_OBJECT_0) {
                break;
            }
            ResetEvent(g_wake);
            if (InterlockedCompareExchange(&g_loggedOff, 0, 0)) {
                ForgetLaunchedSession(); /* ログオフとログオンの知らせが一緒に来たとき、ログオンの起動を止めない */
            }
            {
                LONG session = InterlockedExchange(&g_logonSession, -1);
                if (session >= 0) {
                    LaunchAgentOnce((DWORD)session);
                }
            }
            if (InterlockedExchange(&g_loggedOff, 0)) {
                if (WaitForSingleObject(g_stop, LOGOFF_GRACE_MS) == WAIT_OBJECT_0) {
                    break; /* シャットダウン中だった */
                }
                if (GetSystemMetrics(SM_SHUTTINGDOWN)) {
                    LogF(L"シャットダウン中なので切り替えません");
                    wasActive = ConsoleUserActiveAndUnlocked();
                    continue;
                }
            }
            Reconcile(L"知らせ");
            wasActive = ConsoleUserActiveAndUnlocked();
            lastFull = GetTickCount64();
            continue;
        }
        active = ConsoleUserActiveAndUnlocked();
        if (active && !wasActive) {
            /* 常駐アプリの起動を先に。常駐アプリは WinUSB になるまで待って自動でつながる */
            LaunchAgentOnce(WTSGetActiveConsoleSessionId());
            Reconcile(L"使い始めを検出");
            lastFull = GetTickCount64();
        } else if (active && g_retrySession != 0xFFFFFFFF && g_retrySession == WTSGetActiveConsoleSessionId()) {
            LaunchAgentOnce(g_retrySession); /* 前回の一時的な失敗のやり直し */
        } else if (GetTickCount64() - lastFull >= RECHECK_MS) {
            Reconcile(L"定期の見直し");
            lastFull = GetTickCount64();
        }
        wasActive = active;
    }
    return 0;
}

/* ---- サービス ---- */

static void SetState(DWORD state, DWORD waitHint)
{
    g_status.dwCurrentState = state;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE)
        : 0;
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        g_status.dwCheckPoint = 0;
    } else {
        g_status.dwCheckPoint++;
    }
    SetServiceStatus(g_statusHandle, &g_status);
}

static DWORD WINAPI Handler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context)
{
    UNREFERENCED_PARAMETER(context);
    switch (control) {
    case SERVICE_CONTROL_STOP:
        LogF(L"停止の指示");
        SetState(SERVICE_STOP_PENDING, 5000);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_SHUTDOWN:
        /* シャットダウン中は切り替えない（PnP に断られ、待たされるだけ）。次の起動時に合わせる */
        LogF(L"シャットダウンの知らせ");
        SetState(SERVICE_STOP_PENDING, 3000);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        switch (eventType) {
        case WTS_SESSION_LOCK: LogF(L"ロック"); break;
        case WTS_SESSION_UNLOCK: LogF(L"ロック解除"); break;
        case WTS_SESSION_LOGON:
            LogF(L"ログオン");
            if (eventData != NULL) {
                InterlockedExchange(&g_logonSession, (LONG)((WTSSESSION_NOTIFICATION *)eventData)->dwSessionId);
            }
            break;
        case WTS_SESSION_LOGOFF: LogF(L"ログオフ"); InterlockedExchange(&g_loggedOff, 1); break;
        case WTS_CONSOLE_CONNECT: LogF(L"コンソールに接続"); break;
        case WTS_CONSOLE_DISCONNECT: LogF(L"コンソールから切断"); break;
        default: break;
        }
        SetEvent(g_wake);
        return NO_ERROR;
    case SERVICE_CONTROL_DEVICEEVENT:
        if (eventType == DBT_DEVICEARRIVAL) {
            SetEvent(g_wake); /* 挿し直しで WinUSB が当たっていることがある */
        }
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    HANDLE worker;
    DEV_BROADCAST_DEVICEINTERFACE_W filter;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_statusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, Handler, NULL);
    if (g_statusHandle == NULL) {
        return;
    }
    memset(&g_status, 0, sizeof(g_status));
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    SetState(SERVICE_START_PENDING, 3000);

    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_stop == NULL || g_wake == NULL) {
        g_status.dwWin32ExitCode = GetLastError();
        SetState(SERVICE_STOPPED, 0);
        return;
    }

    memset(&filter, 0, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;
    g_devNotify = RegisterDeviceNotificationW(g_statusHandle, &filter, DEVICE_NOTIFY_SERVICE_HANDLE);

    LogF(L"サービスを開始しました");
    worker = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    SetState(SERVICE_RUNNING, 0);

    WaitForSingleObject(g_stop, INFINITE);
    if (worker != NULL) {
        /* 切り替え中なら終わるのを待つ。シャットダウン中は待たない（切り替えは断られるだけ） */
        WaitForSingleObject(worker, g_status.dwWaitHint == 3000 ? 1000 : 15000);
        CloseHandle(worker);
    }
    if (g_devNotify != NULL) {
        UnregisterDeviceNotification(g_devNotify);
    }
    LogF(L"サービスを停止しました");
    SetState(SERVICE_STOPPED, 0);
}

int wmain(int argc, wchar_t **argv)
{
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SERVICE_NAME, ServiceMain }, { NULL, NULL } };

    InitializeCriticalSection(&g_logLock);
    _wsetlocale(LC_ALL, L".UTF8");
    if (argc >= 2 && _wcsicmp(argv[1], L"--once") == 0) {
        MTP_DRIVER_MODE desired;
        g_console = TRUE;
        SetConsoleOutputCP(CP_UTF8);
        desired = DesiredMode();
        LogF(L"--once: 今は %s、あるべき状態は %s", ModeName(MtpSwitchCurrentMode()), ModeName(desired));
        Reconcile(L"--once");
        return 0;
    }
    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            SetConsoleOutputCP(CP_UTF8);
            wprintf(L"これはサービスとして動くプログラムです。インストーラーで導入してください（試験は --once）。\n");
        }
        return 1;
    }
    return 0;
}
