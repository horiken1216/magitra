/*
 * mtpagent_cli.c - mtpagent のコンソール版（調査・試験用）
 *
 * 普段使いはタスクトレイ常駐版（mtpagent_tray.c → mtpagent.exe）。こちらは結果を画面やファイルに出して確かめる用途。
 *
 * 使い方:
 *   mtpagent-cli [--seconds N] [--dry-run] [--verbose] [--no-natural] [--pointer-gain F] [--scroll-gain F] [--log FILE]
 *     --seconds N      N 秒で自動終了する（省略時は Ctrl+C まで動く）
 *     --dry-run        入力を Windows に送らず、作ったイベントを表示するだけ
 *     --verbose        送ったイベントも表示する
 *     --no-natural     スクロール方向を従来方向にする（既定は Mac と同じナチュラル）
 *     --pointer-gain F ポインターの基本倍率（既定 0.12）
 *     --scroll-gain F  スクロールの倍率（既定 0.6）
 *     --log FILE       フレームごとの時刻・指の状態と座標・作ったイベントを FILE に書く
 *     --no-force       Force Touch を使わない（押した瞬間に左ボタンを送る）
 *     --no-gestures    3・4 本指のスワイプを使わない
 *     --defaults       保存された設定（設定画面で変えた値）を使わず、既定値で動かす（試験の再現用）
 *
 * 起動時に保存された設定（HKCU\Software\MTP\Agent、mtp_settings.h）を読み、上のオプションで上書きする。
 * Force Touch が成立すると、Actuator で振動させてから割り当てた操作（既定は中クリック）を送る。
 * --dry-run でも振動は鳴らす（Windows への入力ではないので、手応えだけ確かめられる）。中クリックは送らない。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp_actions.h"
#include "mtp_core.h"
#include "mtp_device.h"
#include "mtp_gesture.h"
#include "mtp_haptic_out.h"
#include "mtp_input.h"
#include "mtp_settings.h"

static volatile LONG g_stop = 0;
static MTP_HAPTIC_OUT g_haptic;
static MTP_SETTINGS g_settings;

static BOOL WINAPI OnConsoleCtrl(DWORD type)
{
    UNREFERENCED_PARAMETER(type);
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

typedef struct OPTIONS {
    double seconds;     /* < 0 で無制限 */
    int dry_run;
    int verbose;
    const char *log_path;
    MTP_GESTURE_CONFIG gesture;
} OPTIONS;

typedef struct STATS {
    unsigned long reads;
    unsigned long frames;
    unsigned long others;
    unsigned long timeouts;
    unsigned long events;
    double max_gap_ms;  /* 触れている間のフレーム間隔の最大（タイムアウトをまたぐ間隔は数えない） */
    double sum_gap_ms;
    unsigned long gaps;
} STATS;

static void ParseOptions(int argc, char **argv, OPTIONS *opt)
{
    int i;
    int useDefaults = 0;
    memset(opt, 0, sizeof(*opt));
    opt->seconds = -1;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--defaults") == 0) useDefaults = 1;
    }
    if (useDefaults) {
        MtpSettingsDefaults(&g_settings);
    } else {
        MtpSettingsLoad(&g_settings);
    }
    MtpSettingsApply(&g_settings, &opt->gesture);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) opt->seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--dry-run") == 0) opt->dry_run = 1;
        else if (strcmp(argv[i], "--verbose") == 0) opt->verbose = 1;
        else if (strcmp(argv[i], "--no-natural") == 0) opt->gesture.natural_scroll = 0;
        else if (strcmp(argv[i], "--pointer-gain") == 0 && i + 1 < argc) opt->gesture.pointer_gain = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--scroll-gain") == 0 && i + 1 < argc) opt->gesture.scroll_gain = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) opt->log_path = argv[++i];
        else if (strcmp(argv[i], "--no-force") == 0) opt->gesture.force_enabled = 0;
        else if (strcmp(argv[i], "--no-gestures") == 0) opt->gesture.swipe_enabled = 0;
    }
}

