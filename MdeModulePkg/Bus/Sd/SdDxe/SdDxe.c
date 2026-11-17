/** @file
  The SdDxe driver is used to manage the SD memory card device.

  It produces BlockIo and BlockIo2 protocols to allow upper layer
  access the SD memory card device.

  Copyright (c) 2015 - 2018, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "SdDxe.h"
#include <Library/PcdLib.h>

//
// SdDxe Driver Binding Protocol Instance
//
EFI_DRIVER_BINDING_PROTOCOL  gSdDxeDriverBinding = {
  SdDxeDriverBindingSupported,
  SdDxeDriverBindingStart,
  SdDxeDriverBindingStop,
  0x10,
  NULL,
  NULL
};

//
// Template for SD_DEVICE data structure.
//
SD_DEVICE  mSdDeviceTemplate = {
  SD_DEVICE_SIGNATURE,         // Signature
  NULL,                        // Handle
  NULL,                        // DevicePath
  0xFF,                        // Slot
  FALSE,                       // SectorAddressing
  {                            // BlockIo
    EFI_BLOCK_IO_PROTOCOL_REVISION,
    NULL,
    SdReset,
    SdReadBlocks,
    SdWriteBlocks,
    SdFlushBlocks
  },
  {                            // BlockIo2
    NULL,
    SdResetEx,
    SdReadBlocksEx,
    SdWriteBlocksEx,
    SdFlushBlocksEx
  },
  {                            // BlockMedia
    0,                         // MediaId
    FALSE,                     // RemovableMedia
    FALSE,                     // MediaPresent until the CSD geometry is validated
    FALSE,                     // LogicPartition
    FALSE,                     // ReadOnly
    FALSE,                     // WritingCache
    0x200,                     // BlockSize
    0,                         // IoAlign
    0                          // LastBlock
  },
  FALSE,                       // MediaInvalidated
  FALSE,                       // ResetInProgress
  0,                           // ActiveOperations
  {                            // EraseBlock
    EFI_ERASE_BLOCK_PROTOCOL_REVISION,
    1,
    SdEraseBlocks
  },
  {                            // DiskInfo
    EFI_DISK_INFO_SD_MMC_INTERFACE_GUID,
    SdDiskInfoInquiry,
    SdDiskInfoIdentify,
    SdDiskInfoSenseData,
    SdDiskInfoWhichIde
  },
  {                            // Queue
    NULL,
    NULL
  },
  {                            // Csd
    0,
  },
  {                            // Cid
    0,
  },
  NULL,                        // ControllerNameTable
  {                            // ModelName
    0,
  },
  NULL,                        // Private
  {                            // ChildLink
    NULL,
    NULL
  },
  FALSE,                       // ChildTracked
  FALSE                        // StopInProgress
};

/**
  Return whether a PassThru status means this child no longer names usable media.
**/
BOOLEAN
SdMediaUnavailableStatus (
  IN EFI_STATUS  Status
  )
{
  return (BOOLEAN)(
                    (Status == EFI_NO_MEDIA) ||
                    (Status == EFI_NOT_FOUND) ||
                    (Status == EFI_MEDIA_CHANGED)
                    );
}

/**
  Permanently invalidate this SD_DEVICE until its child handle is removed.

  Block I/O has no media-refresh operation. A path becoming available again is
  not enough to reuse the old CSD-derived geometry.
**/
VOID
SdInvalidateMediaAtNotify (
  IN SD_DEVICE  *Device
  )
{
  if ((Device == NULL) || Device->MediaInvalidated) {
    return;
  }

  Device->MediaInvalidated              = TRUE;
  Device->BlockMedia.MediaPresent       = FALSE;
  Device->BlockMedia.MediaId++;
  if (Device->BlockMedia.MediaId == 0) {
    Device->BlockMedia.MediaId = 1;
  }

  Device->BlockMedia.LastBlock = 0;
  Device->EraseBlock.EraseLengthGranularity = 0;
  Device->SectorAddressing = FALSE;
}

