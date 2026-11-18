/*
 * MtpBth.h - Magic Trackpad (Bluetooth) スパイクドライバーの内部定義
 *
 * BTHENUM の HID サービス PDO に機能ドライバーとして入り（hidbth の代わり）、
 * HID の L2CAP チャネル（制御 PSM 0x11、割り込み PSM 0x13）を自分で開く。
 * 割り込みチャネルの入力レポートをそのままユーザーモードへ渡し、実機でレポート形式を確かめる。
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <sdpnode.h>
#include <bthddi.h>

#include "Public.h"
#include "mtp_core.h"

#define MTP_POOL_TAG          'pbtM'
#define MTP_PSM_HID_CONTROL   0x0011
#define MTP_PSM_HID_INTERRUPT 0x0013
#define MTP_READER_COUNT      2
#define MTP_READ_BUFFER_SIZE  672      /* L2CAP 既定 MTU */
#define MTP_RECONNECT_SECONDS 3
#define MTP_RX_SLOTS          8        /* 読み取り要求が来るまでためておく件数 */
#define MTP_RX_SLOT_SIZE      160      /* 3 + 9×16 本ぶんより大きい */

/* HIDP（Bluetooth HID のトランザクション）ヘッダー */
#define HIDP_SET_REPORT_FEATURE 0x53   /* SET_REPORT(0x5) | Feature(0x3) */
#define HIDP_SET_REPORT_OUTPUT  0x52   /* SET_REPORT(0x5) | Output(0x2) */
#define HIDP_DATA_OUTPUT        0xA2   /* DATA(0xA) | Output(0x2) */
#define HIDP_DATA_INPUT         0xA1   /* DATA(0xA) | Input(0x1) */
#define HIDP_HANDSHAKE_MASK     0xF0
#define HIDP_HANDSHAKE          0x00

typedef struct _DEVICE_CONTEXT DEVICE_CONTEXT, *PDEVICE_CONTEXT;

typedef struct _MTP_READER {
    PDEVICE_CONTEXT Ctx;
    WDFREQUEST      Request;
    WDFMEMORY       BrbMemory;
    struct _BRB_L2CA_ACL_TRANSFER *Brb;
    UCHAR           Buffer[MTP_READ_BUFFER_SIZE];
} MTP_READER, *PMTP_READER;

struct _DEVICE_CONTEXT {
    WDFDEVICE       Device;
    WDFIOTARGET     IoTarget;
    WDFQUEUE        ReadQueue;       /* 手動キュー。ReadFile の要求をためる */
    WDFTIMER        ReconnectTimer;  /* パッシブレベル */
    WDFSPINLOCK     Lock;
    WDFWAITLOCK     ChannelLock;     /* 接続・切断・レポート送信を 1 つずつにする（パッシブレベル） */

    BTH_PROFILE_DRIVER_INTERFACE Profile;
    BOOLEAN         ProfileAcquired;
    BTH_ADDR        RemoteAddress;

    L2CAP_CHANNEL_HANDLE ControlChannel;
    L2CAP_CHANNEL_HANDLE InterruptChannel;

    WDFREQUEST      Readers[MTP_READER_COUNT];  /* 各要求のコンテキストが MTP_READER */
    LONG            ReadersOutstanding;
    KEVENT          ReadersIdle;     /* ReadersOutstanding が 0 のときシグナル */
    volatile LONG   Stopping;        /* 1 のとき読み取りを再投入しない（切断処理中） */
    volatile LONG   Active;          /* D0 にいる間だけ 1。0 なら再接続を予約しない */

    MTP_STATUS_INFO Status;          /* Lock で保護 */

    /* 読み取り要求がない間に届いたレポート（Lock で保護）。RxHead から RxCount 件 */
    UCHAR           RxData[MTP_RX_SLOTS][MTP_RX_SLOT_SIZE];
    USHORT          RxLength[MTP_RX_SLOTS];
    ULONG           RxHead;
    ULONG           RxCount;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, MtpGetContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MTP_READER, MtpGetReaderContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MtpEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE MtpEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE MtpEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY MtpEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT MtpEvtD0Exit;
EVT_WDF_TIMER MtpEvtReconnectTimer;
EVT_WDF_IO_QUEUE_IO_READ MtpEvtIoRead;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL MtpEvtIoDeviceControl;
EVT_WDF_REQUEST_COMPLETION_ROUTINE MtpReaderCompletion;

/* L2cap.c */
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS MtpQueryRemoteAddress(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS MtpConnect(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL) VOID MtpDisconnect(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_max_(DISPATCH_LEVEL) VOID MtpScheduleReconnect(_In_ PDEVICE_CONTEXT Ctx, _In_ NTSTATUS Reason);
_IRQL_requires_max_(DISPATCH_LEVEL) VOID MtpHandleRead(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS MtpSendReport(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Method,
    _In_reads_bytes_(Length) const UCHAR *Report, _In_ ULONG Length);