static void PrintEvent(FILE *out, const char *prefix, const MTP_EVENT *ev)
{
    switch (ev->type) {
    case MTP_EVENT_MOVE:
        fprintf(out, "%s move dx=%d dy=%d\n", prefix, ev->dx, ev->dy);
        break;
    case MTP_EVENT_BUTTON:
        fprintf(out, "%s %s %s\n", prefix, ev->button == MTP_BUTTON_RIGHT ? "right" : "left", ev->down ? "down" : "up");
        break;
    case MTP_EVENT_SCROLL:
        fprintf(out, "%s scroll h=%d v=%d\n", prefix, ev->dx, ev->dy);
        break;
    case MTP_EVENT_FORCE:
        fprintf(out, "%s force\n", prefix);
        break;
    case MTP_EVENT_SWIPE: {
        static const char *dirs[] = { "左", "右", "上", "下" };
        fprintf(out, "%s swipe %d本指 %s → %s\n", prefix, ev->fingers, dirs[ev->direction & 3],
            MtpActionName(MtpSettingsSwipeAction(&g_settings, ev->fingers, ev->direction)));
        break;
    }
    case MTP_EVENT_FLING:
        fprintf(out, "%s fling h=%d/s v=%d/s\n", prefix, ev->dx, ev->dy);
        break;
    case MTP_EVENT_TOUCH:
        fprintf(out, "%s touch\n", prefix);
        break;
    case MTP_EVENT_ZOOM:
        fprintf(out, "%s zoom %d\n", prefix, ev->dy);
        break;
    case MTP_EVENT_ZOOM_END:
        fprintf(out, "%s zoom end\n", prefix);
        break;
    case MTP_EVENT_DOUBLE_TAP:
        fprintf(out, "%s double tap → %s\n", prefix, MtpActionName((MTP_ACTION)g_settings.double_tap_action));
        break;
    default:
        break;
    }
}

static void LogFrame(FILE *log, double t_ms, const MTP_FRAME *f)
{
    int i;
    fprintf(log, "%9.1f n=%u btn=%u", t_ms, f->contact_count, f->button);
    for (i = 0; i < f->contact_count; i++) {
        const MTP_CONTACT *c = &f->contacts[i];
        fprintf(log, " | id=%u st=%u maj=%u x=%d y=%d p=%u", c->id, c->state, c->touch_major, c->x, c->y, c->pressure);
    }
    fprintf(log, "\n");
}

static void Emit(const OPTIONS *opt, const MTP_EVENT *events, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (events[i].type == MTP_EVENT_FORCE) {
            MTP_HAPTIC feel;
            SYSTEMTIME st;
            BOOL played;
            MtpSettingsHaptic(&g_settings, &feel);
            played = MtpHapticOutPlay(&g_haptic, &feel);
            GetLocalTime(&st);
            printf("[haptic] %02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                played ? "振動を送った" : "振動を鳴らせなかった（Actuator が見つからないか、間隔が短すぎる）");
        }
        if (opt->dry_run) {
            PrintEvent(stdout, "[dry-run]", &events[i]);
            continue;
        }
        if (events[i].type == MTP_EVENT_FORCE) {
            if (!MtpActionRun((MTP_ACTION)g_settings.force_action)) {
                printf("SendInput failed: %lu\n", GetLastError());
            }
        } else if (events[i].type == MTP_EVENT_DOUBLE_TAP) {
            PrintEvent(stdout, "[gesture]", &events[i]);
            MtpActionRun((MTP_ACTION)g_settings.double_tap_action);
        } else if (events[i].type == MTP_EVENT_SWIPE) {
            PrintEvent(stdout, "[gesture]", &events[i]);
            if (!MtpActionRun(MtpSettingsSwipeAction(&g_settings, events[i].fingers, events[i].direction))) {
                printf("SendInput failed: %lu\n", GetLastError());
            }
        } else if (!MtpInputSend(&events[i])) {
            printf("SendInput failed: %lu\n", GetLastError());
        }
        if (opt->verbose) {
            PrintEvent(stdout, "[sent]", &events[i]);
        }
    }
}