VOID
SdInvalidateMedia (
  IN SD_DEVICE  *Device
  )
{
  EFI_TPL  OldTpl;

  if (Device == NULL) {
    return;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  SdInvalidateMediaAtNotify (Device);
  gBS->RestoreTPL (OldTpl);
}

/**
  Decode and print SD CSD Register content.

  @param[in] Csd           Pointer to SD_CSD data structure.

  @retval EFI_SUCCESS      The function completed successfully
**/
EFI_STATUS
DumpCsd (
  IN SD_CSD  *Csd
  )
{
  SD_CSD2  *Csd2;

  DEBUG ((DEBUG_INFO, "== Dump Sd Csd Register==\n"));
  DEBUG ((DEBUG_INFO, "  CSD structure                    0x%x\n", Csd->CsdStructure));
  DEBUG ((DEBUG_INFO, "  Data read access-time 1          0x%x\n", Csd->Taac));
  DEBUG ((DEBUG_INFO, "  Data read access-time 2          0x%x\n", Csd->Nsac));
  DEBUG ((DEBUG_INFO, "  Max. bus clock frequency         0x%x\n", Csd->TranSpeed));
  DEBUG ((DEBUG_INFO, "  Device command classes           0x%x\n", Csd->Ccc));
  DEBUG ((DEBUG_INFO, "  Max. read data block length      0x%x\n", Csd->ReadBlLen));
  DEBUG ((DEBUG_INFO, "  Partial blocks for read allowed  0x%x\n", Csd->ReadBlPartial));
  DEBUG ((DEBUG_INFO, "  Write block misalignment         0x%x\n", Csd->WriteBlkMisalign));
  DEBUG ((DEBUG_INFO, "  Read block misalignment          0x%x\n", Csd->ReadBlkMisalign));
  DEBUG ((DEBUG_INFO, "  DSR implemented                  0x%x\n", Csd->DsrImp));
  if (Csd->CsdStructure == 0) {
    DEBUG ((DEBUG_INFO, "  Device size                      0x%x\n", Csd->CSizeLow | (Csd->CSizeHigh << 2)));
    DEBUG ((DEBUG_INFO, "  Max. read current @ VDD min      0x%x\n", Csd->VddRCurrMin));
    DEBUG ((DEBUG_INFO, "  Max. read current @ VDD max      0x%x\n", Csd->VddRCurrMax));
    DEBUG ((DEBUG_INFO, "  Max. write current @ VDD min     0x%x\n", Csd->VddWCurrMin));
    DEBUG ((DEBUG_INFO, "  Max. write current @ VDD max     0x%x\n", Csd->VddWCurrMax));
  } else {
    Csd2 = (SD_CSD2 *)(VOID *)Csd;
    DEBUG ((DEBUG_INFO, "  Device size                      0x%x\n", Csd2->CSizeLow | (Csd->CSizeHigh << 16)));
  }

  DEBUG ((DEBUG_INFO, "  Erase sector size                0x%x\n", Csd->SectorSize));
  DEBUG ((DEBUG_INFO, "  Erase single block enable        0x%x\n", Csd->EraseBlkEn));
  DEBUG ((DEBUG_INFO, "  Write protect group size         0x%x\n", Csd->WpGrpSize));
  DEBUG ((DEBUG_INFO, "  Write protect group enable       0x%x\n", Csd->WpGrpEnable));
  DEBUG ((DEBUG_INFO, "  Write speed factor               0x%x\n", Csd->R2WFactor));
  DEBUG ((DEBUG_INFO, "  Max. write data block length     0x%x\n", Csd->WriteBlLen));
  DEBUG ((DEBUG_INFO, "  Partial blocks for write allowed 0x%x\n", Csd->WriteBlPartial));
  DEBUG ((DEBUG_INFO, "  File format group                0x%x\n", Csd->FileFormatGrp));
  DEBUG ((DEBUG_INFO, "  Copy flag (OTP)                  0x%x\n", Csd->Copy));
  DEBUG ((DEBUG_INFO, "  Permanent write protection       0x%x\n", Csd->PermWriteProtect));
  DEBUG ((DEBUG_INFO, "  Temporary write protection       0x%x\n", Csd->TmpWriteProtect));
  DEBUG ((DEBUG_INFO, "  File format                      0x%x\n", Csd->FileFormat));

  return EFI_SUCCESS;
}

/**
  Get SD device model name.

  @param[in, out] Device   The pointer to the SD_DEVICE data structure.
  @param[in]      Cid      Pointer to SD_CID data structure.

  @retval EFI_SUCCESS      The function completed successfully

**/
EFI_STATUS
GetSdModelName (
  IN OUT SD_DEVICE  *Device,
  IN     SD_CID     *Cid
  )
{
  CHAR8  String[SD_MODEL_NAME_MAX_LEN];

  ZeroMem (String, sizeof (String));
  CopyMem (String, Cid->OemId, sizeof (Cid->OemId));
  String[sizeof (Cid->OemId)] = ' ';
  CopyMem (String + sizeof (Cid->OemId) + 1, Cid->ProductName, sizeof (Cid->ProductName));
  String[sizeof (Cid->OemId) + sizeof (Cid->ProductName)] = ' ';
  CopyMem (String + sizeof (Cid->OemId) + sizeof (Cid->ProductName) + 1, Cid->ProductSerialNumber, sizeof (Cid->ProductSerialNumber));

  AsciiStrToUnicodeStrS (String, Device->ModelName, sizeof (Device->ModelName) / sizeof (Device->ModelName[0]));

  return EFI_SUCCESS;
}

/**
  Discover user area partition in the SD device.

  @param[in] Device          The pointer to the SD_DEVICE data structure.

  @retval EFI_SUCCESS        The user area partition in the SD device is successfully identified.
  @return Others             Some error occurs when identifying the user area.

**/
EFI_STATUS
DiscoverUserArea (
  IN SD_DEVICE  *Device
  )
{
  EFI_STATUS  Status;
  SD_CSD      *Csd;
  SD_CSD2     *Csd2;
  SD_CID      *Cid;
  UINT64      Capacity;
  UINT64      BlockCount;
  UINT64      CSize;
  UINT32      DevStatus;
  UINT16      Rca;
  UINT32      CSizeMul;
  UINT32      ReadBlLen;
  UINT32      EraseLengthGranularity;
  BOOLEAN     SectorAddressing;

  //
  // Deselect the device to force it enter stby mode.
  // Note here we don't judge return status as some SD devices return
  // error but the state has been stby.
  //
  SdSelect (Device, 0);

  Status = SdSetRca (Device, &Rca);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DiscoverUserArea(): Assign new Rca = 0x%x fails with %r\n", Rca, Status));
    return Status;
  }

  Csd    = &Device->Csd;
  Status = SdGetCsd (Device, Rca, Csd);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Validate the addressable capacity before querying more card data or
  // publishing Block I/O.  The byte-addressed CSD v1 path is limited to 4 GiB.
  //
  if (Csd->CsdStructure == 0) {
    if ((Csd->ReadBlLen < 9) || (Csd->ReadBlLen > 11)) {
      return EFI_UNSUPPORTED;
    }

    SectorAddressing = FALSE;
    CSize            = (((UINT64)Csd->CSizeHigh << 2) | Csd->CSizeLow) + 1;
    CSizeMul         = 1U << (Csd->CSizeMul + 2);
    ReadBlLen        = 1U << Csd->ReadBlLen;
    Capacity         = MultU64x32 (CSize, CSizeMul);
    Capacity         = MultU64x32 (Capacity, ReadBlLen);
    if (Capacity > SIZE_4GB) {
      return EFI_UNSUPPORTED;
    }
  } else if (Csd->CsdStructure == 1) {
    SectorAddressing = TRUE;
    Csd2             = (SD_CSD2 *)(VOID *)Csd;
    CSize             = (((UINT64)Csd2->CSizeHigh << 16) | Csd2->CSizeLow) + 1;
    Capacity          = MultU64x32 (CSize, SIZE_512KB);
  } else {
    return EFI_UNSUPPORTED;
  }

  if ((Capacity == 0) || ((Capacity % 0x200) != 0)) {
    return EFI_UNSUPPORTED;
  }

  BlockCount = Capacity / 0x200;
  if ((BlockCount == 0) || (BlockCount > ((UINT64)MAX_UINT32 + 1ULL))) {
    return EFI_UNSUPPORTED;
  }

  EraseLengthGranularity = 1;
  if (!Csd->EraseBlkEn) {
    if ((Csd->WriteBlLen < 9) || (Csd->WriteBlLen > 11)) {
      return EFI_UNSUPPORTED;
    }

    EraseLengthGranularity = (Csd->SectorSize + 1U) * (1U << (Csd->WriteBlLen - 9U));
  }

  DumpCsd (Csd);

  Cid    = &Device->Cid;
  Status = SdGetCid (Device, Rca, Cid);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  GetSdModelName (Device, Cid);

  Status = SdSelect (Device, Rca);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DiscoverUserArea(): Reselect the device 0x%x fails with %r\n", Rca, Status));
    return Status;
  }

  Status = SdSendStatus (Device, Rca, &DevStatus);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Device->SectorAddressing            = SectorAddressing;
  Device->BlockIo.Media               = &Device->BlockMedia;
  Device->BlockIo2.Media              = &Device->BlockMedia;
  Device->BlockMedia.IoAlign          = Device->Private->PassThru->IoAlign;
  Device->BlockMedia.BlockSize        = 0x200;
  Device->BlockMedia.LastBlock        = (UINT32)(BlockCount - 1);
  Device->BlockMedia.RemovableMedia   = TRUE;
  Device->BlockMedia.MediaPresent     = TRUE;
  Device->BlockMedia.LogicalPartition = FALSE;
  Device->BlockMedia.ReadOnly         = (BOOLEAN)(PcdGetBool (PcdSdDxeReadOnly) ||
                                                Csd->PermWriteProtect || Csd->TmpWriteProtect);
  Device->EraseBlock.EraseLengthGranularity = EraseLengthGranularity;

  return Status;
}

