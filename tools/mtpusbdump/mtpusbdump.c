/*
 * mtpusbdump.c - WinUSB 経由で Magic Trackpad 2 (USB, MI_01) の生レポートを読み、MtpCore で解析して表示する
 *
 * 前提: drivers/MtpUsb/MtpUsb.inf で MI_01 に WinUSB を当ててあること（docs/INSTALL-USB.md）。
 *
 * 使い方:
 *   mtpusbdump               マルチタッチモードへ切り替えてからレポートを表示し続ける（Ctrl+C で終了）
 *   mtpusbdump --no-enable   切り替えを送らずに表示する（既定モードで何が流れるかの確認用）
 *   mtpusbdump --hex         解析結果に加えて生バイト列も表示する
 *   mtpusbdump --count N     N 件読んだら終了する
 */
#include <windows.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp_core.h"

/* drivers/MtpUsb/MtpUsb.inf の DeviceInterfaceGUIDs と一致させる */
/* {A3F9C2E4-6B1D-4F58-8E27-5C0D9B41E7A6} */
DEFINE_GUID(GUID_DEVINTERFACE_MTP_WINUSB,
    0xa3f9c2e4, 0x6b1d, 0x4f58, 0x8e, 0x27, 0x5c, 0x0d, 0x9b, 0x41, 0xe7, 0xa6);

/* HID クラス要求 */
#define HID_REQ_SET_REPORT        0x09
#define HID_REPORT_TYPE_FEATURE   0x03
#define USB_REQ_TYPE_CLASS_IF_OUT 0x21  /* Host-to-device | Class | Interface */