int main(int argc, char **argv)
{
    OPTIONS opt;
    MTP_DEVICE dev;
    MTP_DEVICE_STATUS ds;
    DWORD err = 0;
    MTP_GESTURE gesture;
    MTP_EVENT events[MTP_GESTURE_MAX_EVENTS];
    ULONGLONG start;
    LARGE_INTEGER qpf, qpc0, qpcLast;
    STATS stats;
    FILE *log = NULL;
    int haveLast = 0;
    int exitCode = 0;

    ParseOptions(argc, argv, &opt);
    MtpHapticOutInit(&g_haptic);
    memset(&stats, 0, sizeof(stats));
    qpcLast.QuadPart = 0;
    QueryPerformanceFrequency(&qpf);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    if (opt.log_path != NULL && fopen_s(&log, opt.log_path, "w") != 0) {
        printf("ログファイルを開けません: %s\n", opt.log_path);
        return 2;
    }

    ds = MtpDeviceOpen(&dev, &err);
    if (ds != MTP_DEVICE_OK) {
        if (ds == MTP_DEVICE_NOT_FOUND) {
            printf("MtpUsb (WinUSB) が見つかりません。drivers/MtpUsb/MtpUsb.inf を導入し、USB で接続してください。\n");
        } else {
            printf("デバイスを開けませんでした（状態 %d、エラー %lu）。タスクトレイ版の mtpagent が動いていないか確認してください。\n",
                (int)ds, err);
        }
        if (log) fclose(log);
        return 2;
    }

    MtpGestureInit(&gesture, &opt.gesture);
    printf("mtpagent-cli 開始（%s%s、ナチュラルスクロール=%s、Force Touch=%s、3・4本指ジェスチャー=%s、Ctrl+C で終了）\n",
        opt.dry_run ? "dry-run" : "入力を送る",
        opt.seconds >= 0 ? "、時間制限あり" : "",
        opt.gesture.natural_scroll ? "オン" : "オフ",
        opt.gesture.force_enabled ? "オン" : "オフ",
        opt.gesture.swipe_enabled ? "オン" : "オフ");

    start = GetTickCount64();
    QueryPerformanceCounter(&qpc0);
    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        UCHAR raw[1024];
        ULONG n = 0;
        MTP_FRAME frame;
        MTP_READ_RESULT rr;
        size_t count;
        LARGE_INTEGER now;
        double t_ms;

        if (opt.seconds >= 0 && (double)(GetTickCount64() - start) / 1000.0 >= opt.seconds) {
            break;
        }
        rr = MtpDeviceRead(&dev, &frame, raw, sizeof(raw), &n, &err);
        if (rr == MTP_READ_TIMEOUT) {
            stats.timeouts++;
            haveLast = 0;
            continue;
        }
        if (rr == MTP_READ_ERROR) {
            printf("読み取りに失敗しました（切断された可能性があります）: %lu\n", err);
            exitCode = 1;
            break;
        }
        stats.reads++;
        QueryPerformanceCounter(&now);
        t_ms = (double)(now.QuadPart - qpc0.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        if (rr == MTP_READ_OTHER) {
            stats.others++;
            if (log) fprintf(log, "%9.1f other id=0x%02X len=%lu\n", t_ms, n ? raw[0] : 0, n);
            continue;
        }
        stats.frames++;
        if (haveLast) {
            double gap = (double)(now.QuadPart - qpcLast.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            stats.sum_gap_ms += gap;
            stats.gaps++;
            if (gap > stats.max_gap_ms) stats.max_gap_ms = gap;
        }
        qpcLast = now;
        haveLast = 1;

        count = MtpGestureProcess(&gesture, &frame, events, MTP_GESTURE_MAX_EVENTS);
        stats.events += (unsigned long)count;
        if (log) {
            size_t k;
            LogFrame(log, t_ms, &frame);
            for (k = 0; k < count; k++) PrintEvent(log, "          ->", &events[k]);
        }
        Emit(&opt, events, count);
    }

    /* 押したままのボタンを必ず離す */
    {
        size_t count = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
        Emit(&opt, events, count);
    }
    printf("mtpagent-cli 終了: 読み取り %lu（フレーム %lu、その他 %lu）、タイムアウト %lu、イベント %lu\n",
        stats.reads, stats.frames, stats.others, stats.timeouts, stats.events);
    if (stats.gaps > 0 && stats.sum_gap_ms > 0) {
        printf("  触れている間のフレーム間隔: 平均 %.1f ms（約 %.0f フレーム/秒）、最大 %.1f ms\n",
            stats.sum_gap_ms / (double)stats.gaps, 1000.0 * (double)stats.gaps / stats.sum_gap_ms, stats.max_gap_ms);
    }

    if (log) fclose(log);
    MtpHapticOutClose(&g_haptic);
    MtpDeviceClose(&dev);
    return exitCode;
}
