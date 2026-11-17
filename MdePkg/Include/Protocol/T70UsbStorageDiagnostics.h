/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_USB_STORAGE_DIAGNOSTICS_H
#define T70_USB_STORAGE_DIAGNOSTICS_H
#include <Uefi.h>
#define T70_USB_STORAGE_DIAGNOSTICS_GUID \
  {0xf02d2ada,0x7006,0x4d26,{0xb1,0x21,0x37,0x48,0x65,0x3c,0x09,0x12}}
#define T70_USB_DIAG_MAX 32
#define T70_USB_DIAG_VERSION 1
typedef struct {
  EFI_HANDLE Controller;
  UINT32 Sequence;
  UINT32 Stage; /* 1=start, 2=transport, 3=command, 4=sense, 5=bind result */
  EFI_STATUS BindStatus;
  EFI_STATUS CommandStatus;
  EFI_STATUS LastError;
  UINT32 CommandResult;
  UINT8 Opcode, Lun, SenseKey, Asc, Ascq, ErrorOpcode;
  UINT8 Reserved[2];
} T70_USB_STORAGE_RECORD;
typedef struct {
  UINT32 Version, Count, Sequence, Dropped;
  T70_USB_STORAGE_RECORD Records[T70_USB_DIAG_MAX];
} T70_USB_STORAGE_DIAGNOSTICS;
#define T70_USB_HOST_DIAGNOSTICS_GUID \
  {0xb54a3981,0x52cf,0x4753,{0xa7,0xc1,0x21,0x30,0x6e,0xc9,0x41,0xb2}}
typedef struct {
  EFI_STATUS Status;
  UINT16 BusStep;
  UINT8 Reason, Failed, Ready, Connected, Devctl, Id;
  UINT8 SenseStage, SenseRegister, SenseSaved, SenseExpected, SenseLive;
  EFI_STATUS SenseStatus;
} T70_USB_HOST_SNAPSHOT;
typedef EFI_STATUS (EFIAPI *T70_USB_HOST_READ)(T70_USB_HOST_SNAPSHOT *Snapshot);
typedef struct { UINT32 Version; T70_USB_HOST_READ Read; } T70_USB_HOST_DIAGNOSTICS;
#endif