/**
  Determine whether the parent pass-through protocol has any child opens.
**/
STATIC
EFI_STATUS
SdGetChildOpenInformation (
  IN  EFI_HANDLE                             Controller,
  OUT EFI_OPEN_PROTOCOL_INFORMATION_ENTRY   **Entries,
  OUT UINTN                                  *EntryCount
  )
{
  EFI_STATUS  Status;

  *Entries    = NULL;
  *EntryCount = 0;
  Status = gBS->OpenProtocolInformation (
                  Controller,
                  &gEfiSdMmcPassThruProtocolGuid,
                  Entries,
                  EntryCount
                  );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status) && (*Entries != NULL)) {
    FreePool (*Entries);
    *Entries    = NULL;
    *EntryCount = 0;
  }

  return Status;
}

STATIC
BOOLEAN
SdHasChildOpen (
  IN EFI_OPEN_PROTOCOL_INFORMATION_ENTRY  *Entries,
  IN UINTN                                EntryCount
  )
{
  UINTN  Index;

  if (Entries == NULL) {
    return FALSE;
  }

  for (Index = 0; Index < EntryCount; Index++) {
    if ((Entries[Index].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) != 0) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
SD_DEVICE *
SdFindTrackedChild (
  IN SD_DRIVER_PRIVATE_DATA  *Private,
  IN EFI_HANDLE              ChildHandle
  )
{
  LIST_ENTRY  *Link;
  SD_DEVICE   *Device;

  for (Link = GetFirstNode (&Private->Children);
       !IsNull (&Private->Children, Link);
       Link = GetNextNode (&Private->Children, Link))
  {
    Device = CR (Link, SD_DEVICE, ChildLink, SD_DEVICE_SIGNATURE);
    if (Device->Handle == ChildHandle) {
      return Device;
    }
  }

  return NULL;
}

STATIC
BOOLEAN
SdIsTrackedChild (
  IN SD_DRIVER_PRIVATE_DATA  *Private,
  IN EFI_HANDLE              ChildHandle
  )
{
  return (BOOLEAN)(SdFindTrackedChild (Private, ChildHandle) != NULL);
}

STATIC
VOID
SdForgetChild (
  IN SD_DRIVER_PRIVATE_DATA  *Private,
  IN SD_DEVICE               *Device
  )
{
  if (Device->ChildTracked) {
    RemoveEntryList (&Device->ChildLink);
    Device->ChildTracked = FALSE;
    ASSERT (Private->ChildCount != 0);
    if (Private->ChildCount != 0) {
      Private->ChildCount--;
    }
  }
}

STATIC
BOOLEAN
SdClaimChildStop (
  IN SD_DEVICE  *Device
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if ((Device->ActiveOperations != 0) || Device->ResetInProgress ||
      Device->StopInProgress || !IsListEmpty (&Device->Queue))
  {
    gBS->RestoreTPL (OldTpl);
    return FALSE;
  }

  Device->StopInProgress = TRUE;
  gBS->RestoreTPL (OldTpl);
  return TRUE;
}

STATIC
VOID
SdReleaseChildStop (
  IN SD_DEVICE  *Device
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Device->StopInProgress = FALSE;
  gBS->RestoreTPL (OldTpl);
}

STATIC
EFI_STATUS
SdUninstallChild (
  IN SD_DRIVER_PRIVATE_DATA  *Private,
  IN SD_DEVICE               *Device
  )
{
  EFI_STATUS  Status;

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Device->Handle,
                  &gEfiDevicePathProtocolGuid,
                  Device->DevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &Device->BlockIo,
                  &gEfiBlockIo2ProtocolGuid,
                  &Device->BlockIo2,
                  &gEfiEraseBlockProtocolGuid,
                  &Device->EraseBlock,
                  &gEfiDiskInfoProtocolGuid,
                  &Device->DiskInfo,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SdForgetChild (Private, Device);
  FreePool (Device->DevicePath);
  if (Device->ControllerNameTable != NULL) {
    FreeUnicodeStringTable (Device->ControllerNameTable);
  }

  FreePool (Device);
  return EFI_SUCCESS;
}

/**
  Scan SD Bus to discover the device.

  @param[in]  Private             The SD driver private data structure.
  @param[in]  Slot                The slot number to check device present.
  @param[out] ChildInstalled      TRUE when this slot's child is published or tracked.

  @retval EFI_SUCCESS             Successfully to discover the device and attach
                                  SdMmcIoProtocol to it.
  @retval EFI_OUT_OF_RESOURCES    The request could not be completed due to a lack
                                  of resources.
  @retval EFI_ALREADY_STARTED     The device was discovered before.
  @retval Others                  Fail to discover the device.

**/
EFI_STATUS
EFIAPI
DiscoverSdDevice (
  IN  SD_DRIVER_PRIVATE_DATA  *Private,
  IN  UINT8                   Slot,
  OUT BOOLEAN                 *ChildInstalled
  )
{
  EFI_STATUS                     Status;
  SD_DEVICE                      *Device;
  EFI_DEVICE_PATH_PROTOCOL       *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL       *NewDevicePath;
  EFI_DEVICE_PATH_PROTOCOL       *RemainingDevicePath;
  EFI_HANDLE                     DeviceHandle;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;

  Device              = NULL;
  *ChildInstalled     = FALSE;
  DevicePath          = NULL;
  NewDevicePath       = NULL;
  RemainingDevicePath = NULL;
  PassThru            = Private->PassThru;

  //
  // Build Device Path
  //
  Status = PassThru->BuildDevicePath (
                       PassThru,
                       Slot,
                       &DevicePath
                       );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (DevicePath->SubType != MSG_SD_DP) {
    Status = EFI_UNSUPPORTED;
    goto Error;
  }

  NewDevicePath = AppendDevicePathNode (
                    Private->ParentDevicePath,
                    DevicePath
                    );

  if (NewDevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  DeviceHandle        = NULL;
  RemainingDevicePath = NewDevicePath;
  Status              = gBS->LocateDevicePath (&gEfiDevicePathProtocolGuid, &RemainingDevicePath, &DeviceHandle);
  if (!EFI_ERROR (Status) && (DeviceHandle != NULL) && IsDevicePathEnd (RemainingDevicePath)) {
    if (SdIsTrackedChild (Private, DeviceHandle)) {
      *ChildInstalled = TRUE;
      // A previously published orphan may have lost its parent-child link.
      // Re-establish it when rediscovery reaches the same child path.
      Status = gBS->OpenProtocol (
                        Private->Controller,
                        &gEfiSdMmcPassThruProtocolGuid,
                        (VOID **)&(Private->PassThru),
                        Private->DriverBindingHandle,
                        DeviceHandle,
                        EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                        );
    }

    // The child path already exists. Keep it and let an explicit Stop retry
    // cleanup if its relationship could not be restored here.
    Status = EFI_ALREADY_STARTED;
    FreePool (NewDevicePath);
    NewDevicePath = NULL;
    goto Error;
  }

  //
  // Allocate buffer to store SD_DEVICE private data.
  //
  Device = AllocateCopyPool (sizeof (SD_DEVICE), &mSdDeviceTemplate);
  if (Device == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  Device->DevicePath = NewDevicePath;
  Device->Slot       = Slot;
  Device->Private    = Private;
  InitializeListHead (&Device->Queue);

  //
  // Expose user area in the Sd memory card to upper layer.
  //
  Status = DiscoverUserArea (Device);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Device->ControllerNameTable = NULL;
  AddUnicodeString2 (
    "eng",
    gSdDxeComponentName.SupportedLanguages,
    &Device->ControllerNameTable,
    Device->ModelName,
    TRUE
    );
  AddUnicodeString2 (
    "en",
    gSdDxeComponentName2.SupportedLanguages,
    &Device->ControllerNameTable,
    Device->ModelName,
    FALSE
    );

  if (Private->ChildCount == MAX_UINTN) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  // Track and pin the child before publishing protocols, whose notifications
  // may run before InstallMultipleProtocolInterfaces returns.
  InsertTailList (&Private->Children, &Device->ChildLink);
  Device->ChildTracked = TRUE;
  Private->ChildCount++;
  Device->StopInProgress = TRUE;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Device->Handle,
                  &gEfiDevicePathProtocolGuid,
                  Device->DevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &Device->BlockIo,
                  &gEfiBlockIo2ProtocolGuid,
                  &Device->BlockIo2,
                  &gEfiEraseBlockProtocolGuid,
                  &Device->EraseBlock,
                  &gEfiDiskInfoProtocolGuid,
                  &Device->DiskInfo,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    Device->StopInProgress = FALSE;
    SdForgetChild (Private, Device);
  } else {
    *ChildInstalled = TRUE;
    Status = gBS->OpenProtocol (
                      Private->Controller,
                      &gEfiSdMmcPassThruProtocolGuid,
                      (VOID **)&(Private->PassThru),
                      Private->DriverBindingHandle,
                      Device->Handle,
                      EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                      );
    if (Status == EFI_ALREADY_STARTED) {
      SdReleaseChildStop (Device);
      return EFI_SUCCESS;
    }

    if (EFI_ERROR (Status)) {
      EFI_STATUS  OpenStatus;
      EFI_STATUS  UninstallStatus;

      OpenStatus = Status;
      UninstallStatus = gBS->UninstallMultipleProtocolInterfaces (
                          Device->Handle,
                          &gEfiDevicePathProtocolGuid,
                          Device->DevicePath,
                          &gEfiBlockIoProtocolGuid,
                          &Device->BlockIo,
                          &gEfiBlockIo2ProtocolGuid,
                          &Device->BlockIo2,
                          &gEfiEraseBlockProtocolGuid,
                          &Device->EraseBlock,
                          &gEfiDiskInfoProtocolGuid,
                          &Device->DiskInfo,
                          NULL
                          );
      if (EFI_ERROR (UninstallStatus)) {
        // Protocol consumers may already hold the child; keep its backing
        // storage and retry the parent/child relationship once.
        Status = gBS->OpenProtocol (
                          Private->Controller,
                          &gEfiSdMmcPassThruProtocolGuid,
                          (VOID **)&(Private->PassThru),
                          Private->DriverBindingHandle,
                          Device->Handle,
                          EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                          );
        FreePool (DevicePath);
        if (!EFI_ERROR (Status) || (Status == EFI_ALREADY_STARTED)) {
          SdReleaseChildStop (Device);
          return EFI_SUCCESS;
        }

        // The child remains published but is not linked yet. Tombstone it and
        // retain it in the private list so Stop(0) can retry its removal.
        SdInvalidateMedia (Device);
        SdReleaseChildStop (Device);
        return UninstallStatus;
      }

      SdReleaseChildStop (Device);
      SdForgetChild (Private, Device);
      *ChildInstalled = FALSE;
      Status = OpenStatus;
    } else {
      SdReleaseChildStop (Device);
    }
  }

Error:
  FreePool (DevicePath);

  if (EFI_ERROR (Status) && (NewDevicePath != NULL)) {
    FreePool (NewDevicePath);
  }

  if (EFI_ERROR (Status) && (Device != NULL)) {
    if (Device->ControllerNameTable != NULL) {
      FreeUnicodeStringTable (Device->ControllerNameTable);
    }

    FreePool (Device);
  }

  return Status;
}

/**
  Tests to see if this driver supports a given controller. If a child device is provided,
  it further tests to see if this driver supports creating a handle for the specified child device.

  This function checks to see if the driver specified by This supports the device specified by
  ControllerHandle. Drivers will typically use the device path attached to
  ControllerHandle and/or the services from the bus I/O abstraction attached to
  ControllerHandle to determine if the driver supports ControllerHandle. This function
  may be called many times during platform initialization. In order to reduce boot times, the tests
  performed by this function must be very small, and take as little time as possible to execute. This
  function must not change the state of any hardware devices, and this function must be aware that the
  device specified by ControllerHandle may already be managed by the same driver or a
  different driver. This function must match its calls to AllocatePages() with FreePages(),
  AllocatePool() with FreePool(), and OpenProtocol() with CloseProtocol().
  Since ControllerHandle may have been previously started by the same driver, if a protocol is
  already in the opened state, then it must not be closed with CloseProtocol(). This is required
  to guarantee the state of ControllerHandle is not modified by this function.

  @param[in]  This                 A pointer to the EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in]  ControllerHandle     The handle of the controller to test. This handle
                                   must support a protocol interface that supplies
                                   an I/O abstraction to the driver.
  @param[in]  RemainingDevicePath  A pointer to the remaining portion of a device path.  This
                                   parameter is ignored by device drivers, and is optional for bus
                                   drivers. For bus drivers, if this parameter is not NULL, then
                                   the bus driver must determine if the bus controller specified
                                   by ControllerHandle and the child controller specified
                                   by RemainingDevicePath are both supported by this
                                   bus driver.

  @retval EFI_SUCCESS              The device specified by ControllerHandle and
                                   RemainingDevicePath is supported by the driver specified by This.
  @retval EFI_ALREADY_STARTED      The device specified by ControllerHandle and
                                   RemainingDevicePath is already being managed by the driver
                                   specified by This.
  @retval EFI_ACCESS_DENIED        The device specified by ControllerHandle and
                                   RemainingDevicePath is already being managed by a different
                                   driver or an application that requires exclusive access.
                                   Currently not implemented.
  @retval EFI_UNSUPPORTED          The device specified by ControllerHandle and
                                   RemainingDevicePath is not supported by the driver specified by This.
**/
EFI_STATUS
EFIAPI
SdDxeDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                     Status;
  EFI_DEVICE_PATH_PROTOCOL       *ParentDevicePath;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;
  UINT8                          Slot;

  //
  // Test EFI_SD_MMC_PASS_THRU_PROTOCOL on the controller handle.
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiSdMmcPassThruProtocolGuid,
                  (VOID **)&PassThru,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );

  if (Status == EFI_ALREADY_STARTED) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Test RemainingDevicePath is valid or not.
  //
  if ((RemainingDevicePath != NULL) && !IsDevicePathEnd (RemainingDevicePath)) {
    Status = PassThru->GetSlotNumber (PassThru, RemainingDevicePath, &Slot);
    if (EFI_ERROR (Status)) {
      //
      // Close the I/O Abstraction(s) used to perform the supported test
      //
      gBS->CloseProtocol (
             Controller,
             &gEfiSdMmcPassThruProtocolGuid,
             This->DriverBindingHandle,
             Controller
             );
      return Status;
    }
  }

  //
  // Close the I/O Abstraction(s) used to perform the supported test
  //
  gBS->CloseProtocol (
         Controller,
         &gEfiSdMmcPassThruProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  //
  // Open the EFI Device Path protocol needed to perform the supported test
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&ParentDevicePath,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  return Status;
}

/**
  Starts a device controller or a bus controller.

  The Start() function is designed to be invoked from the EFI boot service ConnectController().
  As a result, much of the error checking on the parameters to Start() has been moved into this
  common boot service. It is legal to call Start() from other locations,
  but the following calling restrictions must be followed or the system behavior will not be deterministic.
  1. ControllerHandle must be a valid EFI_HANDLE.
  2. If RemainingDevicePath is not NULL, then it must be a pointer to a naturally aligned
     EFI_DEVICE_PATH_PROTOCOL.
  3. Prior to calling Start(), the Supported() function for the driver specified by This must
     have been called with the same calling parameters, and Supported() must have returned EFI_SUCCESS.

  @param[in]  This                 A pointer to the EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in]  ControllerHandle     The handle of the controller to start. This handle
                                   must support a protocol interface that supplies
                                   an I/O abstraction to the driver.
  @param[in]  RemainingDevicePath  A pointer to the remaining portion of a device path.  This
                                   parameter is ignored by device drivers, and is optional for bus
                                   drivers. For a bus driver, if this parameter is NULL, then handles
                                   for all the children of Controller are created by this driver.
                                   If this parameter is not NULL and the first Device Path Node is
                                   not the End of Device Path Node, then only the handle for the
                                   child device specified by the first Device Path Node of
                                   RemainingDevicePath is created by this driver.
                                   If the first Device Path Node of RemainingDevicePath is
                                   the End of Device Path Node, no child handle is created by this
                                   driver.

  @retval EFI_SUCCESS              The device was started.
  @retval EFI_DEVICE_ERROR         The device could not be started due to a device error.Currently not implemented.
  @retval EFI_OUT_OF_RESOURCES     The request could not be completed due to a lack of resources.
  @retval Others                   The driver failed to start the device.

**/
EFI_STATUS
EFIAPI
SdDxeDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                     Status;
  EFI_STATUS                     CleanupStatus;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;
  EFI_DEVICE_PATH_PROTOCOL       *ParentDevicePath;
  SD_DRIVER_PRIVATE_DATA         *Private;
  UINT8                          Slot;
  BOOLEAN                        ParentAlreadyStarted;
  BOOLEAN                        ParentOpenedByThisCall;
  BOOLEAN                        PrivateInstalled;
  BOOLEAN                        ChildInstalled;

  Private                 = NULL;
  PassThru                = NULL;
  ParentDevicePath        = NULL;
  ParentAlreadyStarted    = FALSE;
  ParentOpenedByThisCall  = FALSE;
  PrivateInstalled        = FALSE;
  ChildInstalled          = FALSE;
  Status   = gBS->OpenProtocol (
                    Controller,
                    &gEfiSdMmcPassThruProtocolGuid,
                    (VOID **)&PassThru,
                    This->DriverBindingHandle,
                    Controller,
                    EFI_OPEN_PROTOCOL_BY_DRIVER
                    );
  if ((EFI_ERROR (Status)) && (Status != EFI_ALREADY_STARTED)) {
    return Status;
  }
  ParentAlreadyStarted   = (BOOLEAN)(Status == EFI_ALREADY_STARTED);
  ParentOpenedByThisCall = (BOOLEAN)!ParentAlreadyStarted;

  //
  // Check EFI_ALREADY_STARTED to reuse the original SD_DRIVER_PRIVATE_DATA.
  //
  if (Status != EFI_ALREADY_STARTED) {
    Private = AllocateZeroPool (sizeof (SD_DRIVER_PRIVATE_DATA));
    if (Private == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Error;
    }

    Status = gBS->OpenProtocol (
                    Controller,
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&ParentDevicePath,
                    This->DriverBindingHandle,
                    Controller,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      goto Error;
    }

    Private->PassThru            = PassThru;
    Private->Controller          = Controller;
    Private->ParentDevicePath    = ParentDevicePath;
    Private->DriverBindingHandle = This->DriverBindingHandle;
    InitializeListHead (&Private->Children);

    Status = gBS->InstallProtocolInterface (
                    &Controller,
                    &gEfiCallerIdGuid,
                    EFI_NATIVE_INTERFACE,
                    Private
                    );
    if (EFI_ERROR (Status)) {
      goto Error;
    }

    PrivateInstalled = TRUE;
  } else {
    Status = gBS->OpenProtocol (
                    Controller,
                    &gEfiCallerIdGuid,
                    (VOID **)&Private,
                    This->DriverBindingHandle,
                    Controller,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      goto Error;
    }

    PrivateInstalled = TRUE;
    PassThru         = Private->PassThru;
  }

  if (RemainingDevicePath == NULL) {
    Slot = 0xFF;
    while (TRUE) {
      Status = PassThru->GetNextSlot (PassThru, &Slot);
      if (EFI_ERROR (Status)) {
        //
        // Cannot find more legal slots.
        //
        Status = EFI_SUCCESS;
        break;
      }

      ChildInstalled = FALSE;
      Status = DiscoverSdDevice (Private, Slot, &ChildInstalled);
      if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) {
        break;
      }
    }
  } else if (!IsDevicePathEnd (RemainingDevicePath)) {
    Status = PassThru->GetSlotNumber (PassThru, RemainingDevicePath, &Slot);
    if (!EFI_ERROR (Status)) {
      ChildInstalled = FALSE;
      Status = DiscoverSdDevice (Private, Slot, &ChildInstalled);
    }
  }

Error:
  if (Status == EFI_ALREADY_STARTED) {
    // Existing tracked children are handled as rediscovery success; keep the
    // binding active so a later Start can retry an orphan relationship.
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) {
    if ((Private != NULL) && (Private->ChildCount != 0)) {
      // A partial Start remains bound so its children, including tombstoned
      // orphans, can be retried through a later Start or Stop call.
      return EFI_SUCCESS;
    }

    if (ParentAlreadyStarted) {
      // Do not tear down state established by an earlier Start invocation.
      return Status;
    }

    if (ParentOpenedByThisCall) {
      CleanupStatus = gBS->CloseProtocol (
                             Controller,
                             &gEfiSdMmcPassThruProtocolGuid,
                             This->DriverBindingHandle,
                             Controller
                             );
      if (EFI_ERROR (CleanupStatus) && (CleanupStatus != EFI_NOT_FOUND)) {
        return CleanupStatus;
      }

      ParentOpenedByThisCall = FALSE;
    }

    if (PrivateInstalled) {
      CleanupStatus = gBS->UninstallProtocolInterface (
                             Controller,
                             &gEfiCallerIdGuid,
                             Private
                             );
      if (EFI_ERROR (CleanupStatus)) {
        // Preserve the private object and restore the parent binding if it
        // was closed. A later Stop can retry this failed cleanup.
        if (!ParentOpenedByThisCall) {
          CleanupStatus = gBS->OpenProtocol (
                                 Controller,
                                 &gEfiSdMmcPassThruProtocolGuid,
                                 (VOID **)&PassThru,
                                 This->DriverBindingHandle,
                                 Controller,
                                 EFI_OPEN_PROTOCOL_BY_DRIVER
                                 );
          if (!EFI_ERROR (CleanupStatus) || (CleanupStatus == EFI_ALREADY_STARTED)) {
            ParentOpenedByThisCall = TRUE;
          }
        }

        return Status;
      }

      PrivateInstalled = FALSE;
    }

    if (Private != NULL) {
      FreePool (Private);
    }
  }

  return Status;
}

