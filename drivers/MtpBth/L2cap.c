/*
 * L2cap.c - HID の L2CAP チャネル確立、マルチタッチ有効化、割り込みチャネルの連続読み取り
 *
 * 接続はホスト側から張る（制御 PSM 0x11 → 割り込み PSM 0x13 の順。HID 仕様の順序）。
 * デバイス側からの着信接続は受けない。デバイスがスリープで切断した場合は、
 * MTP_RECONNECT_SECONDS ごとに張り直しを試みる（スパイクの割り切り）。
 */
#include "MtpBth.h"

#define MTP_SYNC_TIMEOUT_MS 5000

static VOID MtpIndicationCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication,
    _In_ PINDICATION_PARAMETERS Parameters);

static VOID
MtpSetFailure(_In_ PDEVICE_CONTEXT Ctx, _In_ MTP_LINK_STATE State, _In_ NTSTATUS Status)
{
    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->Status.LinkState = (ULONG)State;
    Ctx->Status.LastNtStatus = Status;
    WdfSpinLockRelease(Ctx->Lock);
}

/* BRB を同期送信する（PASSIVE_LEVEL 専用） */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
MtpSendBrbSync(_In_ PDEVICE_CONTEXT Ctx, _In_ PBRB Brb, _In_ ULONG BrbSize)
{
    WDF_MEMORY_DESCRIPTOR arg1;
    WDF_REQUEST_SEND_OPTIONS options;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&arg1, Brb, BrbSize);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(MTP_SYNC_TIMEOUT_MS));

    return WdfIoTargetSendInternalIoctlOthersSynchronously(Ctx->IoTarget, NULL,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB, &arg1, NULL, NULL, &options, NULL);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
