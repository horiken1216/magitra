/*
 * mtpagent.c - Magic Trackpad 2 (USB, WinUSB 経由) の常駐アプリ
 *
 * 生フレームを読み、mtp_gesture でポインター移動・クリック・スクロールに変換し、SendInput で Windows に渡す。
 * 前提: drivers/MtpUsb/MtpUsb.inf で MI_01 に WinUSB を当ててあること（docs/INSTALL-USB.md）。
 *
 * 使い方:
 *   mtpagent [--seconds N] [--dry-run] [--verbose] [--no-natural] [--pointer-gain F] [--scroll-gain F]
 *     --seconds N      N 秒で自動終了する（試験用。省略時は Ctrl+C まで動く）
 *     --dry-run        入力を Windows に送らず、作ったイベントを表示するだけ
 *     --verbose        送ったイベントも表示する
 *     --no-natural     スクロール方向を従来方向にする（既定は Mac と同じナチュラル）
 *     --pointer-gain F ポインターの基本倍率（既定 0.12）
 *     --scroll-gain F  スクロールの倍率（既定 0.6）
 *     --log FILE       フレームごとの時刻・指の状態と座標・作ったイベントを FILE に書く（調査用）
 *
 * 注意: 管理者権限で動いているウィンドウには、権限なしの mtpagent からは入力を送れない（Windows の仕様）。
 */
#include <windows.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp_core.h"
#include "mtp_gesture.h"

/* drivers/MtpUsb/MtpUsb.inf の DeviceInterfaceGUIDs と一致させる */
/* {A3F9C2E4-6B1D-4F58-8E27-5C0D9B41E7A6} */
DEFINE_GUID(GUID_DEVINTERFACE_MTP_WINUSB,
    0xa3f9c2e4, 0x6b1d, 0x4f58, 0x8e, 0x27, 0x5c, 0x0d, 0x9b, 0x41, 0xe7, 0xa6);

#define HID_REQ_SET_REPORT        0x09
#define HID_REPORT_TYPE_FEATURE   0x03
#define USB_REQ_TYPE_CLASS_IF_OUT 0x21
#define READ_TIMEOUT_MS           200

static volatile LONG g_stop = 0;

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
    unsigned long reads;       /* 成功した読み取り */
    unsigned long frames;      /* マルチタッチのフレーム */
    unsigned long others;      /* それ以外のレポート */
    unsigned long timeouts;    /* 入力がなくタイムアウトした回数 */
    unsigned long events;
    double max_gap_ms;         /* 連続して触れている間のフレーム間隔の最大（200ms 未満の間隔だけ数える） */
    double sum_gap_ms;
    unsigned long gaps;
} STATS;

static void ParseOptions(int argc, char **argv, OPTIONS *opt)
{
    int i;
    opt->seconds = -1;
    opt->dry_run = 0;
    opt->verbose = 0;
    opt->log_path = NULL;
    MtpGestureDefaultConfig(&opt->gesture);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) opt->seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--dry-run") == 0) opt->dry_run = 1;
        else if (strcmp(argv[i], "--verbose") == 0) opt->verbose = 1;
        else if (strcmp(argv[i], "--no-natural") == 0) opt->gesture.natural_scroll = 0;
        else if (strcmp(argv[i], "--pointer-gain") == 0 && i + 1 < argc) opt->gesture.pointer_gain = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--scroll-gain") == 0 && i + 1 < argc) opt->gesture.scroll_gain = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) opt->log_path = argv[++i];
    }
}

