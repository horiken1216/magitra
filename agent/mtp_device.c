/*
 * mtp_device.c - WinUSB 経由の接続・マルチタッチ切り替え・読み取り
 */
#include "mtp_device.h"

#include <cfgmgr32.h>
#include <initguid.h>
#include <string.h>

/* drivers/MtpUsb/MtpUsb.inf の DeviceInterfaceGUIDs と一致させる */
/* {A3F9C2E4-6B1D-4F58-8E27-5C0D9B41E7A6} */
DEFINE_GUID(GUID_DEVINTERFACE_MTP_WINUSB,
    0xa3f9c2e4, 0x6b1d, 0x4f58, 0x8e, 0x27, 0x5c, 0x0d, 0x9b, 0x41, 0xe7, 0xa6);

#define HID_REQ_SET_REPORT        0x09
#define HID_REPORT_TYPE_FEATURE   0x03
#define USB_REQ_TYPE_CLASS_IF_OUT 0x21

static HANDLE OpenWinUsbPath(void)
{
    WCHAR list[1024];
    GUID guid = GUID_DEVINTERFACE_MTP_WINUSB;

    if (CM_Get_Device_Interface_ListW(&guid, NULL, list, ARRAYSIZE(list), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        list[0] == L'\0') {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return INVALID_HANDLE_VALUE;
    }
    /* WinUSB は FILE_FLAG_OVERLAPPED で開いたハンドルを要求する */
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

MTP_DEVICE_STATUS MtpDeviceOpen(MTP_DEVICE *dev, DWORD *win32_error)
{
    ULONG timeout = MTP_DEVICE_READ_TIMEOUT_MS;

    memset(dev, 0, sizeof(*dev));
    dev->file = OpenWinUsbPath();
    if (dev->file == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (win32_error) *win32_error = err;
        dev->file = NULL;
        return err == ERROR_DEVICE_NOT_CONNECTED ? MTP_DEVICE_NOT_FOUND : MTP_DEVICE_OPEN_FAILED;
    }
    if (!WinUsb_Initialize(dev->file, &dev->usb)) {
        if (win32_error) *win32_error = GetLastError();
        CloseHandle(dev->file);
        memset(dev, 0, sizeof(*dev));
        return MTP_DEVICE_OPEN_FAILED;
    }
    dev->pipe = FindInterruptInPipe(dev->usb, &dev->interface_number);
    if (dev->pipe == 0 || !EnableMultitouch(dev->usb, dev->interface_number)) {
        if (win32_error) *win32_error = GetLastError();
        MtpDeviceClose(dev);
        return MTP_DEVICE_ENABLE_FAILED;
    }
    WinUsb_SetPipePolicy(dev->usb, dev->pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    if (win32_error) *win32_error = 0;
    return MTP_DEVICE_OK;
}

void MtpDeviceClose(MTP_DEVICE *dev)
{
    if (dev->usb != NULL) {
        WinUsb_Free(dev->usb);
    }
    if (dev->file != NULL && dev->file != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->file);
    }
    memset(dev, 0, sizeof(*dev));
}

MTP_READ_RESULT MtpDeviceRead(MTP_DEVICE *dev, MTP_FRAME *frame, UCHAR *raw, ULONG raw_capacity, ULONG *raw_length,
    DWORD *win32_error)
{
    ULONG n = 0;

    if (raw_length) *raw_length = 0;
    if (!WinUsb_ReadPipe(dev->usb, dev->pipe, raw, raw_capacity, &n, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) {
            return MTP_READ_TIMEOUT;
        }
        if (win32_error) *win32_error = err;
        return MTP_READ_ERROR;
    }
    if (raw_length) *raw_length = n;
    return MtpParseReport(MTP_TRANSPORT_USB, raw, n, frame) == MTP_OK ? MTP_READ_FRAME : MTP_READ_OTHER;
}
