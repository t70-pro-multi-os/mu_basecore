/** @file
  Sample ACPI Platform Driver

  Copyright (c) 2008 - 2018, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>

#include <Protocol/AcpiTable.h>
#include <Protocol/FirmwareVolume2.h>

#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>

#include <IndustryStandard/Acpi.h>

/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **Instance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with ACPI storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *Instance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}

/**
  This function calculates and updates an UINT8 checksum.

  @param  Buffer          Pointer to buffer to checksum
  @param  Size            Number of bytes to checksum

**/
VOID
AcpiPlatformChecksum (
  IN UINT8  *Buffer,
  IN UINTN  Size
  )
{
  UINTN  ChecksumOffset;

  ChecksumOffset = OFFSET_OF (EFI_ACPI_DESCRIPTION_HEADER, Checksum);

  //
  // Set checksum to 0 first
  //
  Buffer[ChecksumOffset] = 0;

  //
  // Update checksum value
  //
  Buffer[ChecksumOffset] = CalculateCheckSum8 (Buffer, Size);
}

/**
  Entrypoint of Acpi Platform driver.

  @param  ImageHandle
  @param  SystemTable

  @return EFI_SUCCESS
  @return EFI_LOAD_ERROR
  @return EFI_OUT_OF_RESOURCES

**/
EFI_STATUS
EFIAPI
AcpiPlatformEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  EFI_ACPI_TABLE_PROTOCOL        *AcpiTable;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FwVol;
  INTN                           Instance;
  EFI_ACPI_COMMON_HEADER         *CurrentTable;
  UINTN                          TableHandle;
  UINT32                         FvStatus;
  UINTN                          TableSize;
  UINTN                          Size;
  typedef struct TABLE_KEY_NODE {
    struct TABLE_KEY_NODE  *Next;
    UINTN                  Key;
  } TABLE_KEY_NODE;
  TABLE_KEY_NODE                 *Installed;
  TABLE_KEY_NODE                 *Node;
  EFI_STATUS                     RollbackStatus;

  Installed    = NULL;
  Instance     = 0;
  CurrentTable = NULL;
  TableHandle  = 0;

  //
  // Find the AcpiTable protocol
  //
  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    return EFI_ABORTED;
  }

  //
  // Locate the firmware volume protocol
  //
  Status = LocateFvInstanceWithTables (&FwVol);
  if (EFI_ERROR (Status)) {
    return EFI_ABORTED;
  }

  // Keep the keys from this invocation only. A failed read/install must not
  // leave a silently truncated platform table set behind.
  for (;;) {
    CurrentTable = NULL;
    Size         = 0;
    Status = FwVol->ReadSection (
                      FwVol,
                      (EFI_GUID *)PcdGetPtr (PcdAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (EFI_ERROR (Status)) {
      // A failed read does not transfer ownership of its output buffer.
      // Do not pass an undefined failure output to FreePool during rollback.
      CurrentTable = NULL;
      if ((Status == EFI_NOT_FOUND) && (Instance != 0)) {
        break;
      }
      goto Failed;
    }

    // Assertions are absent in RELEASE. Validate before reading the length
    // or computing a checksum over an FV-supplied buffer.
    if ((CurrentTable == NULL) || (Size < sizeof (EFI_ACPI_DESCRIPTION_HEADER))) {
      Status = EFI_COMPROMISED_DATA;
      goto Failed;
    }
    TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
    if ((TableSize < sizeof (EFI_ACPI_DESCRIPTION_HEADER)) || (TableSize > Size)) {
      Status = EFI_COMPROMISED_DATA;
      goto Failed;
    }

    // Reserve bookkeeping before installation, so allocation failure cannot
    // strand a successfully installed table without a rollback key.
    Status = gBS->AllocatePool (EfiBootServicesData, sizeof (*Node), (VOID **)&Node);
    if (EFI_ERROR (Status)) {
      goto Failed;
    }
    AcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);
    TableHandle = 0;
    Status = AcpiTable->InstallAcpiTable (AcpiTable, CurrentTable, TableSize, &TableHandle);
    gBS->FreePool (CurrentTable);
    CurrentTable = NULL;
    if (EFI_ERROR (Status) && (TableHandle == 0)) {
      gBS->FreePool (Node);
      goto Failed;
    }
    // This AcpiTableDxe can assign a key before PublishTables fails. Include
    // that key in rollback too; otherwise the failed install itself leaks.
    Node->Key  = TableHandle;
    Node->Next = Installed;
    Installed  = Node;
    if (EFI_ERROR (Status)) {
      goto Failed;
    }
    Instance++;
  }

  while (Installed != NULL) {
    Node      = Installed;
    Installed = Node->Next;
    gBS->FreePool (Node);
  }
  return EFI_REQUEST_UNLOAD_IMAGE;

Failed:
  if (CurrentTable != NULL) {
    gBS->FreePool (CurrentTable);
  }
  // Reverse installation order, and attempt every removal even if one fails.
  // A rollback failure is surfaced, never reported as successful cleanup.
  while (Installed != NULL) {
    Node           = Installed;
    Installed      = Node->Next;
    RollbackStatus = AcpiTable->UninstallAcpiTable (AcpiTable, Node->Key);
    if (EFI_ERROR (RollbackStatus)) {
      DEBUG ((DEBUG_ERROR, "ACPI platform rollback failed for key %Lu: %r\n", (UINT64)Node->Key, RollbackStatus));
      Status = EFI_DEVICE_ERROR;
    }
    gBS->FreePool (Node);
  }
  return Status;
}
