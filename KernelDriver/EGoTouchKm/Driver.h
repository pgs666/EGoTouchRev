#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "Ioctl.h"

EXTERN_C_START

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EGoEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EGoEvtIoDeviceControl;

EXTERN_C_END

typedef struct _EGO_DEVICE_CONTEXT {
    ULONG LastStatus;
    EGO_AFE_COMMAND LastAfeCommand;
} EGO_DEVICE_CONTEXT, *PEGO_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(EGO_DEVICE_CONTEXT, EGoGetDeviceContext);

namespace EGoConst {
    inline constexpr PCWSTR kDeviceName = L"\\Device\\EGoTouchKm";
    inline constexpr PCWSTR kSymbolicLink = L"\\DosDevices\\EGoTouchKm";

    inline constexpr ULONG kVersion = 0x00010000;
    inline constexpr ULONG kCapabilityAfeControl = 0x00000001;
    inline constexpr ULONG kCapabilityRegisterRw = 0x00000002;
    inline constexpr ULONG kCapabilityFrameRead = 0x00000004;

    inline constexpr ULONG kFrameBufferSize = 6000;

    // Legacy SPI control codes discovered in user-mode implementation.
    inline constexpr ULONG kSpiIoctlIntOpen = 0x04001C00;
    inline constexpr ULONG kSpiIoctlIntClose = 0x04001C04;
    inline constexpr ULONG kSpiIoctlWriteRead = 0x04001C10;
    inline constexpr ULONG kSpiIoctlWaitInt = 0x04001C20;
    inline constexpr ULONG kSpiIoctlFullDuplex = 0x04001C24;
    inline constexpr ULONG kSpiIoctlGetFrame = 0x04001C28;
    inline constexpr ULONG kSpiIoctlSetTimeout = 0x04001C2C;
    inline constexpr ULONG kSpiIoctlSetBlock = 0x04001C30;
    inline constexpr ULONG kSpiIoctlSetReset = 0x04001C34;
    inline constexpr ULONG kSpiIoctlReadAcpi = 0x04001C38;
}
