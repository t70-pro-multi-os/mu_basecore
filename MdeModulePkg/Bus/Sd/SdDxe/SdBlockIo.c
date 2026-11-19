/** @file
  The helper functions for BlockIo and BlockIo2 protocol.

  Copyright (c) 2015 - 2018, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "SdDxe.h"

#define SD_MAX_BLOCKS_PER_COMMAND     0xFFFFU
#define SD_WRITE_STATUS_POLLS       100
#define SD_WRITE_STATUS_INTERVAL_US  10000
#define SD_WRITE_STATUS_TIMEOUT_US   10000
#define SD_ORDERED_IO_PERIOD_100NS   10000

#define SD_R1_READY_FOR_DATA          BIT8
#define SD_R1_STATE_MASK              (0xFU << 9)
#define SD_R1_STATE_TRAN              (4U << 9)
#define SD_R1_WRITE_PROTECT           (BIT26 | BIT25)
#define SD_R1_ERRORS                  (BIT31 | BIT30 | BIT29 | BIT28 | BIT27 | BIT24 | BIT23 | \
                                       BIT22 | BIT21 | BIT20 | BIT19 | BIT16)

typedef enum {
  SdOrderedRead,
  SdOrderedWrite,
  SdOrderedFlush
} SD_ORDERED_OPERATION;

typedef struct {
  LIST_ENTRY            Link;
  SD_ORDERED_OPERATION  Operation;
  EFI_BLOCK_IO2_TOKEN   *Token;
  EFI_LBA               Lba;
  VOID                  *Buffer;
  UINTN                 BytesRemaining;
  UINTN                 WriteSize;
  BOOLEAN               TransferPending;
  SD_REQUEST            Transfer;
} SD_ORDERED_IO;

/**
  Check the media ID and latched state as one TPL-protected observation.
**/
static EFI_STATUS
SdValidateMediaId (
  IN SD_DEVICE  *Device,
  IN UINT32     MediaId
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (MediaId != Device->BlockMedia.MediaId) {
    Status = EFI_MEDIA_CHANGED;
  } else if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
    Status = EFI_NO_MEDIA;
  } else {
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Convert the PassThru no-slot status to the Block I/O no-media status.
**/
static EFI_STATUS
SdNormalizeMediaStatus (
  IN EFI_STATUS  Status
  )
{
  return (Status == EFI_NOT_FOUND) ? EFI_NO_MEDIA : Status;
}

/**
  Reuse PassThru BuildDevicePath as a readiness probe. Success never refreshes
  a child's identity or geometry.
**/
static EFI_STATUS
SdCheckMediaReady (
  IN SD_DEVICE  *Device,
  IN BOOLEAN    Probe
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_TPL                   OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NO_MEDIA;
  }

  gBS->RestoreTPL (OldTpl);
  if (!Probe) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
      Status = EFI_NO_MEDIA;
    } else {
      Status = EFI_SUCCESS;
    }

    gBS->RestoreTPL (OldTpl);
    return Status;
  }

  DevicePath = NULL;
  Status = Device->Private->PassThru->BuildDevicePath (
             Device->Private->PassThru,
             Device->Slot,
             &DevicePath
             );
  if (EFI_ERROR (Status)) {
    if (DevicePath != NULL) {
      FreePool (DevicePath);
    }

    if ((Status == EFI_NO_MEDIA) || (Status == EFI_NOT_FOUND) ||
        (Status == EFI_MEDIA_CHANGED)) {
      SdInvalidateMedia (Device);
    }

    return SdNormalizeMediaStatus (Status);
  }

  if ((DevicePath == NULL) ||
      (DevicePath->Type != MESSAGING_DEVICE_PATH) ||
      (DevicePath->SubType != MSG_SD_DP) ||
      (DevicePathNodeLength (DevicePath) != sizeof (SD_DEVICE_PATH)) ||
      (((SD_DEVICE_PATH *)DevicePath)->SlotNumber != Device->Slot)) {
    if (DevicePath != NULL) {
      FreePool (DevicePath);
    }

    return EFI_DEVICE_ERROR;
  }

  FreePool (DevicePath);

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
    Status = EFI_NO_MEDIA;
  } else {
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Pin a public I/O call before reading child state or probing the slot.
**/
static EFI_STATUS
SdBeginOperation (
  IN SD_DEVICE  *Device,
  IN BOOLEAN    CheckMediaId,
  IN UINT32     MediaId
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (CheckMediaId && (MediaId != Device->BlockMedia.MediaId)) {
    Status = EFI_MEDIA_CHANGED;
  } else if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
    Status = EFI_NO_MEDIA;
  } else if (Device->ResetInProgress || (Device->ActiveOperations == MAX_UINT32)) {
    Status = EFI_NOT_READY;
  } else {
    Device->ActiveOperations++;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Release a public I/O call pin.
**/
static VOID
SdEndOperation (
  IN SD_DEVICE  *Device
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ASSERT (Device->ActiveOperations != 0);
  if (Device->ActiveOperations != 0) {
    Device->ActiveOperations--;
  }

  gBS->RestoreTPL (OldTpl);
}

/**
  Add a request only if media invalidation/Stop has not closed admission.
**/
static EFI_STATUS
SdEnqueueRequest (
  IN SD_DEVICE   *Device,
  IN SD_REQUEST  *Request
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
    Status = EFI_NO_MEDIA;
  } else if (Device->ResetInProgress) {
    Status = EFI_NOT_READY;
  } else {
    Request->Device = Device;
    InsertTailList (&Device->Queue, &Request->Link);
    Request->IsQueued = TRUE;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Remove a request from its owner's queue exactly once.
**/
static VOID
SdDequeueRequest (
  IN SD_DEVICE   *Device,
  IN SD_REQUEST  *Request
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Request->IsQueued) {
    RemoveEntryList (&Request->Link);
    Request->IsQueued = FALSE;
  }

  gBS->RestoreTPL (OldTpl);
}

static VOID
SdInvalidateMediaOnStatus (
  IN SD_DEVICE   *Device,
  IN EFI_STATUS  Status
  )
{
  if (SdMediaUnavailableStatus (Status)) {
    SdInvalidateMedia (Device);
  }
}

static EFI_STATUS
SdBeginReset (
  IN SD_DEVICE  *Device
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
    Status = EFI_NO_MEDIA;
  } else if (Device->ResetInProgress || (Device->ActiveOperations > 1) ||
             !IsListEmpty (&Device->Queue) || !IsListEmpty (&Device->OrderedIo)) {
    Status = EFI_NOT_READY;
  } else {
    Device->ResetInProgress = TRUE;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}

static EFI_STATUS
SdEndReset (
  IN SD_DEVICE   *Device,
  IN EFI_STATUS  Status
  )
{
  EFI_TPL  OldTpl;

  SdInvalidateMediaOnStatus (Device, Status);
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Device->ResetInProgress = FALSE;
  if ((Status == EFI_SUCCESS) && Device->MediaInvalidated) {
    Status = EFI_NO_MEDIA;
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}

/** Release one batch reference at TPL_NOTIFY, notifying only after all owners end. **/
static VOID
SdReleaseBatchAtNotify (
  IN SD_IO_BATCH  *Batch,
  IN EFI_STATUS   Status
  )
{
  EFI_EVENT Event;
  if (Batch == NULL) { return; }
  if (EFI_ERROR (Status) && !EFI_ERROR (Batch->Status)) { Batch->Status = Status; }
  ASSERT (Batch->References != 0);
  if (--Batch->References != 0) { return; }
  Event = NULL;
  if (Batch->Accepted) {
    Batch->Token->TransactionStatus = Batch->Status;
    Event = Batch->Token->Event;
  }
  FreePool (Batch);
  if (Event != NULL) { gBS->SignalEvent (Event); }
}

static VOID
SdAttachBatch (
  IN SD_REQUEST   *Request,
  IN SD_IO_BATCH  *Batch
  )
{
  EFI_TPL OldTpl;
  if (Batch == NULL) { return; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Batch->References++;
  Request->Batch = Batch;
  gBS->RestoreTPL (OldTpl);
}

/**
  Nonblocking I/O callback function when the event is signaled.

  @param[in]  Event     The Event this notify function registered to.
  @param[in]  Context   Pointer to the context data registered to the
                        Event.

**/
VOID
EFIAPI
AsyncIoCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SD_REQUEST  *Request;

  gBS->CloseEvent (Event);

  Request = (SD_REQUEST *)Context;
  if (SdMediaUnavailableStatus (Request->Packet.TransactionStatus)) {
    SdInvalidateMediaAtNotify (Request->Device);
  }

  DEBUG_CODE_BEGIN ();
  DEBUG ((
    DEBUG_INFO,
    "Sd Async Request: CmdIndex[%d] Arg[%08x] %r\n",
    Request->SdMmcCmdBlk.CommandIndex,
    Request->SdMmcCmdBlk.CommandArgument,
    Request->Packet.TransactionStatus
    ));
  DEBUG_CODE_END ();

  if ((Request->Batch == NULL) && EFI_ERROR (Request->Packet.TransactionStatus)) {
    Request->Token->TransactionStatus = SdNormalizeMediaStatus (Request->Packet.TransactionStatus);
  }

  // This event runs at TPL_NOTIFY. Remove the queue pin directly so we do not
  // attempt to raise to the same TPL from inside the callback.
  if (Request->IsQueued) {
    RemoveEntryList (&Request->Link);
    Request->IsQueued = FALSE;
  }

  if (Request->Batch != NULL) {
    SD_IO_BATCH *Batch = Request->Batch;
    EFI_STATUS Status = SdNormalizeMediaStatus (Request->Packet.TransactionStatus);
    FreePool (Request);
    SdReleaseBatchAtNotify (Batch, Status);
    return;
  }

  if (Request->IsEnd) {
    gBS->SignalEvent (Request->Token->Event);
  }

  FreePool (Request);
}

/**
  Send command SET_RELATIVE_ADDRESS to the device to set the device address.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[out] Rca               The relative device address to assign.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
EFI_STATUS
SdSetRca (
  IN     SD_DEVICE  *Device,
  OUT UINT16        *Rca
  )
{
  EFI_STATUS                           Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL        *PassThru;
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;

  PassThru = Device->Private->PassThru;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));
  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = SD_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex = SD_SET_RELATIVE_ADDR;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeBcr;
  SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR6;

  Status = PassThru->PassThru (PassThru, Device->Slot, &Packet, NULL);
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "Set RCA succeeds with Resp0 = 0x%x\n", SdMmcStatusBlk.Resp0));
    *Rca = (UINT16)(SdMmcStatusBlk.Resp0 >> 16);
  }

  return Status;
}

/**
  Send command SELECT to the device to select/deselect the device.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[in]  Rca               The relative device address to use.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
EFI_STATUS
SdSelect (
  IN     SD_DEVICE  *Device,
  IN     UINT16     Rca
  )
{
  EFI_STATUS                           Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL        *PassThru;
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;

  PassThru = Device->Private->PassThru;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));
  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = SD_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex = SD_SELECT_DESELECT_CARD;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeAc;
  if (Rca != 0) {
    SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR1b;
  }

  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = PassThru->PassThru (PassThru, Device->Slot, &Packet, NULL);

  return Status;
}

/**
  Send command SEND_STATUS to the device to get device status.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[in]  Rca               The relative device address to use.
  @param[in]  Timeout           The command timeout in microseconds.
  @param[out] DevStatus         The buffer to store the device status.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
EFI_STATUS
SdSendStatus (
  IN     SD_DEVICE  *Device,
  IN     UINT16     Rca,
  IN     UINT64     Timeout,
  OUT    UINT32     *DevStatus
  )
{
  EFI_STATUS                           Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL        *PassThru;
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;

  PassThru = Device->Private->PassThru;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));
  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout       = Timeout;

  SdMmcCmdBlk.CommandIndex    = SD_SEND_STATUS;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR1;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = PassThru->PassThru (PassThru, Device->Slot, &Packet, NULL);
  if (!EFI_ERROR (Status)) {
    CopyMem (DevStatus, &SdMmcStatusBlk.Resp0, sizeof (UINT32));
  }

  return Status;
}

static EFI_STATUS
SdWriteResponseStatus (
  IN UINT32  Response
  )
{
  if ((Response & SD_R1_WRITE_PROTECT) != 0) {
    return EFI_WRITE_PROTECTED;
  }

  return ((Response & SD_R1_ERRORS) != 0) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

/**
  Confirm programming after CMD24/CMD25. The caller owns the card I/O order
  throughout this function and runs at TPL_CALLBACK or below.
**/
static EFI_STATUS
SdConfirmWrite (
  IN SD_DEVICE  *Device,
  IN UINT32     WriteResponse
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  WriteStatus;
  UINT32      Response;
  UINTN       Attempt;

  WriteStatus = SdWriteResponseStatus (WriteResponse);
  for (Attempt = 0; Attempt < SD_WRITE_STATUS_POLLS; Attempt++) {
    Status = SdCheckMediaReady (Device, FALSE);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = SdSendStatus (Device, Device->Rca, SD_WRITE_STATUS_TIMEOUT_US, &Response);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = SdCheckMediaReady (Device, FALSE);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // Some R1 error bits clear when read. Keep an error seen while still busy.
    if (!EFI_ERROR (WriteStatus)) {
      WriteStatus = SdWriteResponseStatus (Response);
    }

    if ((Response & (SD_R1_READY_FOR_DATA | SD_R1_STATE_MASK)) ==
        (SD_R1_READY_FOR_DATA | SD_R1_STATE_TRAN))
    {
      Device->WriteStatusPending = FALSE;
      return WriteStatus;
    }

    if (Attempt + 1 < SD_WRITE_STATUS_POLLS) {
      gBS->Stall (SD_WRITE_STATUS_INTERVAL_US);
    }
  }

  return EFI_ERROR (WriteStatus) ? WriteStatus : EFI_TIMEOUT;
}

/**
  Admit direct I/O in the absence of an ordered request. A synchronous writer
  or a caller rechecking a timed-out write needs exclusive use of the card.
  Release Exclusive even if the status recheck fails.
**/
static EFI_STATUS
SdBeginImmediateIo (
  IN  SD_DEVICE  *Device,
  IN  BOOLEAN    IsWrite,
  IN  BOOLEAN    IsAsync,
  OUT BOOLEAN    *Exclusive
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  *Exclusive = FALSE;
  Status     = EFI_SUCCESS;
  OldTpl     = gBS->RaiseTPL (TPL_NOTIFY);
  if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
    Status = EFI_NO_MEDIA;
  } else if (Device->WriteInProgress || !IsListEmpty (&Device->OrderedIo)) {
    Status = EFI_NOT_READY;
  } else if (IsWrite || Device->WriteStatusPending) {
    if (IsAsync || (Device->ActiveOperations != 1) || !IsListEmpty (&Device->Queue)) {
      Status = EFI_NOT_READY;
    } else {
      Device->WriteInProgress = TRUE;
      *Exclusive              = TRUE;
    }
  }

  gBS->RestoreTPL (OldTpl);
  if (*Exclusive && Device->WriteStatusPending) {
    Status = SdConfirmWrite (Device, 0);
    SdInvalidateMediaOnStatus (Device, Status);
    Status = SdNormalizeMediaStatus (Status);
  }

  return Status;
}

static VOID
SdEndImmediateIo (
  IN SD_DEVICE  *Device,
  IN BOOLEAN    Exclusive
  )
{
  EFI_TPL  OldTpl;

  if (Exclusive) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    Device->WriteInProgress = FALSE;
    gBS->RestoreTPL (OldTpl);
  }
}

/**
  Send command SEND_CSD to the device to get the CSD register data.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[in]  Rca               The relative device address to use.
  @param[out] Csd               The buffer to store the SD_CSD register data.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
EFI_STATUS
SdGetCsd (
  IN     SD_DEVICE  *Device,
  IN     UINT16     Rca,
  OUT SD_CSD        *Csd
  )
{
  EFI_STATUS                           Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL        *PassThru;
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;

  PassThru = Device->Private->PassThru;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));
  ZeroMem (Csd, sizeof (SD_CSD));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = SD_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = SD_SEND_CSD;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR2;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = PassThru->PassThru (PassThru, Device->Slot, &Packet, NULL);

  if (!EFI_ERROR (Status)) {
    //
    // For details, refer to SD Host Controller Simplified Spec 3.0 Table 2-12.
    //
    CopyMem (((UINT8 *)Csd) + 1, &SdMmcStatusBlk.Resp0, sizeof (SD_CSD) - 1);
  }

  return Status;
}

/**
  Send command SEND_CID to the device to get the CID register data.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[in]  Rca               The relative device address to use.
  @param[out] Cid               The buffer to store the SD_CID register data.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
EFI_STATUS
SdGetCid (
  IN     SD_DEVICE  *Device,
  IN     UINT16     Rca,
  OUT SD_CID        *Cid
  )
{
  EFI_STATUS                           Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL        *PassThru;
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;

  PassThru = Device->Private->PassThru;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));
  ZeroMem (Cid, sizeof (SD_CID));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = SD_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = SD_SEND_CID;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR2;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = PassThru->PassThru (PassThru, Device->Slot, &Packet, NULL);

  if (!EFI_ERROR (Status)) {
    //
    // For details, refer to SD Host Controller Simplified Spec 3.0 Table 2-12.
    //
    CopyMem (((UINT8 *)Cid) + 1, &SdMmcStatusBlk.Resp0, sizeof (SD_CID) - 1);
  }

  return Status;
}

static VOID
SdInitRwPacket (
  IN SD_DEVICE   *Device,
  IN SD_REQUEST  *Request,
  IN EFI_LBA     Lba,
  IN VOID        *Buffer,
  IN UINTN       BufferSize,
  IN BOOLEAN     IsRead
  )
{
  BOOLEAN  SingleBlock;

  SingleBlock = (BOOLEAN)(BufferSize == Device->BlockMedia.BlockSize);
  ZeroMem (&Request->Packet, sizeof (Request->Packet));
  ZeroMem (&Request->SdMmcCmdBlk, sizeof (Request->SdMmcCmdBlk));
  ZeroMem (&Request->SdMmcStatusBlk, sizeof (Request->SdMmcStatusBlk));
  // Use 2 MiB/s, the lower bound for a Speed Class 2 card, plus one second.
  Request->Packet.SdMmcCmdBlk        = &Request->SdMmcCmdBlk;
  Request->Packet.SdMmcStatusBlk     = &Request->SdMmcStatusBlk;
  Request->Packet.Timeout           = (BufferSize / (2 * 1024 * 1024) + 1) * 1000 * 1000;
  Request->Packet.TransactionStatus = EFI_NOT_READY;
  Request->SdMmcCmdBlk.CommandType   = SdMmcCommandTypeAdtc;
  Request->SdMmcCmdBlk.ResponseType  = SdMmcResponseTypeR1;

  if (IsRead) {
    Request->Packet.InDataBuffer     = Buffer;
    Request->Packet.InTransferLength  = (UINT32)BufferSize;
    Request->SdMmcCmdBlk.CommandIndex  = SingleBlock ? SD_READ_SINGLE_BLOCK : SD_READ_MULTIPLE_BLOCK;
  } else {
    Request->Packet.OutDataBuffer     = Buffer;
    Request->Packet.OutTransferLength = (UINT32)BufferSize;
    Request->SdMmcCmdBlk.CommandIndex  = SingleBlock ? SD_WRITE_SINGLE_BLOCK : SD_WRITE_MULTIPLE_BLOCK;
  }

  if (Device->SectorAddressing) {
    Request->SdMmcCmdBlk.CommandArgument = (UINT32)Lba;
  } else {
    Request->SdMmcCmdBlk.CommandArgument = (UINT32)MultU64x32 (Lba, Device->BlockMedia.BlockSize);
  }
}

static EFI_STATUS
SdSubmitWrite (
  IN SD_DEVICE   *Device,
  IN SD_REQUEST  *Request
  )
{
  EFI_STATUS                     Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;

  Status = SdCheckMediaReady (Device, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Even a transport error can leave a card programming. Recheck CMD13
  // before sending more data unless this write reaches TRAN on its own.
  Device->WriteStatusPending = TRUE;
  PassThru = Device->Private->PassThru;
  return PassThru->PassThru (PassThru, Device->Slot, &Request->Packet, Request->Event);
}

/**
  Read/write blocks through sync or async I/O request.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[in]  Lba               The starting logical block address to be read/written.
                                The caller is responsible for reading/writing to only
                                legitimate locations.
  @param[in]  Buffer            A pointer to the destination/source buffer for the data.
  @param[in]  BufferSize        Size of Buffer, must be a multiple of device block size.
  @param[in]  IsRead            Indicates it is a read or write operation.
  @param[in]  Token             A pointer to the token associated with the transaction.
  @param[in]  IsEnd             A boolean to show whether it's the last cmd in a series of cmds.
                                This parameter is only meaningful in async I/O request.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
static EFI_STATUS
SdRwBlocks (
  IN  SD_DEVICE            *Device,
  IN  EFI_LBA              Lba,
  IN  VOID                 *Buffer,
  IN  UINTN                BufferSize,
  IN  BOOLEAN              IsRead,
  IN  EFI_BLOCK_IO2_TOKEN  *Token,
  IN  BOOLEAN              IsEnd,
  IN  SD_IO_BATCH          *Batch
  )
{
  EFI_STATUS                     Status;
  SD_REQUEST                     *Request;
  BOOLEAN                         IsAsync;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;

  Request = NULL;
  IsAsync  = (BOOLEAN)((Token != NULL) && (Token->Event != NULL));
  PassThru = Device->Private->PassThru;

  Request = AllocateZeroPool (sizeof (SD_REQUEST));
  if (Request == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  Request->Signature = SD_REQUEST_SIGNATURE;
  Status = SdEnqueueRequest (Device, Request);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  SdInitRwPacket (Device, Request, Lba, Buffer, BufferSize, IsRead);

  Request->IsEnd = IsEnd;
  Request->Token = Token;

  if (IsAsync) {
    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    AsyncIoCallback,
                    Request,
                    &Request->Event
                    );
    if (EFI_ERROR (Status)) {
      goto Error;
    }
  } else {
    Request->Event = NULL;
  }

  SdAttachBatch (Request, Batch);
  if (IsRead) {
    Status = PassThru->PassThru (PassThru, Device->Slot, &Request->Packet, Request->Event);
  } else {
    ASSERT (!IsAsync);
    Status = SdSubmitWrite (Device, Request);
    if (!EFI_ERROR (Status)) {
      Status = SdConfirmWrite (Device, Request->SdMmcStatusBlk.Resp0);
    }
  }

Error:
  if (IsAsync) {
    //
    // For asynchronous operation, only free request and event in error case.
    // The request and event will be freed in asynchronous callback for success case.
    //
    if (EFI_ERROR (Status) && (Request != NULL)) {
      SdDequeueRequest (Device, Request);
      if (Request->Event != NULL) {
        gBS->CloseEvent (Request->Event);
      }

      if (Request->Batch != NULL) {
        EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
        SdReleaseBatchAtNotify (Request->Batch, SdNormalizeMediaStatus (Status));
        gBS->RestoreTPL (OldTpl);
      }
      FreePool (Request);
    }
  } else {
    //
    // For synchronous operation, free request whatever the execution result is.
    //
    if (Request != NULL) {
      SdDequeueRequest (Device, Request);
      FreePool (Request);
    }
  }

  return Status;
}

static EFI_STATUS
SdValidateReadWrite (
  IN SD_DEVICE  *Device,
  IN UINT32     MediaId,
  IN EFI_LBA    Lba,
  IN VOID       *Buffer,
  IN UINTN      BufferSize,
  IN BOOLEAN    IsRead,
  IN BOOLEAN    IsAsync
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  EFI_STATUS          Status;
  UINTN               BlockSize;
  UINTN               BlockNum;
  UINTN               IoAlign;

  Media = &Device->BlockMedia;
  Status = SdValidateMediaId (Device, MediaId);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdCheckMediaReady (Device, (BOOLEAN)!IsAsync);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!IsRead && Media->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }

  //
  // Check parameters.
  //
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  // Reject wrapping caller ranges before allocating or submitting requests.
  if (BufferSize > MAX_UINTN - (UINTN)Buffer) {
    return EFI_INVALID_PARAMETER;
  }

  BlockSize = Media->BlockSize;
  if ((BufferSize % BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  BlockNum = BufferSize / BlockSize;
  if ((Lba > Media->LastBlock) ||
      ((BlockNum - 1) > (Media->LastBlock - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  IoAlign = Media->IoAlign;
  if ((IoAlign > 0) && (((UINTN)Buffer & (IoAlign - 1)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

static EFI_STATUS
SdTransferBlocks (
  IN     SD_DEVICE            *Device,
  IN     EFI_LBA              Lba,
  IN OUT VOID                 *Buffer,
  IN     UINTN                BufferSize,
  IN     BOOLEAN              IsRead,
  IN OUT EFI_BLOCK_IO2_TOKEN  *Token
  )
{
  EFI_STATUS   Status;
  UINTN        BlockSize;
  UINTN        BlockNum;
  UINTN        Remaining;
  BOOLEAN      LastRw;
  EFI_EVENT    CallerEvent;
  SD_IO_BATCH  *Batch;
  EFI_TPL      OldTpl;

  Status      = EFI_SUCCESS;
  BlockSize   = Device->BlockMedia.BlockSize;
  LastRw      = FALSE;
  CallerEvent = (Token == NULL) ? NULL : Token->Event;
  Batch = NULL;
  if (CallerEvent != NULL) {
    Batch = AllocateZeroPool (sizeof (*Batch));
    if (Batch == NULL) { return EFI_OUT_OF_RESOURCES; }
    Batch->References = 1; // Submission owner survives immediate callbacks.
    Batch->Token = Token;
    Batch->Status = EFI_SUCCESS;
    Token->TransactionStatus = EFI_SUCCESS;
  }

  //
  // Start to execute data transfer. The max block number in single cmd is 65535 blocks.
  //
  Remaining = BufferSize / BlockSize;

  while (Remaining > 0) {
    if (Remaining <= SD_MAX_BLOCKS_PER_COMMAND) {
      BlockNum = Remaining;
      LastRw   = TRUE;
    } else {
      BlockNum = SD_MAX_BLOCKS_PER_COMMAND;
    }

    BufferSize = BlockNum * BlockSize;
    Status = SdRwBlocks (Device, Lba, Buffer, BufferSize, IsRead, Token, LastRw, Batch);

    if (EFI_ERROR (Status)) {
      SdInvalidateMediaOnStatus (Device, Status);
      Status = SdNormalizeMediaStatus (Status);
      break;
    }
    if (Batch != NULL) {
      OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
      Batch->Accepted = TRUE;
      Status = Batch->Status;
      gBS->RestoreTPL (OldTpl);
      if (EFI_ERROR (Status)) { break; }
    }

    DEBUG ((
      DEBUG_BLKIO,
      "Sd%a(): Lba 0x%x BlkNo 0x%x Event %p with %r\n",
      IsRead ? "Read" : "Write",
      Lba,
      BlockNum,
      CallerEvent,
      Status
      ));
    Lba       += BlockNum;
    Buffer     = (UINT8 *)Buffer + BufferSize;
    Remaining -= BlockNum;
  }

  if (Batch != NULL) {
    BOOLEAN Accepted;
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    Accepted = Batch->Accepted;
    SdReleaseBatchAtNotify (Batch, Status);
    gBS->RestoreTPL (OldTpl);
    // Accepted commands retain caller memory until the single batch completion.
    if (Accepted) { return EFI_SUCCESS; }
  }
  return Status;
}

/**
  The provider owns Transfer until this notification runs at TPL_NOTIFY.
  Publish its completion and latch removal here. The worker owns CMD13.
**/
static VOID
EFIAPI
SdWriteTransferComplete (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SD_ORDERED_IO  *Io;

  Io = Context;
  if (SdMediaUnavailableStatus (Io->Transfer.Packet.TransactionStatus)) {
    SdInvalidateMediaAtNotify (Io->Transfer.Device);
  }

  Io->TransferPending = FALSE;
}

static EFI_STATUS
SdAdvanceOrderedIo (
  IN  SD_DEVICE      *Device,
  IN  SD_ORDERED_IO  *Io,
  OUT BOOLEAN        *Done
  )
{
  EFI_STATUS  Status;

  *Done = TRUE;
  if (Io->WriteSize != 0) {
    Status = Io->Transfer.Packet.TransactionStatus;
    if (!EFI_ERROR (Status)) {
      Status = SdConfirmWrite (Device, Io->Transfer.SdMmcStatusBlk.Resp0);
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }

    Io->Lba            += Io->WriteSize / Device->BlockMedia.BlockSize;
    Io->Buffer          = (UINT8 *)Io->Buffer + Io->WriteSize;
    Io->BytesRemaining -= Io->WriteSize;
    Io->WriteSize       = 0;
  }

  Status = SdCheckMediaReady (Device, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Device->WriteStatusPending) {
    Status = SdConfirmWrite (Device, 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (Io->Operation == SdOrderedFlush) {
    return SdCheckMediaReady (Device, TRUE);
  }

  if (Io->BytesRemaining == 0) {
    return EFI_SUCCESS;
  }

  if (Io->Operation == SdOrderedRead) {
    return SdTransferBlocks (Device, Io->Lba, Io->Buffer, Io->BytesRemaining, TRUE, NULL);
  }

  Io->WriteSize = MIN (Io->BytesRemaining, SD_MAX_BLOCKS_PER_COMMAND * Device->BlockMedia.BlockSize);
  SdInitRwPacket (Device, &Io->Transfer, Io->Lba, Io->Buffer, Io->WriteSize, FALSE);
  Io->TransferPending = TRUE;
  Status = SdSubmitWrite (Device, &Io->Transfer);
  *Done = (BOOLEAN)EFI_ERROR (Status);
  return Status;
}

/**
  Run the head request at TPL_CALLBACK. The list keeps the child and caller
  buffers alive until both the provider and this worker have finished.
**/
static VOID
EFIAPI
SdProcessOrderedIo (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SD_DEVICE      *Device;
  SD_ORDERED_IO  *Io;
  EFI_TPL        OldTpl;
  EFI_STATUS     Status;
  BOOLEAN        Done;
  EFI_EVENT      Completion;

  Device = Context;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if ((Device->ActiveOperations != 0) || !IsListEmpty (&Device->Queue)) {
    gBS->RestoreTPL (OldTpl);
    return;
  }

  ASSERT (!IsListEmpty (&Device->OrderedIo));
  Io = BASE_CR (GetFirstNode (&Device->OrderedIo), SD_ORDERED_IO, Link);
  if (Io->TransferPending) {
    gBS->RestoreTPL (OldTpl);
    return;
  }

  gBS->RestoreTPL (OldTpl);
  Status = SdAdvanceOrderedIo (Device, Io, &Done);
  if (!Done) {
    return;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (SdMediaUnavailableStatus (Status)) {
    SdInvalidateMediaAtNotify (Device);
  }

  if (!EFI_ERROR (Status) && Device->MediaInvalidated) {
    Status = EFI_NO_MEDIA;
  }

  if (Io->Transfer.Event != NULL) {
    gBS->CloseEvent (Io->Transfer.Event);
  }

  RemoveEntryList (&Io->Link);
  if (IsListEmpty (&Device->OrderedIo)) {
    Device->OrderedIoTimer = NULL;
    gBS->CloseEvent (Event);
  }

  Completion = Io->Token->Event;
  Io->Token->TransactionStatus = SdNormalizeMediaStatus (Status);
  FreePool (Io);
  gBS->RestoreTPL (OldTpl);

  // The caller can release its token, buffer and child from this notification.
  gBS->SignalEvent (Completion);
}

static EFI_STATUS
SdQueueOrderedIo (
  IN SD_DEVICE             *Device,
  IN SD_ORDERED_OPERATION  Operation,
  IN EFI_LBA               Lba,
  IN VOID                  *Buffer,
  IN UINTN                 BufferSize,
  IN EFI_BLOCK_IO2_TOKEN   *Token
  )
{
  SD_ORDERED_IO  *Io;
  EFI_STATUS     Status;
  EFI_TPL        OldTpl;
  EFI_EVENT      Timer;

  Io = AllocateZeroPool (sizeof (*Io));
  if (Io == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Io->Operation      = Operation;
  Io->Lba            = Lba;
  Io->Buffer         = Buffer;
  Io->BytesRemaining = BufferSize;
  Io->Token          = Token;
  Io->Transfer.Device = Device;
  Status             = EFI_SUCCESS;
  if ((Operation == SdOrderedWrite) && (BufferSize != 0)) {
    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    SdWriteTransferComplete,
                    Io,
                    &Io->Transfer.Event
                    );
  }

  if (!EFI_ERROR (Status)) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
      Status = EFI_NO_MEDIA;
    } else if (Device->OrderedIoTimer == NULL) {
      Status = gBS->CreateEvent (
                      EVT_TIMER | EVT_NOTIFY_SIGNAL,
                      TPL_CALLBACK,
                      SdProcessOrderedIo,
                      Device,
                      &Timer
                      );
      if (!EFI_ERROR (Status)) {
        Status = gBS->SetTimer (Timer, TimerPeriodic, SD_ORDERED_IO_PERIOD_100NS);
        if (EFI_ERROR (Status)) {
          gBS->CloseEvent (Timer);
        } else {
          Device->OrderedIoTimer = Timer;
        }
      }
    }

    if (!EFI_ERROR (Status)) {
      InsertTailList (&Device->OrderedIo, &Io->Link);
    }

    gBS->RestoreTPL (OldTpl);
  }

  if (EFI_ERROR (Status)) {
    if (Io->Transfer.Event != NULL) {
      gBS->CloseEvent (Io->Transfer.Event);
    }

    FreePool (Io);
  }

  return Status;
}

static EFI_STATUS
SdReadWriteInternal (
  IN     SD_DEVICE            *Device,
  IN     UINT32               MediaId,
  IN     EFI_LBA              Lba,
  IN OUT VOID                 *Buffer,
  IN     UINTN                BufferSize,
  IN     BOOLEAN              IsRead,
  IN OUT EFI_BLOCK_IO2_TOKEN  *Token
  )
{
  EFI_STATUS  Status;
  BOOLEAN     IsAsync;
  BOOLEAN     Exclusive;
  BOOLEAN     IsWrite;

  IsAsync = (BOOLEAN)((Token != NULL) && (Token->Event != NULL));
  IsWrite = (BOOLEAN)(!IsRead && (BufferSize != 0));
  Status  = SdValidateReadWrite (Device, MediaId, Lba, Buffer, BufferSize, IsRead, IsAsync);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (IsAsync && IsWrite) {
    return SdQueueOrderedIo (Device, SdOrderedWrite, Lba, Buffer, BufferSize, Token);
  }

  Status = SdBeginImmediateIo (Device, IsWrite, IsAsync, &Exclusive);
  if ((Status == EFI_NOT_READY) && IsAsync) {
    return SdQueueOrderedIo (Device, SdOrderedRead, Lba, Buffer, BufferSize, Token);
  }

  if (!EFI_ERROR (Status)) {
    if (BufferSize != 0) {
      Status = SdTransferBlocks (Device, Lba, Buffer, BufferSize, IsRead, Token);
    } else if (IsAsync) {
      Token->TransactionStatus = EFI_SUCCESS;
      gBS->SignalEvent (Token->Event);
    }
  }

  SdEndImmediateIo (Device, Exclusive);
  return Status;
}

EFI_STATUS
SdReadWrite (
  IN     SD_DEVICE            *Device,
  IN     UINT32               MediaId,
  IN     EFI_LBA              Lba,
  IN OUT VOID                 *Buffer,
  IN     UINTN                BufferSize,
  IN     BOOLEAN              IsRead,
  IN OUT EFI_BLOCK_IO2_TOKEN  *Token
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   CompletionEvent;
  BOOLEAN     Synchronous;

  Status = SdBeginOperation (Device, TRUE, MediaId);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Synchronous = PcdGetBool (PcdSdDxeSynchronousPassThru);
  CompletionEvent = (Synchronous && (Token != NULL)) ? Token->Event : NULL;
  Status = SdReadWriteInternal (Device, MediaId, Lba, Buffer, BufferSize, IsRead,
                               Synchronous ? NULL : Token);
  SdEndOperation (Device);
  if ((CompletionEvent != NULL) && !EFI_ERROR (Status)) {
    Token->TransactionStatus = Status;
    // The caller may free Token and stop the device from this notification.
    gBS->SignalEvent (CompletionEvent);
  }
  return Status;
}

/**
  Reset the Block Device.

  @param  This                 Indicates a pointer to the calling context.
  @param  ExtendedVerification Driver may perform diagnostics on reset.

  @retval EFI_SUCCESS          The device was reset.
  @retval EFI_DEVICE_ERROR     The device is not functioning properly and could
                               not be reset.

**/
static EFI_STATUS
EFIAPI
SdResetInternal (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  BOOLEAN                ExtendedVerification
  )
{
  EFI_STATUS                     Status;
  SD_DEVICE                      *Device;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;

  Device = SD_DEVICE_DATA_FROM_BLKIO (This);
  PassThru = Device->Private->PassThru;
  Status = SdCheckMediaReady (Device, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdBeginReset (Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PassThru->ResetDevice (PassThru, Device->Slot);
  Status = SdEndReset (Device, Status);
  Status = SdNormalizeMediaStatus (Status);
  if (EFI_ERROR (Status) && !SdMediaUnavailableStatus (Status)) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}
EFI_STATUS
EFIAPI
SdReset (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  BOOLEAN                ExtendedVerification
  )
{
  EFI_STATUS  Status;
  SD_DEVICE   *Device;

  Device = SD_DEVICE_DATA_FROM_BLKIO (This);
  Status = SdBeginOperation (Device, FALSE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdResetInternal (This, ExtendedVerification);
  SdEndOperation (Device);
  return Status;
}

/**
  Read BufferSize bytes from Lba into Buffer.

  @param  This       Indicates a pointer to the calling context.
  @param  MediaId    Id of the media, changes every time the media is replaced.
  @param  Lba        The starting Logical Block Address to read from
  @param  BufferSize Size of Buffer, must be a multiple of device block size.
  @param  Buffer     A pointer to the destination buffer for the data. The caller is
                     responsible for either having implicit or explicit ownership of the buffer.

  @retval EFI_SUCCESS           The data was read correctly from the device.
  @retval EFI_DEVICE_ERROR      The device reported an error while performing the read.
  @retval EFI_NO_MEDIA          There is no media in the device.
  @retval EFI_MEDIA_CHANGED     The MediaId does not match the current device.
  @retval EFI_BAD_BUFFER_SIZE   The Buffer was not a multiple of the block size of the device.
  @retval EFI_INVALID_PARAMETER The read request contains LBAs that are not valid,
                                or the buffer is not on proper alignment.

**/
EFI_STATUS
EFIAPI
SdReadBlocks (
  IN     EFI_BLOCK_IO_PROTOCOL  *This,
  IN     UINT32                 MediaId,
  IN     EFI_LBA                Lba,
  IN     UINTN                  BufferSize,
  OUT VOID                      *Buffer
  )
{
  EFI_STATUS  Status;
  SD_DEVICE   *Device;

  Device = SD_DEVICE_DATA_FROM_BLKIO (This);

  Status = SdReadWrite (Device, MediaId, Lba, Buffer, BufferSize, TRUE, NULL);
  return Status;
}

/**
  Write BufferSize bytes from Lba into Buffer.

  @param  This       Indicates a pointer to the calling context.
  @param  MediaId    The media ID that the write request is for.
  @param  Lba        The starting logical block address to be written. The caller is
                     responsible for writing to only legitimate locations.
  @param  BufferSize Size of Buffer, must be a multiple of device block size.
  @param  Buffer     A pointer to the source buffer for the data.

  @retval EFI_SUCCESS           The data was written correctly to the device.
  @retval EFI_WRITE_PROTECTED   The device can not be written to.
  @retval EFI_DEVICE_ERROR      The device reported an error while performing the write.
  @retval EFI_NO_MEDIA          There is no media in the device.
  @retval EFI_MEDIA_CHANGED     The MediaId does not match the current device.
  @retval EFI_BAD_BUFFER_SIZE   The Buffer was not a multiple of the block size of the device.
  @retval EFI_INVALID_PARAMETER The write request contains LBAs that are not valid,
                                or the buffer is not on proper alignment.

**/
EFI_STATUS
EFIAPI
SdWriteBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  IN  VOID                   *Buffer
  )
{
  EFI_STATUS  Status;
  SD_DEVICE   *Device;

  Device = SD_DEVICE_DATA_FROM_BLKIO (This);

  Status = SdReadWrite (Device, MediaId, Lba, Buffer, BufferSize, FALSE, NULL);
  return Status;
}

static EFI_STATUS
SdFlush (
  IN     SD_DEVICE            *Device,
  IN OUT EFI_BLOCK_IO2_TOKEN  *Token
  )
{
  EFI_STATUS  Status;
  BOOLEAN     IsAsync;
  BOOLEAN     Exclusive;

  IsAsync = (BOOLEAN)((Token != NULL) && (Token->Event != NULL));
  Status  = SdBeginOperation (Device, FALSE, 0);
  if (!EFI_ERROR (Status)) {
    Status = SdBeginImmediateIo (Device, FALSE, IsAsync, &Exclusive);
    if ((Status == EFI_NOT_READY) && IsAsync) {
      Status = SdQueueOrderedIo (Device, SdOrderedFlush, 0, NULL, 0, Token);
      SdEndOperation (Device);
      return Status;
    }

    if (!EFI_ERROR (Status)) {
      Status = SdCheckMediaReady (Device, (BOOLEAN)!IsAsync);
    }

    SdEndImmediateIo (Device, Exclusive);
    SdEndOperation (Device);
  }

  if (Token != NULL) {
    Token->TransactionStatus = Status;
    if (IsAsync) {
      gBS->SignalEvent (Token->Event);
    }
  }

  return Status;
}

/**
  Flush the Block Device.

  @param  This              Indicates a pointer to the calling context.

  @retval EFI_SUCCESS       All outstanding data was written to the device
  @retval EFI_DEVICE_ERROR  The device reported an error while writing back the data
  @retval EFI_NO_MEDIA      There is no media in the device.

**/
EFI_STATUS
EFIAPI
SdFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return SdFlush (SD_DEVICE_DATA_FROM_BLKIO (This), NULL);
}

/**
  Reset the Block Device.

  @param[in]  This                 Indicates a pointer to the calling context.
  @param[in]  ExtendedVerification Driver may perform diagnostics on reset.

  @retval EFI_SUCCESS              The device was reset.
  @retval EFI_DEVICE_ERROR         The device is not functioning properly and could
                                   not be reset.

**/
static EFI_STATUS
EFIAPI
SdResetExInternal (
  IN  EFI_BLOCK_IO2_PROTOCOL  *This,
  IN  BOOLEAN                 ExtendedVerification
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;
  SD_DEVICE   *Device;

  Device = SD_DEVICE_DATA_FROM_BLKIO2 (This);
  Status = SdCheckMediaReady (Device, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Device->MediaInvalidated || !Device->BlockMedia.MediaPresent) {
    Status = EFI_NO_MEDIA;
  } else if (Device->ResetInProgress || Device->WriteInProgress ||
             !IsListEmpty (&Device->Queue) || !IsListEmpty (&Device->OrderedIo)) {
    Status = EFI_NOT_READY;
  } else {
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}
EFI_STATUS
EFIAPI
SdResetEx (
  IN  EFI_BLOCK_IO2_PROTOCOL  *This,
  IN  BOOLEAN                 ExtendedVerification
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;
  SD_DEVICE   *Device;

  Device = SD_DEVICE_DATA_FROM_BLKIO2 (This);

  // DiskIoDxe resets BlockIo2 while disconnecting this child. Stop has already
  // invalidated the media and drained its I/O, so no card access is needed.
  // Returning NO_MEDIA here would prevent DiskIo/FAT from releasing the child.
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Device->StopInProgress) {
    Status = ((Device->ActiveOperations == 0) && !Device->ResetInProgress &&
              IsListEmpty (&Device->Queue) && IsListEmpty (&Device->OrderedIo)) ?
             EFI_SUCCESS : EFI_NOT_READY;
    gBS->RestoreTPL (OldTpl);
    return Status;
  }

  gBS->RestoreTPL (OldTpl);
  Status = SdBeginOperation (Device, FALSE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdResetExInternal (This, ExtendedVerification);
  SdEndOperation (Device);
  return Status;
}

/**
  Read BufferSize bytes from Lba into Buffer.

  @param[in]       This         Indicates a pointer to the calling context.
  @param[in]       MediaId      Id of the media, changes every time the media is replaced.
  @param[in]       Lba          The starting Logical Block Address to read from.
  @param[in, out]  Token        A pointer to the token associated with the transaction.
  @param[in]       BufferSize   Size of Buffer, must be a multiple of device block size.
  @param[out]      Buffer       A pointer to the destination buffer for the data. The caller is
                                responsible for either having implicit or explicit ownership of the buffer.

  @retval EFI_SUCCESS           The read request was queued if Event is not NULL.
                                The data was read correctly from the device if
                                the Event is NULL.
  @retval EFI_DEVICE_ERROR      The device reported an error while performing
                                the read.
  @retval EFI_NO_MEDIA          There is no media in the device.
  @retval EFI_MEDIA_CHANGED     The MediaId is not for the current media.
  @retval EFI_BAD_BUFFER_SIZE   The BufferSize parameter is not a multiple of the
                                intrinsic block size of the device.
  @retval EFI_INVALID_PARAMETER The read request contains LBAs that are not valid,
                                or the buffer is not on proper alignment.
  @retval EFI_OUT_OF_RESOURCES  The request could not be completed due to a lack
                                of resources.

**/
EFI_STATUS
EFIAPI
SdReadBlocksEx (
  IN     EFI_BLOCK_IO2_PROTOCOL  *This,
  IN     UINT32                  MediaId,
  IN     EFI_LBA                 Lba,
  IN OUT EFI_BLOCK_IO2_TOKEN     *Token,
  IN     UINTN                   BufferSize,
  OUT VOID                       *Buffer
  )
{
  EFI_STATUS  Status;
  SD_DEVICE   *Device;

  Device = SD_DEVICE_DATA_FROM_BLKIO2 (This);

  Status = SdReadWrite (Device, MediaId, Lba, Buffer, BufferSize, TRUE, Token);
  return Status;
}

/**
  Write BufferSize bytes from Lba into Buffer.

  @param[in]       This         Indicates a pointer to the calling context.
  @param[in]       MediaId      The media ID that the write request is for.
  @param[in]       Lba          The starting logical block address to be written. The
                                caller is responsible for writing to only legitimate
                                locations.
  @param[in, out]  Token        A pointer to the token associated with the transaction.
  @param[in]       BufferSize   Size of Buffer, must be a multiple of device block size.
  @param[in]       Buffer       A pointer to the source buffer for the data.

  @retval EFI_SUCCESS           The data was written correctly to the device.
  @retval EFI_WRITE_PROTECTED   The device can not be written to.
  @retval EFI_DEVICE_ERROR      The device reported an error while performing the write.
  @retval EFI_NO_MEDIA          There is no media in the device.
  @retval EFI_MEDIA_CHANGED     The MediaId does not match the current device.
  @retval EFI_BAD_BUFFER_SIZE   The Buffer was not a multiple of the block size of the device.
  @retval EFI_INVALID_PARAMETER The write request contains LBAs that are not valid,
                                or the buffer is not on proper alignment.

**/
EFI_STATUS
EFIAPI
SdWriteBlocksEx (
  IN     EFI_BLOCK_IO2_PROTOCOL  *This,
  IN     UINT32                  MediaId,
  IN     EFI_LBA                 Lba,
  IN OUT EFI_BLOCK_IO2_TOKEN     *Token,
  IN     UINTN                   BufferSize,
  IN     VOID                    *Buffer
  )
{
  EFI_STATUS  Status;
  SD_DEVICE   *Device;

  Device = SD_DEVICE_DATA_FROM_BLKIO2 (This);

  Status = SdReadWrite (Device, MediaId, Lba, Buffer, BufferSize, FALSE, Token);
  return Status;
}

/**
  Flush the Block Device.

  @param[in]       This     Indicates a pointer to the calling context.
  @param[in, out]  Token    A pointer to the token associated with the transaction.

  @retval EFI_SUCCESS       All outstanding data was written to the device
  @retval EFI_DEVICE_ERROR  The device reported an error while writing back the data
  @retval EFI_NO_MEDIA      There is no media in the device.

**/
EFI_STATUS
EFIAPI
SdFlushBlocksEx (
  IN     EFI_BLOCK_IO2_PROTOCOL  *This,
  IN OUT EFI_BLOCK_IO2_TOKEN     *Token
  )
{
  return SdFlush (SD_DEVICE_DATA_FROM_BLKIO2 (This), Token);
}

/**
  Set the erase start address through sync or async I/O request.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[in]  StartLba          The starting logical block address to be erased.
  @param[in]  Token             A pointer to the token associated with the transaction.
  @param[in]  IsEnd             A boolean to show whether it's the last cmd in a series of cmds.
                                This parameter is only meaningful in async I/O request.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
EFI_STATUS
SdEraseBlockStart (
  IN  SD_DEVICE            *Device,
  IN  EFI_LBA              StartLba,
  IN  EFI_BLOCK_IO2_TOKEN  *Token,
  IN  BOOLEAN              IsEnd
  )
{
  EFI_STATUS                     Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;
  SD_REQUEST                     *EraseBlockStart;
  BOOLEAN                         IsAsync;

  EraseBlockStart = NULL;
  IsAsync = (BOOLEAN)((Token != NULL) && (Token->Event != NULL));
  PassThru        = Device->Private->PassThru;

  EraseBlockStart = AllocateZeroPool (sizeof (SD_REQUEST));
  if (EraseBlockStart == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  EraseBlockStart->Signature = SD_REQUEST_SIGNATURE;
  Status = SdEnqueueRequest (Device, EraseBlockStart);
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  EraseBlockStart->Packet.SdMmcCmdBlk    = &EraseBlockStart->SdMmcCmdBlk;
  EraseBlockStart->Packet.SdMmcStatusBlk = &EraseBlockStart->SdMmcStatusBlk;
  EraseBlockStart->Packet.Timeout        = SD_GENERIC_TIMEOUT;

  EraseBlockStart->SdMmcCmdBlk.CommandIndex = SD_ERASE_WR_BLK_START;
  EraseBlockStart->SdMmcCmdBlk.CommandType  = SdMmcCommandTypeAc;
  EraseBlockStart->SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR1;

  if (Device->SectorAddressing) {
    EraseBlockStart->SdMmcCmdBlk.CommandArgument = (UINT32)StartLba;
  } else {
    EraseBlockStart->SdMmcCmdBlk.CommandArgument = (UINT32)MultU64x32 (StartLba, Device->BlockMedia.BlockSize);
  }

  EraseBlockStart->IsEnd = IsEnd;
  EraseBlockStart->Token = Token;

  if (IsAsync) {
    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    AsyncIoCallback,
                    EraseBlockStart,
                    &EraseBlockStart->Event
                    );
    if (EFI_ERROR (Status)) {
      goto Error;
    }
  } else {
    EraseBlockStart->Event = NULL;
  }

  Status = PassThru->PassThru (PassThru, Device->Slot, &EraseBlockStart->Packet, EraseBlockStart->Event);

Error:
  if (IsAsync) {
    //
    // For asynchronous operation, only free request and event in error case.
    // The request and event will be freed in asynchronous callback for success case.
    //
    if (EFI_ERROR (Status) && (EraseBlockStart != NULL)) {
      SdDequeueRequest (Device, EraseBlockStart);
      if (EraseBlockStart->Event != NULL) {
        gBS->CloseEvent (EraseBlockStart->Event);
      }

      FreePool (EraseBlockStart);
    }
  } else {
    //
    // For synchronous operation, free request whatever the execution result is.
    //
    if (EraseBlockStart != NULL) {
      SdDequeueRequest (Device, EraseBlockStart);
      FreePool (EraseBlockStart);
    }
  }

  return Status;
}

/**
  Set the erase end address through sync or async I/O request.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[in]  EndLba            The ending logical block address to be erased.
  @param[in]  Token             A pointer to the token associated with the transaction.
  @param[in]  IsEnd             A boolean to show whether it's the last cmd in a series of cmds.
                                This parameter is only meaningful in async I/O request.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
EFI_STATUS
SdEraseBlockEnd (
  IN  SD_DEVICE            *Device,
  IN  EFI_LBA              EndLba,
  IN  EFI_BLOCK_IO2_TOKEN  *Token,
  IN  BOOLEAN              IsEnd
  )
{
  EFI_STATUS                     Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;
  SD_REQUEST                     *EraseBlockEnd;
  BOOLEAN                         IsAsync;

  EraseBlockEnd = NULL;
  IsAsync = (BOOLEAN)((Token != NULL) && (Token->Event != NULL));
  PassThru      = Device->Private->PassThru;

  EraseBlockEnd = AllocateZeroPool (sizeof (SD_REQUEST));
  if (EraseBlockEnd == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  EraseBlockEnd->Signature = SD_REQUEST_SIGNATURE;
  Status = SdEnqueueRequest (Device, EraseBlockEnd);
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  EraseBlockEnd->Packet.SdMmcCmdBlk    = &EraseBlockEnd->SdMmcCmdBlk;
  EraseBlockEnd->Packet.SdMmcStatusBlk = &EraseBlockEnd->SdMmcStatusBlk;
  EraseBlockEnd->Packet.Timeout        = SD_GENERIC_TIMEOUT;

  EraseBlockEnd->SdMmcCmdBlk.CommandIndex = SD_ERASE_WR_BLK_END;
  EraseBlockEnd->SdMmcCmdBlk.CommandType  = SdMmcCommandTypeAc;
  EraseBlockEnd->SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR1;

  if (Device->SectorAddressing) {
    EraseBlockEnd->SdMmcCmdBlk.CommandArgument = (UINT32)EndLba;
  } else {
    EraseBlockEnd->SdMmcCmdBlk.CommandArgument = (UINT32)MultU64x32 (EndLba, Device->BlockMedia.BlockSize);
  }

  EraseBlockEnd->IsEnd = IsEnd;
  EraseBlockEnd->Token = Token;

  if (IsAsync) {
    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    AsyncIoCallback,
                    EraseBlockEnd,
                    &EraseBlockEnd->Event
                    );
    if (EFI_ERROR (Status)) {
      goto Error;
    }
  } else {
    EraseBlockEnd->Event = NULL;
  }

  Status = PassThru->PassThru (PassThru, Device->Slot, &EraseBlockEnd->Packet, EraseBlockEnd->Event);

Error:
  if (IsAsync) {
    //
    // For asynchronous operation, only free request and event in error case.
    // The request and event will be freed in asynchronous callback for success case.
    //
    if (EFI_ERROR (Status) && (EraseBlockEnd != NULL)) {
      SdDequeueRequest (Device, EraseBlockEnd);
      if (EraseBlockEnd->Event != NULL) {
        gBS->CloseEvent (EraseBlockEnd->Event);
      }

      FreePool (EraseBlockEnd);
    }
  } else {
    //
    // For synchronous operation, free request whatever the execution result is.
    //
    if (EraseBlockEnd != NULL) {
      SdDequeueRequest (Device, EraseBlockEnd);
      FreePool (EraseBlockEnd);
    }
  }

  return Status;
}

/**
  Erase specified blocks through sync or async I/O request.

  @param[in]  Device            A pointer to the SD_DEVICE instance.
  @param[in]  Token             A pointer to the token associated with the transaction.
  @param[in]  IsEnd             A boolean to show whether it's the last cmd in a series of cmds.
                                This parameter is only meaningful in async I/O request.

  @retval EFI_SUCCESS           The request is executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The request could not be executed due to a lack of resources.
  @retval Others                The request could not be executed successfully.

**/
EFI_STATUS
SdEraseBlock (
  IN  SD_DEVICE            *Device,
  IN  EFI_BLOCK_IO2_TOKEN  *Token,
  IN  BOOLEAN              IsEnd
  )
{
  EFI_STATUS                     Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;
  SD_REQUEST                     *EraseBlock;
  BOOLEAN                         IsAsync;

  EraseBlock = NULL;
  IsAsync = (BOOLEAN)((Token != NULL) && (Token->Event != NULL));
  PassThru   = Device->Private->PassThru;

  EraseBlock = AllocateZeroPool (sizeof (SD_REQUEST));
  if (EraseBlock == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  EraseBlock->Signature = SD_REQUEST_SIGNATURE;
  Status = SdEnqueueRequest (Device, EraseBlock);
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  EraseBlock->Packet.SdMmcCmdBlk    = &EraseBlock->SdMmcCmdBlk;
  EraseBlock->Packet.SdMmcStatusBlk = &EraseBlock->SdMmcStatusBlk;
  EraseBlock->Packet.Timeout        = SD_GENERIC_TIMEOUT;

  EraseBlock->SdMmcCmdBlk.CommandIndex = SD_ERASE;
  EraseBlock->SdMmcCmdBlk.CommandType  = SdMmcCommandTypeAc;
  EraseBlock->SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR1b;

  EraseBlock->IsEnd = IsEnd;
  EraseBlock->Token = Token;

  if (IsAsync) {
    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    AsyncIoCallback,
                    EraseBlock,
                    &EraseBlock->Event
                    );
    if (EFI_ERROR (Status)) {
      goto Error;
    }
  } else {
    EraseBlock->Event = NULL;
  }

  Status = PassThru->PassThru (PassThru, Device->Slot, &EraseBlock->Packet, EraseBlock->Event);

Error:
  if (IsAsync) {
    //
    // For asynchronous operation, only free request and event in error case.
    // The request and event will be freed in asynchronous callback for success case.
    //
    if (EFI_ERROR (Status) && (EraseBlock != NULL)) {
      SdDequeueRequest (Device, EraseBlock);
      if (EraseBlock->Event != NULL) {
        gBS->CloseEvent (EraseBlock->Event);
      }

      FreePool (EraseBlock);
    }
  } else {
    //
    // For synchronous operation, free request whatever the execution result is.
    //
    if (EraseBlock != NULL) {
      SdDequeueRequest (Device, EraseBlock);
      FreePool (EraseBlock);
    }
  }

  return Status;
}

/**
  Erase a specified number of device blocks.

  @param[in]       This           Indicates a pointer to the calling context.
  @param[in]       MediaId        The media ID that the erase request is for.
  @param[in]       Lba            The starting logical block address to be
                                  erased. The caller is responsible for erasing
                                  only legitimate locations.
  @param[in, out]  Token          A pointer to the token associated with the
                                  transaction.
  @param[in]       Size           The size in bytes to be erased. This must be
                                  a multiple of the physical block size of the
                                  device.

  @retval EFI_SUCCESS             The erase request was queued if Event is not
                                  NULL. The data was erased correctly to the
                                  device if the Event is NULL.to the device.
  @retval EFI_WRITE_PROTECTED     The device cannot be erased due to write
                                  protection.
  @retval EFI_DEVICE_ERROR        The device reported an error while attempting
                                  to perform the erase operation.
  @retval EFI_INVALID_PARAMETER   The erase request contains LBAs that are not
                                  valid.
  @retval EFI_NO_MEDIA            There is no media in the device.
  @retval EFI_MEDIA_CHANGED       The MediaId is not for the current media.

**/
static EFI_STATUS
EFIAPI
SdEraseBlocksInternal (
  IN     EFI_ERASE_BLOCK_PROTOCOL  *This,
  IN     UINT32                    MediaId,
  IN     EFI_LBA                   Lba,
  IN OUT EFI_ERASE_BLOCK_TOKEN     *Token,
  IN     UINTN                     Size
  )
{
  EFI_STATUS          Status;
  EFI_BLOCK_IO_MEDIA  *Media;
  UINTN               BlockSize;
  UINTN               BlockNum;
  EFI_LBA             LastLba;
  SD_DEVICE           *Device;
  EFI_EVENT           CallerEvent;
  BOOLEAN             IsAsync;

  Status = EFI_SUCCESS;
  Device = SD_DEVICE_DATA_FROM_ERASEBLK (This);
  CallerEvent = (Token == NULL) ? NULL : Token->Event;
  IsAsync     = (BOOLEAN)(CallerEvent != NULL);
  Media  = &Device->BlockMedia;

  Status = SdValidateMediaId (Device, MediaId);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdCheckMediaReady (Device, (BOOLEAN)!IsAsync);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Media->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }

  //
  // Check parameters.
  //
  BlockSize = Media->BlockSize;
  if ((Size % BlockSize) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  BlockNum = Size / BlockSize;
  if ((Lba > Media->LastBlock) ||
      ((BlockNum - 1) > (Media->LastBlock - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  if (IsAsync) {
    Token->TransactionStatus = EFI_SUCCESS;
  }

  LastLba = Lba + BlockNum - 1;

  Status = SdEraseBlockStart (Device, Lba, (EFI_BLOCK_IO2_TOKEN *)Token, FALSE);
  if (EFI_ERROR (Status)) {
    SdInvalidateMediaOnStatus (Device, Status);
    return SdNormalizeMediaStatus (Status);
  }

  Status = SdEraseBlockEnd (Device, LastLba, (EFI_BLOCK_IO2_TOKEN *)Token, FALSE);
  if (EFI_ERROR (Status)) {
    SdInvalidateMediaOnStatus (Device, Status);
    return SdNormalizeMediaStatus (Status);
  }

  Status = SdEraseBlock (Device, (EFI_BLOCK_IO2_TOKEN *)Token, TRUE);
  if (EFI_ERROR (Status)) {
    SdInvalidateMediaOnStatus (Device, Status);
    return SdNormalizeMediaStatus (Status);
  }

  DEBUG ((
    DEBUG_INFO,
    "SdEraseBlocks(): Lba 0x%x BlkNo 0x%x Event %p with %r\n",
    Lba,
    BlockNum,
    CallerEvent,
    Status
    ));

  return Status;
}

EFI_STATUS
EFIAPI
SdEraseBlocks (
  IN     EFI_ERASE_BLOCK_PROTOCOL  *This,
  IN     UINT32                    MediaId,
  IN     EFI_LBA                   Lba,
  IN OUT EFI_ERASE_BLOCK_TOKEN     *Token,
  IN     UINTN                     Size
  )
{
  EFI_STATUS  Status;
  SD_DEVICE   *Device;
  EFI_EVENT   CompletionEvent;
  BOOLEAN     Synchronous;
  BOOLEAN     Exclusive;

  Device = SD_DEVICE_DATA_FROM_ERASEBLK (This);
  Status = SdBeginOperation (Device, TRUE, MediaId);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Synchronous = PcdGetBool (PcdSdDxeSynchronousPassThru);
  CompletionEvent = (Synchronous && (Token != NULL)) ? Token->Event : NULL;
  Status = SdBeginImmediateIo (Device, FALSE, (BOOLEAN)(!Synchronous && (Token != NULL) &&
                                                       (Token->Event != NULL)), &Exclusive);
  if (!EFI_ERROR (Status)) {
    Status = SdEraseBlocksInternal (This, MediaId, Lba, Synchronous ? NULL : Token, Size);
  }

  SdEndImmediateIo (Device, Exclusive);
  SdEndOperation (Device);
  if ((CompletionEvent != NULL) && !EFI_ERROR (Status)) {
    Token->TransactionStatus = Status;
    gBS->SignalEvent (CompletionEvent);
  }
  return Status;
}