static HANDLE OpenWinUsbPath(void)
{
    WCHAR list[1024];
    GUID guid = GUID_DEVINTERFACE_MTP_WINUSB;

    if (CM_Get_Device_Interface_ListW(&guid, NULL, list, ARRAYSIZE(list), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        list[0] == L'\0') {
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(list, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
}

static UCHAR FindInterruptInPipe(WINUSB_INTERFACE_HANDLE usb, UCHAR *interfaceNumber)
{
    USB_INTERFACE_DESCRIPTOR ifd;
    UCHAR i;

    if (!WinUsb_QueryInterfaceSettings(usb, 0, &ifd)) {
        return 0;
    }
    *interfaceNumber = ifd.bInterfaceNumber;
    for (i = 0; i < ifd.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pipe;
        if (WinUsb_QueryPipe(usb, 0, i, &pipe) && pipe.PipeType == UsbdPipeTypeInterrupt &&
            USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            return pipe.PipeId;
        }
    }
    return 0;
}

static BOOL EnableMultitouch(WINUSB_INTERFACE_HANDLE usb, UCHAR interfaceNumber)
{
    const uint8_t *report;
    size_t length;
    UCHAR data[8];
    WINUSB_SETUP_PACKET setup;
    ULONG sent = 0;

    if (MtpGetEnableReport(MTP_TRANSPORT_USB, &report, &length) != MTP_OK || length > sizeof(data)) {
        return FALSE;
    }
    memcpy(data, report, length);
    setup.RequestType = USB_REQ_TYPE_CLASS_IF_OUT;
    setup.Request = HID_REQ_SET_REPORT;
    setup.Value = (USHORT)((HID_REPORT_TYPE_FEATURE << 8) | report[0]);
    setup.Index = interfaceNumber;
    setup.Length = (USHORT)length;
    return WinUsb_ControlTransfer(usb, setup, data, (ULONG)length, &sent, NULL);
}

static void PrintEventTo(FILE *out, const char *prefix, const MTP_EVENT *ev)
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
    default:
        break;
    }
}

static void PrintEvent(const char *prefix, const MTP_EVENT *ev)
{
    PrintEventTo(stdout, prefix, ev);
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

/* イベントを SendInput の INPUT に変換する。戻り値は書いた件数（0..2） */
static UINT ToInputs(const MTP_EVENT *ev, INPUT *in)
{
    memset(in, 0, sizeof(INPUT) * 2);
    in[0].type = INPUT_MOUSE;
    in[1].type = INPUT_MOUSE;
    switch (ev->type) {
    case MTP_EVENT_MOVE:
        in[0].mi.dx = ev->dx;
        in[0].mi.dy = ev->dy;
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
        return 1;
    case MTP_EVENT_BUTTON:
        if (ev->button == MTP_BUTTON_RIGHT) {
            in[0].mi.dwFlags = ev->down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        } else {
            in[0].mi.dwFlags = ev->down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        }
        return 1;
    case MTP_EVENT_SCROLL: {
        UINT k = 0;
        if (ev->dy != 0) {
            in[k].mi.dwFlags = MOUSEEVENTF_WHEEL;
            in[k].mi.mouseData = (DWORD)ev->dy;
            k++;
        }
        if (ev->dx != 0) {
            in[k].mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in[k].mi.mouseData = (DWORD)ev->dx;
            k++;
        }
        return k;
    }
    default:
        return 0;
    }
}

static void Emit(const OPTIONS *opt, const MTP_EVENT *events, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        INPUT in[2];
        UINT n;
        if (opt->dry_run) {
            PrintEvent("[dry-run]", &events[i]);
            continue;
        }
        n = ToInputs(&events[i], in);
        if (n > 0 && SendInput(n, in, sizeof(INPUT)) != n) {
            printf("SendInput failed: %lu\n", GetLastError());
        }
        if (opt->verbose) {
            PrintEvent("[sent]", &events[i]);
        }
    }
}

int main(int argc, char **argv)
{
    OPTIONS opt;
    HANDLE file;
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR pipe, interfaceNumber = 0;
    ULONG timeout = READ_TIMEOUT_MS;
    MTP_GESTURE gesture;
    MTP_EVENT events[MTP_GESTURE_MAX_EVENTS];
    ULONGLONG start;
    LARGE_INTEGER qpf, qpc0, qpcLast;
    STATS stats;
    FILE *log = NULL;
    int haveLast = 0;
    int exitCode = 0;

    ParseOptions(argc, argv, &opt);
    memset(&stats, 0, sizeof(stats));
    qpcLast.QuadPart = 0;
    QueryPerformanceFrequency(&qpf);
    if (opt.log_path != NULL && fopen_s(&log, opt.log_path, "w") != 0) {
        printf("ログファイルを開けません: %s\n", opt.log_path);
        return 2;
    }
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    file = OpenWinUsbPath();
    if (file == INVALID_HANDLE_VALUE) {
        printf("MtpUsb (WinUSB) が見つかりません。drivers/MtpUsb/MtpUsb.inf を導入し、USB で接続してください。\n");
        return 2;
    }
    if (!WinUsb_Initialize(file, &usb)) {
        printf("WinUsb_Initialize failed: %lu\n", GetLastError());
        CloseHandle(file);
        return 1;
    }
    pipe = FindInterruptInPipe(usb, &interfaceNumber);
    if (pipe == 0 || !EnableMultitouch(usb, interfaceNumber)) {
        printf("割り込みパイプの取得か、マルチタッチモードへの切り替えに失敗しました: %lu\n", GetLastError());
        exitCode = 1;
        goto done;
    }
    WinUsb_SetPipePolicy(usb, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    MtpGestureInit(&gesture, &opt.gesture);
    printf("mtpagent 開始（%s%s、ナチュラルスクロール=%s、Ctrl+C で終了）\n",
        opt.dry_run ? "dry-run" : "入力を送る",
        opt.seconds >= 0 ? "、時間制限あり" : "",
        opt.gesture.natural_scroll ? "オン" : "オフ");

    start = GetTickCount64();
    QueryPerformanceCounter(&qpc0);
    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        UCHAR buf[1024];
        ULONG n = 0;
        MTP_FRAME frame;
        size_t count;
        LARGE_INTEGER now;
        double t_ms;

        if (opt.seconds >= 0 && (double)(GetTickCount64() - start) / 1000.0 >= opt.seconds) {
            break;
        }
        if (!WinUsb_ReadPipe(usb, pipe, buf, sizeof(buf), &n, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT) {
                stats.timeouts++;
                haveLast = 0; /* 入力が途切れた。次のフレームとの間隔は数えない */
                continue;
            }
            printf("読み取りに失敗しました（切断された可能性があります）: %lu\n", err);
            exitCode = 1;
            break;
        }
        stats.reads++;
        QueryPerformanceCounter(&now);
        t_ms = (double)(now.QuadPart - qpc0.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        if (MtpParseReport(MTP_TRANSPORT_USB, buf, n, &frame) != MTP_OK) {
            stats.others++;
            if (log) fprintf(log, "%9.1f other id=0x%02X len=%lu\n", t_ms, n ? buf[0] : 0, n);
            continue; /* マルチタッチ以外（切り替え直後のマウス用レポートなど） */
        }
        stats.frames++;
        if (haveLast) {
            double gap = (double)(now.QuadPart - qpcLast.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            if (gap < READ_TIMEOUT_MS) {
                stats.sum_gap_ms += gap;
                stats.gaps++;
                if (gap > stats.max_gap_ms) stats.max_gap_ms = gap;
            }
        }
        qpcLast = now;
        haveLast = 1;

        count = MtpGestureProcess(&gesture, &frame, events, MTP_GESTURE_MAX_EVENTS);
        stats.events += (unsigned long)count;
        if (log) {
            size_t k;
            LogFrame(log, t_ms, &frame);
            for (k = 0; k < count; k++) PrintEventTo(log, "          ->", &events[k]);
        }
        Emit(&opt, events, count);
    }

    /* 押したままのボタンを必ず離す */
    {
        size_t count = MtpGestureRelease(&gesture, events, MTP_GESTURE_MAX_EVENTS);
        Emit(&opt, events, count);
    }
    printf("mtpagent 終了: 読み取り %lu（フレーム %lu、その他 %lu）、タイムアウト %lu、イベント %lu\n",
        stats.reads, stats.frames, stats.others, stats.timeouts, stats.events);
    if (stats.gaps > 0) {
        printf("  触れている間のフレーム間隔: 平均 %.1f ms（約 %.0f フレーム/秒）、最大 %.1f ms\n",
            stats.sum_gap_ms / (double)stats.gaps, 1000.0 * (double)stats.gaps / stats.sum_gap_ms, stats.max_gap_ms);
    }

done:
    if (log) fclose(log);
    WinUsb_Free(usb);
    CloseHandle(file);
    return exitCode;
}
