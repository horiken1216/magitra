/*
 * mtp_device.c - 接続・マルチタッチ切り替え・読み取り（USB は WinUSB、Bluetooth は MtpBth）
 */
#include "mtp_device.h"

#include <winioctl.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <string.h>

#include "../drivers/MtpBth/Public.h"

/* drivers/MtpUsb/MtpUsb.inf の DeviceInterfaceGUIDs と一致させる */
/* {A3F9C2E4-6B1D-4F58-8E27-5C0D9B41E7A6} */
DEFINE_GUID(GUID_DEVINTERFACE_MTP_WINUSB,
    0xa3f9c2e4, 0x6b1d, 0x4f58, 0x8e, 0x27, 0x5c, 0x0d, 0x9b, 0x41, 0xe7, 0xa6);

#define HID_REQ_SET_REPORT        0x09
#define HID_REPORT_TYPE_FEATURE   0x03
#define USB_REQ_TYPE_CLASS_IF_OUT 0x21

/* デバイスインターフェイスを 1 つ開く。見つからなければ ERROR_DEVICE_NOT_CONNECTED */
static HANDLE OpenInterface(const GUID *guid)
{
    WCHAR list[1024];
    GUID g = *guid;

    if (CM_Get_Device_Interface_ListW(&g, NULL, list, ARRAYSIZE(list), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        list[0] == L'\0') {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return INVALID_HANDLE_VALUE;
    }
    /* WinUSB は FILE_FLAG_OVERLAPPED で開いたハンドルを要求する。MtpBth も時間切れを付けて読むため OVERLAPPED で開く */
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
        SetLastError(ERROR_INTERNAL_ERROR);
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

static MTP_DEVICE_STATUS OpenUsb(MTP_DEVICE *dev, DWORD *win32_error)
{
    ULONG timeout = MTP_DEVICE_READ_TIMEOUT_MS;

    dev->transport = MTP_TRANSPORT_USB;
    dev->file = OpenInterface(&GUID_DEVINTERFACE_MTP_WINUSB);
    if (dev->file == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (win32_error) *win32_error = err;
        dev->file = NULL;
        return err == ERROR_DEVICE_NOT_CONNECTED ? MTP_DEVICE_NOT_FOUND : MTP_DEVICE_OPEN_FAILED;
    }
    if (!WinUsb_Initialize(dev->file, &dev->usb)) {
        if (win32_error) *win32_error = GetLastError();
        MtpDeviceClose(dev);
        return MTP_DEVICE_OPEN_FAILED;
    }
    dev->pipe = FindInterruptInPipe(dev->usb, &dev->interface_number);
    if (dev->pipe == 0 || !EnableMultitouch(dev->usb, dev->interface_number)) {
        if (win32_error) *win32_error = GetLastError();
        MtpDeviceClose(dev);
        return MTP_DEVICE_ENABLE_FAILED;
    }
    WinUsb_SetPipePolicy(dev->usb, dev->pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    return MTP_DEVICE_OK;
}

static MTP_DEVICE_STATUS OpenBluetooth(MTP_DEVICE *dev, DWORD *win32_error)
{
    dev->transport = MTP_TRANSPORT_BT;
    dev->file = OpenInterface(&GUID_DEVINTERFACE_MTP_RAW);
    if (dev->file == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (win32_error) *win32_error = err;
        dev->file = NULL;
        return err == ERROR_DEVICE_NOT_CONNECTED ? MTP_DEVICE_NOT_FOUND : MTP_DEVICE_OPEN_FAILED;
    }
    dev->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (dev->read_event == NULL) {
        if (win32_error) *win32_error = GetLastError();
        MtpDeviceClose(dev);
        return MTP_DEVICE_OPEN_FAILED;
    }
    return MTP_DEVICE_OK;
}

MTP_DEVICE_STATUS MtpDeviceOpen(MTP_DEVICE *dev, DWORD *win32_error)
{
    MTP_DEVICE_STATUS st;

    memset(dev, 0, sizeof(*dev));
    st = OpenUsb(dev, win32_error);
    if (st == MTP_DEVICE_NOT_FOUND) {
        memset(dev, 0, sizeof(*dev));
        st = OpenBluetooth(dev, win32_error);
    }
    if (st == MTP_DEVICE_OK && win32_error) {
        *win32_error = 0;
    }
    return st;
}

void MtpDeviceClose(MTP_DEVICE *dev)
{
    if (dev->usb != NULL) {
        WinUsb_Free(dev->usb);
    }
    if (dev->file != NULL && dev->file != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->file);
    }
    if (dev->read_event != NULL) {
        CloseHandle(dev->read_event);
    }
    memset(dev, 0, sizeof(*dev));
}

void MtpDeviceAbortRead(MTP_DEVICE *dev)
{
    if (dev->transport == MTP_TRANSPORT_USB && dev->usb != NULL) {
        WinUsb_AbortPipe(dev->usb, dev->pipe);
    } else if (dev->transport == MTP_TRANSPORT_BT && dev->file != NULL) {
        CancelIoEx(dev->file, NULL);
    }
}

/* MtpBth から 1 件読む。MTP_DEVICE_READ_TIMEOUT_MS 待っても届かなければ取り消して時間切れにする */
static MTP_READ_RESULT ReadBluetooth(MTP_DEVICE *dev, UCHAR *raw, ULONG raw_capacity, ULONG *n, DWORD *win32_error)
{
    OVERLAPPED ov;
    DWORD got = 0;
    DWORD err;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = dev->read_event;
    ResetEvent(dev->read_event);
    if (!ReadFile(dev->file, raw, raw_capacity, NULL, &ov)) {
        err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            if (win32_error) *win32_error = err;
            return MTP_READ_ERROR;
        }
        if (WaitForSingleObject(dev->read_event, MTP_DEVICE_READ_TIMEOUT_MS) == WAIT_TIMEOUT) {
            CancelIoEx(dev->file, &ov);
        }
    }
    /* 取り消しと完了が行き違った場合は、届いたレポートをそのまま使う */
    if (!GetOverlappedResult(dev->file, &ov, &got, TRUE)) {
        err = GetLastError();
        if (err == ERROR_OPERATION_ABORTED) {
            return MTP_READ_TIMEOUT;
        }
        if (win32_error) *win32_error = err;
        return MTP_READ_ERROR;
    }
    *n = got;
    return MTP_READ_FRAME;
}

MTP_READ_RESULT MtpDeviceRead(MTP_DEVICE *dev, MTP_FRAME *frame, UCHAR *raw, ULONG raw_capacity, ULONG *raw_length,
    DWORD *win32_error)
{
    ULONG n = 0;

    if (raw_length) *raw_length = 0;
    if (dev->transport == MTP_TRANSPORT_BT) {
        MTP_READ_RESULT rr = ReadBluetooth(dev, raw, raw_capacity, &n, win32_error);
        if (rr != MTP_READ_FRAME) {
            return rr;
        }
    } else if (!WinUsb_ReadPipe(dev->usb, dev->pipe, raw, raw_capacity, &n, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) {
            return MTP_READ_TIMEOUT;
        }
        if (win32_error) *win32_error = err;
        return MTP_READ_ERROR;
    }
    if (raw_length) *raw_length = n;
    return MtpParseReport(dev->transport, raw, n, frame) == MTP_OK ? MTP_READ_FRAME : MTP_READ_OTHER;
}
