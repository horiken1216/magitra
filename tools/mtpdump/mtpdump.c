/*
 * mtpdump.c - MtpBth から生フレームを読み、MtpCore で解析して表示する検証ツール
 *
 * 使い方:
 *   mtpdump            状態を表示してからフレームを表示し続ける（Ctrl+C で終了）
 *   mtpdump --status   状態だけ表示する
 *   mtpdump --hex      解析結果に加えて生バイト列も表示する
 *   mtpdump --haptic   振動レポートを 3 種類の送り方で 1 回ずつ（3 秒おきに）送り、結果を表示する
 */
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <stdio.h>
#include <string.h>

#include "../../drivers/MtpBth/Public.h"
#include "mtp_core.h"

static const char *LinkStateName(ULONG s)
{
    switch (s) {
    case MtpLinkIdle: return "idle";
    case MtpLinkConnecting: return "connecting";
    case MtpLinkConnected: return "connected";
    case MtpLinkDisconnected: return "disconnected";
    default: return "?";
    }
}

static HANDLE OpenMtpDevice(void)
{
    WCHAR list[1024];
    CONFIGRET cr;
    GUID guid = GUID_DEVINTERFACE_MTP_RAW;

    cr = CM_Get_Device_Interface_ListW(&guid, NULL, list, ARRAYSIZE(list),
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || list[0] == L'\0') {
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(list, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

/* どの送り方で鳴るかを確かめる。番号と名前を表示してから送るので、鳴ったときの番号を控えてもらう */
static int TestHaptic(HANDLE h)
{
    static const struct { UCHAR method; const char *name; } kMethods[] = {
        { MTP_SEND_CONTROL_FEATURE, "control SET_REPORT(Feature)" },
        { MTP_SEND_CONTROL_OUTPUT, "control SET_REPORT(Output)" },
        { MTP_SEND_INTERRUPT_OUTPUT, "interrupt DATA(Output)" },
    };
    MTP_HAPTIC haptic = MTP_HAPTIC_BASELINE;
    uint8_t report[MTP_ACTUATOR_BT_REPORT_LENGTH];
    UCHAR in[1 + MTP_ACTUATOR_BT_REPORT_LENGTH];
    MTP_STATUS_INFO st;
    DWORD got;
    size_t i;

    haptic.strength = 0x30; /* 体感しやすいように上限まで上げる（上限は core が丸める） */
    MtpBuildActuatorReportBt(&haptic, report, sizeof(report), NULL);
    memcpy(in + 1, report, sizeof(report));
    for (i = 0; i < ARRAYSIZE(kMethods); i++) {
        BOOL ok;
        DWORD err;
        printf("[%zu] %s ... ", i + 1, kMethods[i].name);
        fflush(stdout);
        in[0] = kMethods[i].method;
        ok = DeviceIoControl(h, MTP_IOCTL_SET_REPORT, in, sizeof(in), NULL, 0, &got, NULL);
        err = ok ? 0 : GetLastError();
        got = 0;
        if (DeviceIoControl(h, MTP_IOCTL_GET_STATUS, NULL, 0, &st, sizeof(st), &got, NULL) && got == sizeof(st)) {
            printf("%s (err=%lu, handshake=0x%lX)\n", ok ? "sent" : "failed", err, st.SetReportHandshake);
        } else {
            printf("%s (err=%lu)\n", ok ? "sent" : "failed", err);
        }
        Sleep(3000);
    }
    return 0;
}

static BOOL PrintStatus(HANDLE h)
{
    MTP_STATUS_INFO st;
    DWORD got = 0;

    if (!DeviceIoControl(h, MTP_IOCTL_GET_STATUS, NULL, 0, &st, sizeof(st), &got, NULL) || got != sizeof(st)) {
        printf("status IOCTL failed: %lu\n", GetLastError());
        return FALSE;
    }
    printf("link=%s last=0x%08lX attempts=%lu handshake=0x%lX recv=%lu delivered=%lu dropped=%lu addr=%012llX\n",
        LinkStateName(st.LinkState), (unsigned long)st.LastNtStatus, st.ConnectAttempts, st.EnableHandshake,
        st.FramesReceived, st.FramesDelivered, st.FramesDropped, st.RemoteAddress);
    return TRUE;
}

int main(int argc, char **argv)
{
    BOOL statusOnly = FALSE;
    BOOL hex = FALSE;
    BOOL haptic = FALSE;
    HANDLE h;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--status") == 0) statusOnly = TRUE;
        else if (strcmp(argv[i], "--hex") == 0) hex = TRUE;
        else if (strcmp(argv[i], "--haptic") == 0) haptic = TRUE;
    }

    h = OpenMtpDevice();
    if (h == INVALID_HANDLE_VALUE) {
        printf("MtpBth device interface not found (driver not installed or device not started)\n");
        return 2;
    }
    if (!PrintStatus(h)) {
        CloseHandle(h);
        return 1;
    }
    if (statusOnly || haptic) {
        i = haptic ? TestHaptic(h) : 0;
        CloseHandle(h);
        return i;
    }

    for (;;) {
        UCHAR buf[1024];
        DWORD n = 0;
        MTP_FRAME frame;
        MTP_STATUS ps;

        if (!ReadFile(h, buf, sizeof(buf), &n, NULL)) {
            printf("ReadFile failed: %lu\n", GetLastError());
            break;
        }
        if (hex) {
            DWORD k;
            printf("raw[%lu]:", n);
            for (k = 0; k < n; k++) printf(" %02X", buf[k]);
            printf("\n");
        }
        ps = MtpParseReport(MTP_TRANSPORT_BT, buf, n, &frame);
        if (ps != MTP_OK) {
            printf("non-touch report id=0x%02X len=%lu (parse=%d)\n", n ? buf[0] : 0, n, (int)ps);
            continue;
        }
        printf("btn=%u n=%u", frame.button, frame.contact_count);
        for (i = 0; i < frame.contact_count; i++) {
            const MTP_CONTACT *c = &frame.contacts[i];
            printf(" | id=%u x=%5d y=%5d p=%3u maj=%3u min=%3u sz=%3u st=%u o=%d",
                c->id, c->x, c->y, c->pressure, c->touch_major, c->touch_minor, c->size, c->state, c->orientation);
        }
        printf("\n");
    }
    CloseHandle(h);
    return 0;
}
