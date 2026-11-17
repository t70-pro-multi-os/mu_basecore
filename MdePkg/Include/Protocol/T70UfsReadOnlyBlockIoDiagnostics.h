/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_H
#define T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_H

#include <Uefi.h>

#define T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_GUID \
  {0x27dc58a1,0xc87b,0x490d,{0xa7,0x67,0x4c,0x6d,0x16,0x8c,0x42,0x9b}}
#define T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_VERSION  2U
#define T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_MAX     8U

/* Stage values describe only binding and READ CAPACITY. No user data is read. */
typedef enum {
  T70UfsDiagStageNone = 0,
  T70UfsDiagStageScsiIo,
  T70UfsDiagStageDevicePath,
  T70UfsDiagStageLuPath,
  T70UfsDiagStageDeviceType,
  T70UfsDiagStageDeviceLocation,
  T70UfsDiagStageTarget,
  T70UfsDiagStageLun,
  T70UfsDiagStageExistingBlockIo,
  T70UfsDiagStageSupported,
  T70UfsDiagStageOpenScsi,
  T70UfsDiagStageIoAlignment,
  T70UfsDiagStageCapacityAllocate,
  T70UfsDiagStageCapacity10,
  T70UfsDiagStageCapacity10Decode,
  T70UfsDiagStageCapacity16,
  T70UfsDiagStageCapacity16Decode,
  T70UfsDiagStageCapacityFree,
  T70UfsDiagStageGeometry,
  T70UfsDiagStageInstallBlockIo,
  T70UfsDiagStageReady
} T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_STAGE;

#define T70_UFS_DIAG_SUPPORTED_SEEN       0x00000001U
#define T70_UFS_DIAG_START_SEEN           0x00000002U
#define T70_UFS_DIAG_OPEN_ATTEMPTED       0x00000004U
#define T70_UFS_DIAG_CAPACITY10_ATTEMPTED  0x00000008U
#define T70_UFS_DIAG_CAPACITY16_ATTEMPTED  0x00000010U
#define T70_UFS_DIAG_INSTALL_ATTEMPTED     0x00000020U
#define T70_UFS_DIAG_LUN_VALID             0x00000040U

typedef struct {
  EFI_HANDLE  Controller;
  UINT32      Flags;
  UINT32      SupportedStage;
  EFI_STATUS  SupportedStatus;
  EFI_STATUS  StageStatus;
  UINT32      StartStage;
  EFI_STATUS  StartStatus;
  EFI_STATUS  OpenStatus;
  EFI_STATUS  Capacity10Status;
  EFI_STATUS  Capacity16Status;
  EFI_STATUS  InstallStatus;
  UINT8       Lun;
  UINT8       Reserved[3];
  UINT32      BlockSize;
  UINT64      LastBlock;
} T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD;

typedef struct {
  UINT32 Version;
  UINT32 Count;
  UINT32 Dropped;
  EFI_STATUS DriverBindingInstallStatus;
  EFI_STATUS DiagnosticsUninstallStatus;
  T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD Records[
    T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_MAX
    ];
} T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS;

#endif