static HANDLE OpenWinUsbPath(void)
{
    WCHAR list[1024];
    GUID guid = GUID_DEVINTERFACE_MTP_WINUSB;
    CONFIGRET cr;

    cr = CM_Get_Device_Interface_ListW(&guid, NULL, list, ARRAYSIZE(list), CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || list[0] == L'\0') {
        return INVALID_HANDLE_VALUE;
    }
    /* WinUSB は FILE_FLAG_OVERLAPPED で開いたハンドルを要求する */
    return CreateFileW(list, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
}

static const char *PipeTypeName(USBD_PIPE_TYPE t)
{
    switch (t) {
    case UsbdPipeTypeControl: return "control";
    case UsbdPipeTypeIsochronous: return "isoch";
    case UsbdPipeTypeBulk: return "bulk";
    case UsbdPipeTypeInterrupt: return "interrupt";
    default: return "?";
    }
}

/* インターフェイスの全パイプを表示し、割り込み IN パイプを返す（なければ 0） */
static UCHAR FindInterruptInPipe(WINUSB_INTERFACE_HANDLE usb, UCHAR *interfaceNumber)
{
    USB_INTERFACE_DESCRIPTOR ifd;
    UCHAR found = 0;
    UCHAR i;

    if (!WinUsb_QueryInterfaceSettings(usb, 0, &ifd)) {
        printf("WinUsb_QueryInterfaceSettings failed: %lu\n", GetLastError());
        return 0;
    }
    *interfaceNumber = ifd.bInterfaceNumber;
    printf("interface %u: class=0x%02X subclass=0x%02X protocol=0x%02X endpoints=%u\n",
        ifd.bInterfaceNumber, ifd.bInterfaceClass, ifd.bInterfaceSubClass, ifd.bInterfaceProtocol,
        ifd.bNumEndpoints);

    for (i = 0; i < ifd.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pipe;
        if (!WinUsb_QueryPipe(usb, 0, i, &pipe)) {
            continue;
        }
        printf("  pipe 0x%02X %s %s maxpacket=%u interval=%u\n", pipe.PipeId, PipeTypeName(pipe.PipeType),
            USB_ENDPOINT_DIRECTION_IN(pipe.PipeId) ? "IN" : "OUT", pipe.MaximumPacketSize, pipe.Interval);
        if (found == 0 && pipe.PipeType == UsbdPipeTypeInterrupt && USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            found = pipe.PipeId;
        }
    }
    return found;
}

/* HID SET_REPORT(Feature) をインターフェイス宛ての制御転送で送る。report[0] が Report ID */
static BOOL SendFeatureReport(WINUSB_INTERFACE_HANDLE usb, UCHAR interfaceNumber, const uint8_t *report, size_t length)
{
    WINUSB_SETUP_PACKET setup;
    UCHAR data[64];
    ULONG sent = 0;

    if (length == 0 || length > sizeof(data)) {
        return FALSE;
    }
    memcpy(data, report, length);
    setup.RequestType = USB_REQ_TYPE_CLASS_IF_OUT;
    setup.Request = HID_REQ_SET_REPORT;
    setup.Value = (USHORT)((HID_REPORT_TYPE_FEATURE << 8) | report[0]);
    setup.Index = interfaceNumber;
    setup.Length = (USHORT)length;

    if (!WinUsb_ControlTransfer(usb, setup, data, (ULONG)length, &sent, NULL)) {
        printf("SET_REPORT failed: %lu\n", GetLastError());
        return FALSE;
    }
    printf("SET_REPORT feature 0x%02X (%zu bytes) sent=%lu\n", report[0], length, sent);
    return TRUE;
}

static void PrintFrame(const MTP_FRAME *frame)
{
    int i;
    printf("btn=%u n=%u%s", frame->button, frame->contact_count, frame->truncated ? "+" : "");
    for (i = 0; i < frame->contact_count; i++) {
        const MTP_CONTACT *c = &frame->contacts[i];
        printf(" | id=%u x=%5d y=%5d p=%3u maj=%3u min=%3u sz=%3u st=%u o=%d",
            c->id, c->x, c->y, c->pressure, c->touch_major, c->touch_minor, c->size, c->state, c->orientation);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    BOOL enable = TRUE;
    BOOL hex = FALSE;
    long maxCount = -1;
    long count = 0;
    HANDLE file;
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR pipe;
    UCHAR interfaceNumber = 0;
    ULONG timeout = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-enable") == 0) enable = FALSE;
        else if (strcmp(argv[i], "--hex") == 0) hex = TRUE;
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) maxCount = strtol(argv[++i], NULL, 10);
    }

    file = OpenWinUsbPath();
    if (file == INVALID_HANDLE_VALUE) {
        printf("MtpUsb (WinUSB) interface not found. Is drivers/MtpUsb/MtpUsb.inf installed and the trackpad connected by USB?\n");
        return 2;
    }
    if (!WinUsb_Initialize(file, &usb)) {
        printf("WinUsb_Initialize failed: %lu\n", GetLastError());
        CloseHandle(file);
        return 1;
    }

    pipe = FindInterruptInPipe(usb, &interfaceNumber);
    if (pipe == 0) {
        printf("no interrupt IN pipe found\n");
        goto done;
    }
    /* 0 = タイムアウトなし（入力が来るまで待つ） */
    WinUsb_SetPipePolicy(usb, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    if (enable) {
        const uint8_t *report;
        size_t length;
        if (MtpGetEnableReport(MTP_TRANSPORT_USB, &report, &length) != MTP_OK ||
            !SendFeatureReport(usb, interfaceNumber, report, length)) {
            goto done;
        }
    }

    printf("reading pipe 0x%02X ... (Ctrl+C to stop)\n", pipe);
    while (maxCount < 0 || count < maxCount) {
        UCHAR buf[4096];
        ULONG n = 0;
        MTP_FRAME frame;
        MTP_STATUS ps;

        if (!WinUsb_ReadPipe(usb, pipe, buf, sizeof(buf), &n, NULL)) {
            printf("WinUsb_ReadPipe failed: %lu\n", GetLastError());
            break;
        }
        count++;
        if (hex) {
            ULONG k;
            printf("raw[%lu]:", n);
            for (k = 0; k < n; k++) printf(" %02X", buf[k]);
            printf("\n");
        }
        ps = MtpParseReport(MTP_TRANSPORT_USB, buf, n, &frame);
        if (ps == MTP_OK) {
            PrintFrame(&frame);
        } else {
            printf("report id=0x%02X len=%lu (not a multitouch frame, parse=%d)\n", n ? buf[0] : 0, n, (int)ps);
        }
    }

done:
    WinUsb_Free(usb);
    CloseHandle(file);
    return 0;
}