MtpQueryRemoteAddress(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    BTH_DEVICE_INFO info;
    WDF_MEMORY_DESCRIPTOR out;

    RtlZeroMemory(&info, sizeof(info));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &info, sizeof(info));
    status = WdfIoTargetSendInternalIoctlSynchronously(Ctx->IoTarget, NULL,
        IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &out, NULL, NULL);
    if (NT_SUCCESS(status)) {
        Ctx->RemoteAddress = info.address;
        WdfSpinLockAcquire(Ctx->Lock);
        Ctx->Status.RemoteAddress = info.address;
        WdfSpinLockRelease(Ctx->Lock);
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
MtpOpenChannel(_In_ PDEVICE_CONTEXT Ctx, _In_ USHORT Psm, _Out_ L2CAP_CHANNEL_HANDLE *Handle)
{
    NTSTATUS status;
    struct _BRB_L2CA_OPEN_CHANNEL *brb;

    *Handle = NULL;
    brb = (struct _BRB_L2CA_OPEN_CHANNEL *)Ctx->Profile.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL, MTP_POOL_TAG);
    if (brb == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    brb->BtAddress = Ctx->RemoteAddress;
    brb->Psm = Psm;
    brb->ChannelFlags = CF_ROLE_EITHER;
    brb->ConfigOut.Flags = CFG_MTU;
    brb->ConfigOut.Mtu.Max = L2CAP_DEFAULT_MTU;
    brb->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
    brb->ConfigOut.Mtu.Preferred = L2CAP_DEFAULT_MTU;
    brb->ConfigIn.Flags = CFG_MTU;
    brb->ConfigIn.Mtu = brb->ConfigOut.Mtu;
    brb->IncomingQueueDepth = 50;
    brb->ReferenceObject = (PVOID)WdfDeviceWdmGetDeviceObject(Ctx->Device);
    brb->CallbackFlags = CALLBACK_DISCONNECT;
    brb->Callback = MtpIndicationCallback;
    brb->CallbackContext = Ctx;

    status = MtpSendBrbSync(Ctx, (PBRB)brb, sizeof(*brb));
    if (NT_SUCCESS(status)) {
        *Handle = brb->ChannelHandle;
    }
    Ctx->Profile.BthFreeBrb((PBRB)brb);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
MtpCloseChannel(_In_ PDEVICE_CONTEXT Ctx, _Inout_ L2CAP_CHANNEL_HANDLE *Handle)
{
    struct _BRB_L2CA_CLOSE_CHANNEL *brb;

    NTSTATUS status;

    if (*Handle == NULL) {
        return;
    }
    brb = (struct _BRB_L2CA_CLOSE_CHANNEL *)Ctx->Profile.BthAllocateBrb(BRB_L2CA_CLOSE_CHANNEL, MTP_POOL_TAG);
    if (brb == NULL) {
        /* 閉じる要求を出せていないのでチャネルは残っている。ハンドルを保持し、次の切断・再接続で閉じ直す */
        MtpSetFailure(Ctx, MtpLinkDisconnected, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    brb->BtAddress = Ctx->RemoteAddress;
    brb->ChannelHandle = *Handle;
    status = MtpSendBrbSync(Ctx, (PBRB)brb, sizeof(*brb));
    Ctx->Profile.BthFreeBrb((PBRB)brb);

    /*
     * クローズ要求自体の失敗は、相手側の切断で既にチャネルが消えている場合が大半なので、ハンドルは手放す。
     * 保持して閉じ直し続けると、消えたチャネルのために再接続が進まなくなる。失敗は状態として残す。
     */
    if (!NT_SUCCESS(status)) {
        MtpSetFailure(Ctx, MtpLinkDisconnected, status);
    }
    *Handle = NULL;
}

/* 同期 ACL 転送。In のとき *Length は入力でバッファ長、出力で受信長 */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
MtpAclSync(_In_ PDEVICE_CONTEXT Ctx, _In_ L2CAP_CHANNEL_HANDLE Channel, _In_ BOOLEAN In,
    _Inout_updates_bytes_(*Length) PUCHAR Buffer, _Inout_ PULONG Length)
{
    NTSTATUS status;
    struct _BRB_L2CA_ACL_TRANSFER *brb;

    brb = (struct _BRB_L2CA_ACL_TRANSFER *)Ctx->Profile.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, MTP_POOL_TAG);
    if (brb == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    brb->BtAddress = Ctx->RemoteAddress;
    brb->ChannelHandle = Channel;
    brb->TransferFlags = In ? (ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK) : ACL_TRANSFER_DIRECTION_OUT;
    brb->Buffer = Buffer;
    brb->BufferMDL = NULL;
    brb->BufferSize = *Length;

    status = MtpSendBrbSync(Ctx, (PBRB)brb, sizeof(*brb));
    if (NT_SUCCESS(status)) {
        *Length = brb->BufferSize;
    }
    Ctx->Profile.BthFreeBrb((PBRB)brb);
    return status;
}

/* 制御チャネルで SET_REPORT(Feature) を送り、HANDSHAKE を受ける */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
MtpEnableMultitouch(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    const uint8_t *report;
    size_t reportLength;
    UCHAR packet[16];
    UCHAR reply[16];
    ULONG length;

    if (MtpGetEnableReport(MTP_TRANSPORT_BT, &report, &reportLength) != MTP_OK ||
        reportLength + 1 > sizeof(packet)) {
        return STATUS_INTERNAL_ERROR;
    }
    packet[0] = HIDP_SET_REPORT_FEATURE;
    RtlCopyMemory(packet + 1, report, reportLength);
    length = (ULONG)(reportLength + 1);

    status = MtpAclSync(Ctx, Ctx->ControlChannel, FALSE, packet, &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(reply, sizeof(reply));
    length = sizeof(reply);
    status = MtpAclSync(Ctx, Ctx->ControlChannel, TRUE, reply, &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (length > sizeof(reply)) {
        length = sizeof(reply);
    }

    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->Status.EnableHandshake = (length >= 1) ? reply[0] : 0xFFFFFFFF;
    WdfSpinLockRelease(Ctx->Lock);

    /* HANDSHAKE(0x0) の結果コード 0 = SUCCESSFUL のみ成功とする */
    if (length < 1 || reply[0] != HIDP_HANDSHAKE) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static NTSTATUS
MtpSubmitReader(_In_ PMTP_READER Reader)
{
    PDEVICE_CONTEXT ctx = Reader->Ctx;
    WDF_REQUEST_REUSE_PARAMS reuse;
    NTSTATUS status;

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    status = WdfRequestReuse(Reader->Request, &reuse);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx->Profile.BthReuseBrb((PBRB)Reader->Brb, BRB_L2CA_ACL_TRANSFER);
    Reader->Brb->BtAddress = ctx->RemoteAddress;
    Reader->Brb->ChannelHandle = ctx->InterruptChannel;
    Reader->Brb->TransferFlags = ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK;
    Reader->Brb->Buffer = Reader->Buffer;
    Reader->Brb->BufferMDL = NULL;
    Reader->Brb->BufferSize = sizeof(Reader->Buffer);

    status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->IoTarget, Reader->Request,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB, Reader->BrbMemory, NULL, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WdfRequestSetCompletionRoutine(Reader->Request, MtpReaderCompletion, Reader);

    InterlockedIncrement(&ctx->ReadersOutstanding);
    KeClearEvent(&ctx->ReadersIdle);
    if (!WdfRequestSend(Reader->Request, ctx->IoTarget, WDF_NO_SEND_OPTIONS)) {
        status = WdfRequestGetStatus(Reader->Request);
        if (InterlockedDecrement(&ctx->ReadersOutstanding) == 0) {
            KeSetEvent(&ctx->ReadersIdle, IO_NO_INCREMENT, FALSE);
        }
        return status;
    }

    /*
     * 送信後に停止フラグを読み直す。MtpDisconnect は「Stopping = 1 → 全要求をキャンセル」の順で動くので、
     * - その前に送信していれば、MtpDisconnect のキャンセルがこの要求に届く
     * - その後に送信していれば、ここで Stopping = 1 が見えるので自分でキャンセルする
     * どちらの順でも、停止後に投入された要求が取り残されて ReadersIdle の待機が終わらなくなることはない。
     */
    if (InterlockedCompareExchange(&ctx->Stopping, 0, 0) != 0) {
        (VOID)WdfRequestCancelSentRequest(Reader->Request);
    }
    return STATUS_SUCCESS;
}

/* 読み取り要求をレポート 1 件で完了する */
_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
MtpCompleteRead(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Read, _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    PVOID buffer;
    size_t capacity;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(Read, 1, &buffer, &capacity);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Read, status);
        return;
    }
    if (capacity < Length) {
        WdfRequestComplete(Read, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    RtlCopyMemory(buffer, Data, Length);

    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->Status.FramesDelivered++;
    WdfSpinLockRelease(Ctx->Lock);
    WdfRequestCompleteWithInformation(Read, STATUS_SUCCESS, Length);
}

/* 割り込みチャネルの DATA を 1 件ユーザーモードへ渡す。待っている読み取り要求がなければためておく */
_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
MtpDeliverFrame(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Payload, _In_ ULONG Length)
{
    WDFREQUEST read = NULL;
    NTSTATUS status;
    ULONG slot;

    WdfSpinLockAcquire(Ctx->Lock);
    if (Length > MTP_RX_SLOT_SIZE) {
        Ctx->Status.FramesDropped++;
        WdfSpinLockRelease(Ctx->Lock);
        return;
    }
    /* 要求の取り出しと、ためる処理を同じロックの中で行う（MtpHandleRead と行き違いにならないようにする） */
    status = WdfIoQueueRetrieveNextRequest(Ctx->ReadQueue, &read);
    if (!NT_SUCCESS(status)) {
        if (Ctx->RxCount == MTP_RX_SLOTS) {
            Ctx->RxHead = (Ctx->RxHead + 1) % MTP_RX_SLOTS;
            Ctx->RxCount--;
            Ctx->Status.FramesDropped++;
        }
        slot = (Ctx->RxHead + Ctx->RxCount) % MTP_RX_SLOTS;
        RtlCopyMemory(Ctx->RxData[slot], Payload, Length);
        Ctx->RxLength[slot] = (USHORT)Length;
        Ctx->RxCount++;
        WdfSpinLockRelease(Ctx->Lock);
        return;
    }
    WdfSpinLockRelease(Ctx->Lock);
    MtpCompleteRead(Ctx, read, Payload, Length);
}

/* ReadFile の要求。ためてあるレポートがあればすぐに渡し、なければ届くまで手動キューで待たせる */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
MtpHandleRead(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request)
{
    UCHAR local[MTP_RX_SLOT_SIZE];
    ULONG length = 0;
    NTSTATUS status = STATUS_SUCCESS;

    WdfSpinLockAcquire(Ctx->Lock);
    if (Ctx->RxCount > 0) {
        length = Ctx->RxLength[Ctx->RxHead];
        RtlCopyMemory(local, Ctx->RxData[Ctx->RxHead], length);
        Ctx->RxHead = (Ctx->RxHead + 1) % MTP_RX_SLOTS;
        Ctx->RxCount--;
    } else {
        status = WdfRequestForwardToIoQueue(Request, Ctx->ReadQueue);
    }
    WdfSpinLockRelease(Ctx->Lock);

    if (length > 0) {
        MtpCompleteRead(Ctx, Request, local, length);
    } else if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
    }
}

/* アプリからのレポート（振動など）を送る。ChannelLock で接続・切断と重ならないようにする */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
MtpSendReport(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Method, _In_reads_bytes_(Length) const UCHAR *Report,
    _In_ ULONG Length)
{
    UCHAR packet[MTP_SET_REPORT_MAX + 1];
    UCHAR reply[16];
    ULONG length;
    UCHAR header;
    BOOLEAN connected;
    L2CAP_CHANNEL_HANDLE channel;
    NTSTATUS status;

    if (Length < 1 || Length > MTP_SET_REPORT_MAX) {
        return STATUS_INVALID_PARAMETER;
    }
    switch (Method) {
    case MTP_SEND_INTERRUPT_OUTPUT: header = HIDP_DATA_OUTPUT; break;
    case MTP_SEND_CONTROL_OUTPUT: header = HIDP_SET_REPORT_OUTPUT; break;
    case MTP_SEND_CONTROL_FEATURE: header = HIDP_SET_REPORT_FEATURE; break;
    default: return STATUS_INVALID_PARAMETER;
    }
    packet[0] = header;
    RtlCopyMemory(packet + 1, Report, Length);

    WdfWaitLockAcquire(Ctx->ChannelLock, NULL);
    WdfSpinLockAcquire(Ctx->Lock);
    connected = Ctx->Status.LinkState == MtpLinkConnected;
    WdfSpinLockRelease(Ctx->Lock);
    channel = Method == MTP_SEND_INTERRUPT_OUTPUT ? Ctx->InterruptChannel : Ctx->ControlChannel;
    if (!connected || channel == NULL) {
        status = STATUS_DEVICE_NOT_CONNECTED;
        goto out;
    }

    length = Length + 1;
    status = MtpAclSync(Ctx, channel, FALSE, packet, &length);
    if (NT_SUCCESS(status) && Method != MTP_SEND_INTERRUPT_OUTPUT) {
        RtlZeroMemory(reply, sizeof(reply));
        length = sizeof(reply);
        status = MtpAclSync(Ctx, Ctx->ControlChannel, TRUE, reply, &length);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(Ctx->Lock);
            Ctx->Status.SetReportHandshake = (length >= 1) ? reply[0] : 0xFFFFFFFF;
            WdfSpinLockRelease(Ctx->Lock);
            if (length < 1 || reply[0] != HIDP_HANDSHAKE) {
                status = STATUS_DEVICE_PROTOCOL_ERROR;
            }
        }
    }
    if (NT_SUCCESS(status)) {
        WdfSpinLockAcquire(Ctx->Lock);
        Ctx->Status.ReportsSent++;
        WdfSpinLockRelease(Ctx->Lock);
    }
out:
    WdfWaitLockRelease(Ctx->ChannelLock);
    return status;
}

VOID
MtpReaderCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    PMTP_READER reader = (PMTP_READER)Context;
    PDEVICE_CONTEXT ctx = reader->Ctx;
    NTSTATUS status = Params->IoStatus.Status;
    BOOLEAN stopping;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    stopping = InterlockedCompareExchange(&ctx->Stopping, 0, 0) != 0;

    if (NT_SUCCESS(status)) {
        ULONG length = reader->Brb->BufferSize;
        if (length >= 2 && reader->Buffer[0] == HIDP_DATA_INPUT) {
            WdfSpinLockAcquire(ctx->Lock);
            ctx->Status.FramesReceived++;
            WdfSpinLockRelease(ctx->Lock);
            MtpDeliverFrame(ctx, reader->Buffer + 1, length - 1);
        }
        if (!stopping) {
            status = MtpSubmitReader(reader);
        }
    }

    if (!NT_SUCCESS(status) && !stopping && status != STATUS_CANCELLED) {
        MtpScheduleReconnect(ctx, status);
    }

    if (InterlockedDecrement(&ctx->ReadersOutstanding) == 0) {
        KeSetEvent(&ctx->ReadersIdle, IO_NO_INCREMENT, FALSE);
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
MtpConnect(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    ULONG i;

    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->Status.LinkState = MtpLinkConnecting;
    Ctx->Status.ConnectAttempts++;
    WdfSpinLockRelease(Ctx->Lock);

    /* 前回の残りを片付けてから張り直す */
    MtpDisconnect(Ctx);
    InterlockedExchange(&Ctx->Stopping, 0);

    /* 閉じ切れなかったチャネルが残っている間は張り直さない（ハンドルを上書きして見失うため） */
    if (Ctx->ControlChannel != NULL || Ctx->InterruptChannel != NULL) {
        status = STATUS_DEVICE_BUSY;
        MtpSetFailure(Ctx, MtpLinkDisconnected, status);
        return status;
    }

    status = MtpOpenChannel(Ctx, MTP_PSM_HID_CONTROL, &Ctx->ControlChannel);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    status = MtpOpenChannel(Ctx, MTP_PSM_HID_INTERRUPT, &Ctx->InterruptChannel);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    status = MtpEnableMultitouch(Ctx);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    /* 前の接続でためた分は捨てる */
    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->RxHead = 0;
    Ctx->RxCount = 0;
    WdfSpinLockRelease(Ctx->Lock);

    for (i = 0; i < MTP_READER_COUNT; i++) {
        status = MtpSubmitReader(MtpGetReaderContext(Ctx->Readers[i]));
        if (!NT_SUCCESS(status)) {
            goto fail;
        }
    }

    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->Status.LinkState = MtpLinkConnected;
    Ctx->Status.LastNtStatus = STATUS_SUCCESS;
    WdfSpinLockRelease(Ctx->Lock);
    return STATUS_SUCCESS;

fail:
    MtpSetFailure(Ctx, MtpLinkDisconnected, status);
    MtpDisconnect(Ctx);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
MtpDisconnect(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;
    LONG previous = InterlockedExchange(&Ctx->Stopping, 1);

    for (i = 0; i < MTP_READER_COUNT; i++) {
        if (Ctx->Readers[i] != NULL) {
            (VOID)WdfRequestCancelSentRequest(Ctx->Readers[i]);
        }
    }
    (VOID)KeWaitForSingleObject(&Ctx->ReadersIdle, Executive, KernelMode, FALSE, NULL);

    /* 割り込み → 制御の順に閉じる（HID 仕様の順序） */
    MtpCloseChannel(Ctx, &Ctx->InterruptChannel);
    MtpCloseChannel(Ctx, &Ctx->ControlChannel);

    InterlockedExchange(&Ctx->Stopping, previous);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
MtpScheduleReconnect(_In_ PDEVICE_CONTEXT Ctx, _In_ NTSTATUS Reason)
{
    MtpSetFailure(Ctx, MtpLinkDisconnected, Reason);
    if (InterlockedCompareExchange(&Ctx->Active, 0, 0) == 0) {
        return;
    }
    /* 既に予約済みなら WdfTimerStart は予約し直すだけ */
    WdfTimerStart(Ctx->ReconnectTimer, WDF_REL_TIMEOUT_IN_SEC(MTP_RECONNECT_SECONDS));
}

static VOID
MtpIndicationCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication, _In_ PINDICATION_PARAMETERS Parameters)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Parameters);

    if (Indication == IndicationRemoteDisconnect) {
        MtpScheduleReconnect(ctx, STATUS_CONNECTION_DISCONNECTED);
    }
}
