#include "Driver.h"

namespace {

void CompleteRequestWithStatus(
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ size_t Information = 0) {
    WdfRequestCompleteWithInformation(Request, Status, Information);
}

NTSTATUS CopyToOutputBuffer(
    _In_ WDFREQUEST Request,
    _In_ const void* Source,
    _In_ size_t SourceSize,
    _In_ size_t MinimumRequired = 0) {
    size_t outLen = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(
        Request,
        MinimumRequired > 0 ? MinimumRequired : SourceSize,
        nullptr,
        &outLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (outLen < SourceSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    void* outBuf = nullptr;
    status = WdfRequestRetrieveOutputBuffer(Request, SourceSize, &outBuf, nullptr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlCopyMemory(outBuf, Source, SourceSize);
    return STATUS_SUCCESS;
}

NTSTATUS HandlePing(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request) {
    UNREFERENCED_PARAMETER(Device);

    EGO_PING_REPLY reply{};
    reply.Version = EGoConst::kVersion;
    reply.CapabilityFlags =
        EGoConst::kCapabilityAfeControl |
        EGoConst::kCapabilityRegisterRw |
        EGoConst::kCapabilityFrameRead;

    NTSTATUS status = CopyToOutputBuffer(Request, &reply, sizeof(reply));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    CompleteRequestWithStatus(Request, STATUS_SUCCESS, sizeof(reply));
    return STATUS_SUCCESS;
}

NTSTATUS HandleGetProtocolInfo(_In_ WDFREQUEST Request) {
    EGO_PROTOCOL_INFO info{};
    info.SpiIoctlIntOpen = EGoConst::kSpiIoctlIntOpen;
    info.SpiIoctlIntClose = EGoConst::kSpiIoctlIntClose;
    info.SpiIoctlWriteRead = EGoConst::kSpiIoctlWriteRead;
    info.SpiIoctlWaitInt = EGoConst::kSpiIoctlWaitInt;
    info.SpiIoctlFullDuplex = EGoConst::kSpiIoctlFullDuplex;
    info.SpiIoctlGetFrame = EGoConst::kSpiIoctlGetFrame;
    info.SpiIoctlSetTimeout = EGoConst::kSpiIoctlSetTimeout;
    info.SpiIoctlSetBlock = EGoConst::kSpiIoctlSetBlock;
    info.SpiIoctlSetReset = EGoConst::kSpiIoctlSetReset;
    info.SpiIoctlReadAcpi = EGoConst::kSpiIoctlReadAcpi;

    NTSTATUS status = CopyToOutputBuffer(Request, &info, sizeof(info));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    CompleteRequestWithStatus(Request, STATUS_SUCCESS, sizeof(info));
    return STATUS_SUCCESS;
}

NTSTATUS HandleSetAfeCommand(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request) {
    PEGO_AFE_COMMAND cmd = nullptr;
    size_t inLen = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(EGO_AFE_COMMAND),
        reinterpret_cast<PVOID*>(&cmd),
        &inLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (inLen < sizeof(EGO_AFE_COMMAND)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    auto* ctx = EGoGetDeviceContext(Device);
    ctx->LastAfeCommand = *cmd;
    ctx->LastStatus = STATUS_SUCCESS;

    CompleteRequestWithStatus(Request, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS HandleGetLastStatus(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request) {
    auto* ctx = EGoGetDeviceContext(Device);
    ULONG last = ctx->LastStatus;
    NTSTATUS status = CopyToOutputBuffer(Request, &last, sizeof(last));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    CompleteRequestWithStatus(Request, STATUS_SUCCESS, sizeof(last));
    return STATUS_SUCCESS;
}

NTSTATUS HandleRegisterRead(_In_ WDFREQUEST Request) {
    PEGO_REGISTER_RW req = nullptr;
    size_t inLen = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(EGO_REGISTER_RW),
        reinterpret_cast<PVOID*>(&req),
        &inLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (req->Length > sizeof(req->Data)) {
        return STATUS_INVALID_PARAMETER;
    }

    EGO_REGISTER_RW reply{};
    reply.Address = req->Address;
    reply.Length = req->Length;

    // Placeholder pattern, to be replaced by real SPI register access.
    for (ULONG i = 0; i < reply.Length; ++i) {
        reply.Data[i] = static_cast<UCHAR>((req->Address + i) & 0xFFu);
    }

    status = CopyToOutputBuffer(Request, &reply, sizeof(reply));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    CompleteRequestWithStatus(Request, STATUS_SUCCESS, sizeof(reply));
    return STATUS_SUCCESS;
}

NTSTATUS HandleRegisterWrite(_In_ WDFREQUEST Request) {
    PEGO_REGISTER_RW req = nullptr;
    size_t inLen = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(EGO_REGISTER_RW),
        reinterpret_cast<PVOID*>(&req),
        &inLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (req->Length > sizeof(req->Data)) {
        return STATUS_INVALID_PARAMETER;
    }

    UNREFERENCED_PARAMETER(req);

    CompleteRequestWithStatus(Request, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS HandleGetFrame(_In_ WDFREQUEST Request) {
    EGO_FRAME_RESPONSE resp{};
    resp.DataLength = EGoConst::kFrameBufferSize;

    // Placeholder frame for host-side compatibility testing.
    for (ULONG i = 0; i < EGoConst::kFrameBufferSize; ++i) {
        resp.Data[i] = static_cast<UCHAR>(i & 0xFFu);
    }

    void* outBuf = nullptr;
    size_t outLen = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(EGO_FRAME_RESPONSE),
        &outBuf,
        &outLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (outLen < sizeof(EGO_FRAME_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(outBuf, &resp, sizeof(resp));
    CompleteRequestWithStatus(Request, STATUS_SUCCESS, sizeof(resp));
    return STATUS_SUCCESS;
}

} // namespace

extern "C"
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EGoEvtDeviceAdd);

    return WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);
}

NTSTATUS EGoEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit) {
    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, EGoConst::kDeviceName);
    NTSTATUS status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, EGO_DEVICE_CONTEXT);

    WDFDEVICE device;
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, EGoConst::kSymbolicLink);
    status = WdfDeviceCreateSymbolicLink(device, &symLink);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    auto* ctx = EGoGetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));

    WDF_IO_QUEUE_CONFIG ioConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioConfig, WdfIoQueueDispatchParallel);
    ioConfig.EvtIoDeviceControl = EGoEvtIoDeviceControl;

    WDFQUEUE queue;
    status = WdfIoQueueCreate(device, &ioConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

VOID EGoEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode) {
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    switch (IoControlCode) {
    case IOCTL_EGO_PING:
        status = HandlePing(device, Request);
        return;

    case IOCTL_EGO_GET_PROTOCOL_INFO:
        status = HandleGetProtocolInfo(Request);
        return;

    case IOCTL_EGO_SEND_AFE_COMMAND:
        status = HandleSetAfeCommand(device, Request);
        return;

    case IOCTL_EGO_GET_LAST_STATUS:
        status = HandleGetLastStatus(device, Request);
        return;

    case IOCTL_EGO_READ_REGISTER:
        status = HandleRegisterRead(Request);
        return;

    case IOCTL_EGO_WRITE_REGISTER:
        status = HandleRegisterWrite(Request);
        return;

    case IOCTL_EGO_GET_FRAME:
        status = HandleGetFrame(Request);
        return;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    CompleteRequestWithStatus(Request, status, 0);
}
