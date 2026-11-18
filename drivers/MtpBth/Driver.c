/*
 * Driver.c - MtpBth のドライバー初期化、PnP/電源、ユーザーモード向けキュー
 */
#include "MtpBth.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, MtpEvtDeviceAdd)
#pragma alloc_text(PAGE, MtpEvtPrepareHardware)
#pragma alloc_text(PAGE, MtpEvtReleaseHardware)
#endif

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, MtpEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

static NTSTATUS
MtpCreateReaders(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    ULONG i;

    for (i = 0; i < MTP_READER_COUNT; i++) {
        WDF_OBJECT_ATTRIBUTES attributes;
        PMTP_READER reader;
        WDFREQUEST request;
        PVOID brb;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MTP_READER);
        attributes.ParentObject = Ctx->Device;
        status = WdfRequestCreate(&attributes, Ctx->IoTarget, &request);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        brb = Ctx->Profile.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, MTP_POOL_TAG);
        if (brb == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        reader = MtpGetReaderContext(request);
        RtlZeroMemory(reader, sizeof(*reader));
        reader->Ctx = Ctx;
        reader->Request = request;
        reader->Brb = (struct _BRB_L2CA_ACL_TRANSFER *)brb;

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = request;
        status = WdfMemoryCreatePreallocated(&attributes, brb, sizeof(struct _BRB_L2CA_ACL_TRANSFER),
            &reader->BrbMemory);
        if (!NT_SUCCESS(status)) {
            Ctx->Profile.BthFreeBrb((PBRB)brb);
            reader->Brb = NULL;
            return status;
        }
        Ctx->Readers[i] = request;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
MtpEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_TIMER_CONFIG timerConfig;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = MtpEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = MtpEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = MtpEvtD0Entry;
    pnp.EvtDeviceD0Exit = MtpEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = MtpGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Device = device;
    ctx->IoTarget = WdfDeviceGetIoTarget(device);
    ctx->Status.EnableHandshake = 0xFFFFFFFF;
    ctx->Status.SetReportHandshake = 0xFFFFFFFF;
    KeInitializeEvent(&ctx->ReadersIdle, NotificationEvent, TRUE);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &ctx->ChannelLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 既定キュー: IOCTL と ReadFile の受付。レポート送信は L2CAP に同期で送るので、パッシブレベルで呼ばせる */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = MtpEvtIoRead;
    queueConfig.EvtIoDeviceControl = MtpEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &queueConfig, &attributes, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 手動キュー: フレームが届くまで ReadFile を保留する */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->ReadQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&timerConfig, MtpEvtReconnectTimer);
    timerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timerConfig, &attributes, &ctx->ReconnectTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_MTP_RAW, NULL);
}

NTSTATUS
MtpEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Resources, _In_ WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS status;
    PDEVICE_CONTEXT ctx = MtpGetContext(Device);

    UNREFERENCED_PARAMETER(Resources);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    if (!ctx->ProfileAcquired) {
        status = WdfFdoQueryForInterface(Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
            (PINTERFACE)&ctx->Profile, sizeof(ctx->Profile),
            BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ctx->ProfileAcquired = TRUE;

        status = MtpCreateReaders(ctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    return MtpQueryRemoteAddress(ctx);
}

NTSTATUS
MtpEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = MtpGetContext(Device);
    ULONG i;

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    if (ctx->ProfileAcquired) {
        for (i = 0; i < MTP_READER_COUNT; i++) {
            PMTP_READER reader;
            if (ctx->Readers[i] == NULL) {
                continue;
            }
            reader = MtpGetReaderContext(ctx->Readers[i]);
            if (reader->Brb != NULL) {
                ctx->Profile.BthFreeBrb((PBRB)reader->Brb);
                reader->Brb = NULL;
            }
            WdfObjectDelete(ctx->Readers[i]);
            ctx->Readers[i] = NULL;
        }
        if (ctx->Profile.Interface.InterfaceDereference != NULL) {
            ctx->Profile.Interface.InterfaceDereference(ctx->Profile.Interface.Context);
        }
        ctx->ProfileAcquired = FALSE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
MtpEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = MtpGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    InterlockedExchange(&ctx->Active, 1);
    /* D0Entry では待たない。接続は即時に走るパッシブタイマーで行う */
    WdfTimerStart(ctx->ReconnectTimer, WDF_REL_TIMEOUT_IN_MS(1));
    return STATUS_SUCCESS;
}

NTSTATUS
MtpEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = MtpGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    InterlockedExchange(&ctx->Active, 0);
    WdfTimerStop(ctx->ReconnectTimer, TRUE);
    WdfWaitLockAcquire(ctx->ChannelLock, NULL);
    MtpDisconnect(ctx);
    WdfWaitLockRelease(ctx->ChannelLock);
    return STATUS_SUCCESS;
}

VOID
MtpEvtReconnectTimer(_In_ WDFTIMER Timer)
{
    PDEVICE_CONTEXT ctx = MtpGetContext(WdfTimerGetParentObject(Timer));
    NTSTATUS status;

    if (InterlockedCompareExchange(&ctx->Active, 0, 0) == 0) {
        return;
    }
    WdfWaitLockAcquire(ctx->ChannelLock, NULL);
    status = MtpConnect(ctx);
    WdfWaitLockRelease(ctx->ChannelLock);
    if (!NT_SUCCESS(status)) {
        MtpScheduleReconnect(ctx, status);
    }
}

VOID
MtpEvtIoRead(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t Length)
{
    PDEVICE_CONTEXT ctx = MtpGetContext(WdfIoQueueGetDevice(Queue));

    if (Length == 0) {
        WdfRequestComplete(Request, STATUS_INVALID_BUFFER_SIZE);
        return;
    }
    MtpHandleRead(ctx, Request);
}

VOID
MtpEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = MtpGetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status;
    PMTP_STATUS_INFO out;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode == MTP_IOCTL_SET_REPORT) {
        PUCHAR in;
        size_t inLength;
        status = WdfRequestRetrieveInputBuffer(Request, 2, (PVOID *)&in, &inLength);
        if (NT_SUCCESS(status)) {
            status = inLength > MTP_SET_REPORT_MAX + 1 ? STATUS_INVALID_PARAMETER
                : MtpSendReport(ctx, in[0], in + 1, (ULONG)(inLength - 1));
        }
        WdfRequestComplete(Request, status);
        return;
    }
    if (IoControlCode != MTP_IOCTL_GET_STATUS) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(MTP_STATUS_INFO), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    WdfSpinLockAcquire(ctx->Lock);
    *out = ctx->Status;
    WdfSpinLockRelease(ctx->Lock);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(MTP_STATUS_INFO));
}