/**
  Stops a device controller or a bus controller.

  The Stop() function is designed to be invoked from the EFI boot service DisconnectController().
  As a result, much of the error checking on the parameters to Stop() has been moved
  into this common boot service. It is legal to call Stop() from other locations,
  but the following calling restrictions must be followed or the system behavior will not be deterministic.
  1. ControllerHandle must be a valid EFI_HANDLE that was used on a previous call to this
     same driver's Start() function.
  2. The first NumberOfChildren handles of ChildHandleBuffer must all be a valid
     EFI_HANDLE. In addition, all of these handles must have been created in this driver's
     Start() function, and the Start() function must have called OpenProtocol() on
     ControllerHandle with an Attribute of EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER.

  @param[in]  This              A pointer to the EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in]  ControllerHandle  A handle to the device being stopped. The handle must
                                support a bus specific I/O protocol for the driver
                                to use to stop the device.
  @param[in]  NumberOfChildren  The number of child device handles in ChildHandleBuffer.
  @param[in]  ChildHandleBuffer An array of child handles to be freed. May be NULL
                                if NumberOfChildren is 0.

  @retval EFI_SUCCESS           The device was stopped.
  @retval EFI_DEVICE_ERROR      The device could not be stopped due to a device error.

**/
EFI_STATUS
EFIAPI
SdDxeDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                           Status;
  EFI_STATUS                           ReopenStatus;
  BOOLEAN                              AllChildrenStopped;
  UINTN                                Index;
  SD_DRIVER_PRIVATE_DATA               *Private;
  SD_DEVICE                            *Device;
  EFI_SD_MMC_PASS_THRU_PROTOCOL        *PassThru;
  EFI_BLOCK_IO2_PROTOCOL               *BlockIo2;
  EFI_BLOCK_IO_PROTOCOL                *BlockIo;
  BOOLEAN                              RetryRequired;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY  *OpenEntries;
  UINTN                                OpenEntryCount;
  LIST_ENTRY                           *Link;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiCallerIdGuid,
                  (VOID **)&Private,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (NumberOfChildren == 0) {
    Status = SdGetChildOpenInformation (Controller, &OpenEntries, &OpenEntryCount);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (SdHasChildOpen (OpenEntries, OpenEntryCount)) {
      if (OpenEntries != NULL) {
        FreePool (OpenEntries);
      }

      return EFI_NOT_READY;
    }

    if (OpenEntries != NULL) {
      FreePool (OpenEntries);
    }

    // Relationship-less published children are rollback orphans. Retry their
    // removal here so ChildCount cannot permanently block parent Stop.
    while (!IsListEmpty (&Private->Children)) {
      Link   = GetFirstNode (&Private->Children);
      Device = CR (Link, SD_DEVICE, ChildLink, SD_DEVICE_SIGNATURE);
      SdInvalidateMedia (Device);
      if (!SdClaimChildStop (Device)) {
        return EFI_NOT_READY;
      }

      Status = gBS->CloseProtocol (
                      Controller,
                      &gEfiSdMmcPassThruProtocolGuid,
                      This->DriverBindingHandle,
                      Device->Handle
                      );
      if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
        SdReleaseChildStop (Device);
        return Status;
      }

      Status = SdUninstallChild (Private, Device);
      if (EFI_ERROR (Status)) {
        // Keep the relationship absent so another Stop(0) can retry this
        // orphan after the protocol consumer releases it.
        SdReleaseChildStop (Device);
        return Status;
      }
    }

    if (Private->ChildCount != 0) {
      return EFI_NOT_READY;
    }

    // Close the parent first. If it fails, the caller-id interface still
    // exposes Private for a later Stop retry.
    Status = gBS->CloseProtocol (
                    Controller,
                    &gEfiSdMmcPassThruProtocolGuid,
                    This->DriverBindingHandle,
                    Controller
                    );
    if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
      return Status;
    }

    Status = gBS->UninstallProtocolInterface (
                    Controller,
                    &gEfiCallerIdGuid,
                    Private
                    );
    if (EFI_ERROR (Status)) {
      ReopenStatus = gBS->OpenProtocol (
                            Controller,
                            &gEfiSdMmcPassThruProtocolGuid,
                            (VOID **)&PassThru,
                            This->DriverBindingHandle,
                            Controller,
                            EFI_OPEN_PROTOCOL_BY_DRIVER
                            );
      if (EFI_ERROR (ReopenStatus) && (ReopenStatus != EFI_ALREADY_STARTED)) {
        // Keep Private installed even if the parent could not be rebound.
        return Status;
      }

      return Status;
    }

    FreePool (Private);
    return EFI_SUCCESS;
  }

  if (ChildHandleBuffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  AllChildrenStopped = TRUE;
  RetryRequired      = FALSE;

  for (Index = 0; Index < NumberOfChildren; Index++) {
    Device = SdFindTrackedChild (Private, ChildHandleBuffer[Index]);
    if ((Device == NULL) || (Device->Private != Private) || !Device->ChildTracked) {
      // Do not derive SD_DEVICE from a foreign BlockIo interface.
      AllChildrenStopped = FALSE;
      continue;
    }

    BlockIo  = NULL;
    BlockIo2 = NULL;
    Status   = gBS->OpenProtocol (
                      ChildHandleBuffer[Index],
                      &gEfiBlockIoProtocolGuid,
                      (VOID **)&BlockIo,
                      This->DriverBindingHandle,
                      Controller,
                      EFI_OPEN_PROTOCOL_GET_PROTOCOL
                      );
    if (EFI_ERROR (Status)) {
      Status = gBS->OpenProtocol (
                      ChildHandleBuffer[Index],
                      &gEfiBlockIo2ProtocolGuid,
                      (VOID **)&BlockIo2,
                      This->DriverBindingHandle,
                      Controller,
                      EFI_OPEN_PROTOCOL_GET_PROTOCOL
                      );
      if (EFI_ERROR (Status) || (BlockIo2 != &Device->BlockIo2)) {
        AllChildrenStopped = FALSE;
        continue;
      }
    } else if (BlockIo != &Device->BlockIo) {
      AllChildrenStopped = FALSE;
      continue;
    }

    // A failed or pending transfer still owns its command packet and may be
    // using caller memory. Invalidate first, then let completions drain.
    SdInvalidateMedia (Device);
    if (!SdClaimChildStop (Device)) {
      AllChildrenStopped = FALSE;
      RetryRequired      = TRUE;
      continue;
    }

    Status = gBS->CloseProtocol (
                    Controller,
                    &gEfiSdMmcPassThruProtocolGuid,
                    This->DriverBindingHandle,
                    ChildHandleBuffer[Index]
                    );
    if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
      SdReleaseChildStop (Device);
      AllChildrenStopped = FALSE;
      continue;
    }

    Status = SdUninstallChild (Private, Device);
    if (EFI_ERROR (Status)) {
      AllChildrenStopped = FALSE;
      RetryRequired      = TRUE;
      ReopenStatus       = gBS->OpenProtocol (
                                 Controller,
                                 &gEfiSdMmcPassThruProtocolGuid,
                                 (VOID **)&PassThru,
                                 This->DriverBindingHandle,
                                 ChildHandleBuffer[Index],
                                 EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                                 );
      if (EFI_ERROR (ReopenStatus) && (ReopenStatus != EFI_ALREADY_STARTED)) {
        // The tracked child remains available to an explicit Stop retry. Its
        // missing relationship is tolerated by CloseProtocol on that retry.
      }

      SdReleaseChildStop (Device);
    }
  }

  if (!AllChildrenStopped) {
    return RetryRequired ? EFI_NOT_READY : EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  The user Entry Point for module SdDxe. The user code starts with this function.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some errors occur when executing this entry point.

**/
EFI_STATUS
EFIAPI
InitializeSdDxe (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  //
  // Install driver model protocol(s).
  //
  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &gSdDxeDriverBinding,
             ImageHandle,
             &gSdDxeComponentName,
             &gSdDxeComponentName2
             );
  ASSERT_EFI_ERROR (Status);

  return Status;
}
