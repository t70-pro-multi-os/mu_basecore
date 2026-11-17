/** @file
  UfsPassThruDxe driver is used to produce EFI_EXT_SCSI_PASS_THRU protocol interface
  for upper layer application to execute UFS-supported SCSI cmds.

  Copyright (c) 2014 - 2022, Intel Corporation. All rights reserved.<BR>
  Copyright (c) Microsoft Corporation.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UfsPassThru.h"

STATIC
VOID
UfsQueueTransferRequest (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN UFS_PASS_THRU_TRANS_REQ     *TransReq
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (IsListEmpty (&TransReq->TransferList)) {
    InsertTailList (&Private->Queue, &TransReq->TransferList);
  }

  gBS->RestoreTPL (OldTpl);
}

STATIC
VOID
UfsUnqueueTransferRequest (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN UFS_PASS_THRU_TRANS_REQ     *TransReq
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!IsListEmpty (&TransReq->TransferList)) {
    if (TransReq->Slot < 32) {
      Private->ReservedTrlSlots &= ~(((UINT32)1U << TransReq->Slot));
    }

    RemoveEntryList (&TransReq->TransferList);
    InitializeListHead (&TransReq->TransferList);
  }

  gBS->RestoreTPL (OldTpl);
}

STATIC
EFI_STATUS
UfsReleaseTransferRequestResources (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN UFS_PASS_THRU_TRANS_REQ     *TransReq,
  IN BOOLEAN                    CopyReadData
  )
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;
  EFI_STATUS                          Status;

  UfsHc = Private->UfsHostController;

  if (TransReq->DataBufMapping != NULL) {
    Status = UfsHc->Unmap (UfsHc, TransReq->DataBufMapping);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    TransReq->DataBufMapping = NULL;
  }

  if (CopyReadData && (TransReq->AlignedDataBuf != NULL) && (TransReq->Packet != NULL) &&
      (TransReq->Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ))
  {
    if ((TransReq->Packet->InTransferLength > TransReq->AlignedDataBufSize) ||
        ((TransReq->Packet->InTransferLength != 0) && (TransReq->Packet->InDataBuffer == NULL)))
    {
      return EFI_DEVICE_ERROR;
    }

    if (TransReq->Packet->InTransferLength != 0) {
      CopyMem (TransReq->Packet->InDataBuffer, TransReq->AlignedDataBuf, TransReq->Packet->InTransferLength);
    }
  }

  if (TransReq->AlignedDataBuf != NULL) {
    ZeroMem (TransReq->AlignedDataBuf, TransReq->AlignedDataBufSize);
    FreeAlignedPages (TransReq->AlignedDataBuf, EFI_SIZE_TO_PAGES (TransReq->AlignedDataBufSize));
    TransReq->AlignedDataBuf     = NULL;
    TransReq->AlignedDataBufSize = 0;
  }

  if (TransReq->CmdDescMapping != NULL) {
    Status = UfsHc->Unmap (UfsHc, TransReq->CmdDescMapping);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    TransReq->CmdDescMapping = NULL;
  }

  if (TransReq->CmdDescHost != NULL) {
    Status = UfsHc->FreeBuffer (
                       UfsHc,
                       EFI_SIZE_TO_PAGES (TransReq->CmdDescSize),
                       TransReq->CmdDescHost
                       );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    TransReq->CmdDescHost = NULL;
  }

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
UfsTransferRequestOwnsResources (
  IN UFS_PASS_THRU_TRANS_REQ  *TransReq
  )
{
  return (BOOLEAN)(
                    (TransReq->DataBufMapping != NULL) ||
                    (TransReq->AlignedDataBuf != NULL) ||
                    (TransReq->CmdDescMapping != NULL) ||
                    (TransReq->CmdDescHost != NULL) ||
                    (TransReq->Packet != NULL) ||
                    (TransReq->CallerEvent != NULL)
                    );
}

STATIC
VOID
UfsRetainOrFreeTransferRequest (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN UFS_PASS_THRU_TRANS_REQ     *TransReq
  )
{
  if (UfsTransferRequestOwnsResources (TransReq)) {
    UfsQueueTransferRequest (Private, TransReq);
  } else {
    UfsUnqueueTransferRequest (Private, TransReq);
    FreePool (TransReq);
  }
}

/**
  Read 32bits data from specified UFS MMIO register.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Offset        The offset within the UFS Host Controller MMIO space to start
                            the memory operation.
  @param[out] Value         The data buffer to store.

  @retval EFI_TIMEOUT       The operation is time out.
  @retval EFI_SUCCESS       The operation succeeds.
  @retval Others            The operation fails.

**/
EFI_STATUS
UfsMmioRead32 (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     UINTN                       Offset,
  OUT UINT32                         *Value
  )
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;
  EFI_STATUS                          Status;

  UfsHc = Private->UfsHostController;

  Status = UfsHc->Read (UfsHc, EfiUfsHcWidthUint32, Offset, 1, Value);

  return Status;
}

/**
  Write 32bits data to specified UFS MMIO register.

  @param[in] Private        The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in] Offset         The offset within the UFS Host Controller MMIO space to start
                            the memory operation.
  @param[in] Value          The data to write.

  @retval EFI_TIMEOUT       The operation is time out.
  @retval EFI_SUCCESS       The operation succeeds.
  @retval Others            The operation fails.

**/
EFI_STATUS
UfsMmioWrite32 (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINTN                       Offset,
  IN  UINT32                      Value
  )
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;
  EFI_STATUS                          Status;

  UfsHc = Private->UfsHostController;

  Status = UfsHc->Write (UfsHc, EfiUfsHcWidthUint32, Offset, 1, &Value);

  return Status;
}

/**
  Wait for the value of the specified system memory set to the test value.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Offset        The offset within the UFS Host Controller MMIO space to start
                            the memory operation.
  @param[in]  MaskValue     The mask value of memory.
  @param[in]  TestValue     The test value of memory.
  @param[in]  Timeout       The time out value for wait memory set, uses 100ns as a unit.

  @retval EFI_TIMEOUT       The system memory setting is time out.
  @retval EFI_SUCCESS       The system memory is correct set.
  @retval Others            The operation fails.

**/
EFI_STATUS
UfsWaitMemSet (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINTN                       Offset,
  IN  UINT32                      MaskValue,
  IN  UINT32                      TestValue,
  IN  UINT64                      Timeout
  )
{
  UINT32      Value;
  UINT64      Delay;
  BOOLEAN     InfiniteWait;
  EFI_STATUS  Status;

  InfiniteWait = (BOOLEAN)(Timeout == 0);
  Delay        = DivU64x32 (Timeout, 10) + 1;

  do {
    Status = UfsMmioRead32 (Private, Offset, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Value == MAX_UINT32) {
      return EFI_DEVICE_ERROR;
    }

    Value &= MaskValue;
    if (Value == TestValue) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
    Delay--;
  } while (InfiniteWait || (Delay > 0));

  return EFI_TIMEOUT;
}

/**
  Dump UIC command execution result for debugging.

  @param[in]   UicOpcode  The executed UIC opcode.
  @param[in]   Result     The result to be parsed.

**/
VOID
DumpUicCmdExecResult (
  IN  UINT8  UicOpcode,
  IN  UINT8  Result
  )
{
  if (UicOpcode <= UfsUicDmePeerSet) {
    switch (Result) {
      case 0x00:
        break;
      case 0x01:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - INVALID_MIB_ATTRIBUTE\n"));
        break;
      case 0x02:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - INVALID_MIB_ATTRIBUTE_VALUE\n"));
        break;
      case 0x03:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - READ_ONLY_MIB_ATTRIBUTE\n"));
        break;
      case 0x04:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - WRITE_ONLY_MIB_ATTRIBUTE\n"));
        break;
      case 0x05:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - BAD_INDEX\n"));
        break;
      case 0x06:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - LOCKED_MIB_ATTRIBUTE\n"));
        break;
      case 0x07:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - BAD_TEST_FEATURE_INDEX\n"));
        break;
      case 0x08:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - PEER_COMMUNICATION_FAILURE\n"));
        break;
      case 0x09:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - BUSY\n"));
        break;
      case 0x0A:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - DME_FAILURE\n"));
        break;
      default:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - UNKNOWN_ERROR (0x%02x)\n", Result));
        break;
    }
  } else {
    switch (Result) {
      case 0x00:
        break;
      case 0x01:
        DEBUG ((DEBUG_VERBOSE, "UIC control command fails - FAILURE\n"));
        break;
      default:
        DEBUG ((DEBUG_VERBOSE, "UIC control command fails - UNKNOWN_ERROR (0x%02x)\n", Result));
        break;
    }
  }
}

/**
  Dump QUERY RESPONSE UPIU result for debugging.

  @param[in]   Result  The result to be parsed.

**/
VOID
DumpQueryResponseResult (
  IN  UINT8  Result
  )
{
  switch (Result) {
    case 0xF6:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Parameter Not Readable\n"));
      break;
    case 0xF7:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Parameter Not Writeable\n"));
      break;
    case 0xF8:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Parameter Already Written\n"));
      break;
    case 0xF9:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Length\n"));
      break;
    case 0xFA:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Value\n"));
      break;
    case 0xFB:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Selector\n"));
      break;
    case 0xFC:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Index\n"));
      break;
    case 0xFD:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Idn\n"));
      break;
    case 0xFE:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Opcode\n"));
      break;
    case 0xFF:
      DEBUG ((DEBUG_VERBOSE, "Query Response with General Failure\n"));
      break;
    default:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Unknown Error (0x%02x)\n", Result));
      break;
  }
}

/**
  Swap little endian to big endian.

  @param[in, out] Buffer      The data buffer. In input, it contains little endian data.
                              In output, it will become big endian.
  @param[in]      BufferSize  The length of converted data.

**/
VOID
SwapLittleEndianToBigEndian (
  IN OUT UINT8   *Buffer,
  IN     UINT32  BufferSize
  )
{
  UINT32  Index;
  UINT8   Temp;
  UINT32  SwapCount;

  SwapCount = BufferSize / 2;
  for (Index = 0; Index < SwapCount; Index++) {
    Temp                           = Buffer[Index];
    Buffer[Index]                  = Buffer[BufferSize - 1 - Index];
    Buffer[BufferSize - 1 - Index] = Temp;
  }
}

/**
  Fill TSF field of QUERY REQUEST UPIU.

  @param[in, out] TsfBase      The base address of TSF field of QUERY REQUEST UPIU.
  @param[in]      Opcode       The opcode of request.
  @param[in]      DescId       The descriptor ID of request.
  @param[in]      Index        The index of request.
  @param[in]      Selector     The selector of request.
  @param[in]      Length       The length of transferred data. The maximum is 4.
  @param[in]      Value        The value of transferred data.

**/
VOID
UfsFillTsfOfQueryReqUpiu (
  IN OUT UTP_UPIU_TSF  *TsfBase,
  IN     UINT8         Opcode,
  IN     UINT8         DescId    OPTIONAL,
  IN     UINT8         Index     OPTIONAL,
  IN     UINT8         Selector  OPTIONAL,
  IN     UINT16        Length    OPTIONAL,
  IN     UINT32        Value     OPTIONAL
  )
{
  ASSERT (TsfBase != NULL);
  ASSERT (Opcode <= UtpQueryFuncOpcodeTogFlag);

  TsfBase->Opcode = Opcode;
  if (Opcode != UtpQueryFuncOpcodeNop) {
    TsfBase->DescId   = DescId;
    TsfBase->Index    = Index;
    TsfBase->Selector = Selector;

    if ((Opcode == UtpQueryFuncOpcodeRdDesc) || (Opcode == UtpQueryFuncOpcodeWrDesc)) {
      SwapLittleEndianToBigEndian ((UINT8 *)&Length, sizeof (Length));
      TsfBase->Length = Length;
    }

    if (Opcode == UtpQueryFuncOpcodeWrAttr) {
      SwapLittleEndianToBigEndian ((UINT8 *)&Value, sizeof (Value));
      TsfBase->Value = Value;
    }
  }
}

/**
  Initialize COMMAND UPIU.

  @param[in, out] Command         The base address of COMMAND UPIU.
  @param[in]      Lun             The Lun on which the SCSI command is executed.
  @param[in]      TaskTag         The task tag of request.
  @param[in]      Cdb             The cdb buffer containing SCSI command.
  @param[in]      CdbLength       The cdb length.
  @param[in]      DataDirection   The direction of data transfer.
  @param[in]      ExpDataTranLen  The expected transfer data length.

  @retval EFI_SUCCESS     The initialization succeed.

**/
EFI_STATUS
UfsInitCommandUpiu (
  IN OUT UTP_COMMAND_UPIU    *Command,
  IN     UINT8               Lun,
  IN     UINT8               TaskTag,
  IN     UINT8               *Cdb,
  IN     UINT8               CdbLength,
  IN     UFS_DATA_DIRECTION  DataDirection,
  IN     UINT32              ExpDataTranLen
  )
{
  UINT8  Flags;

  ASSERT ((Command != NULL) && (Cdb != NULL));

  //
  // Task attribute is hard-coded to Ordered.
  //
  if (DataDirection == UfsDataIn) {
    Flags = BIT0 | BIT6;
  } else if (DataDirection == UfsDataOut) {
    Flags = BIT0 | BIT5;
  } else {
    Flags = BIT0;
  }

  //
  // Fill UTP COMMAND UPIU associated fields.
  //
  Command->TransCode = 0x01;
  Command->Flags     = Flags;
  Command->Lun       = Lun;
  Command->TaskTag   = TaskTag;
  Command->CmdSet    = 0x00;
  SwapLittleEndianToBigEndian ((UINT8 *)&ExpDataTranLen, sizeof (ExpDataTranLen));
  Command->ExpDataTranLen = ExpDataTranLen;

  CopyMem (Command->Cdb, Cdb, CdbLength);

  return EFI_SUCCESS;
}

/**
  Initialize UTP PRDT for data transfer.

  @param[in] Prdt         The base address of PRDT.
  @param[in] Buffer       The buffer to be read or written.
  @param[in] BufferSize   The data size to be read or written.

  @retval EFI_SUCCESS     The initialization succeed.

**/
EFI_STATUS
UfsInitUtpPrdt (
  IN  UTP_TR_PRD  *Prdt,
  IN  VOID        *Buffer,
  IN  UINT32      BufferSize
  )
{
  UINTN   PrdtIndex;
  UINT32  RemainingLen;
  UINT8   *Remaining;
  UINTN   PrdtNumber;

  ASSERT (((UINTN)Buffer & (BIT0 | BIT1)) == 0);
  ASSERT ((BufferSize & (BIT1 | BIT0)) == 0);

  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  RemainingLen = BufferSize;
  Remaining    = Buffer;
  PrdtNumber   = (UINTN)DivU64x32 ((UINT64)BufferSize + UFS_MAX_DATA_LEN_PER_PRD - 1, UFS_MAX_DATA_LEN_PER_PRD);

  for (PrdtIndex = 0; PrdtIndex < PrdtNumber; PrdtIndex++) {
    if (RemainingLen < UFS_MAX_DATA_LEN_PER_PRD) {
      Prdt[PrdtIndex].DbCount = (UINT32)RemainingLen - 1;
    } else {
      Prdt[PrdtIndex].DbCount = UFS_MAX_DATA_LEN_PER_PRD - 1;
    }

    Prdt[PrdtIndex].DbAddr  = (UINT32)RShiftU64 ((UINT64)(UINTN)Remaining, 2);
    Prdt[PrdtIndex].DbAddrU = (UINT32)RShiftU64 ((UINT64)(UINTN)Remaining, 32);
    RemainingLen           -= UFS_MAX_DATA_LEN_PER_PRD;
    Remaining              += UFS_MAX_DATA_LEN_PER_PRD;
  }

  return EFI_SUCCESS;
}

/**
  Initialize QUERY REQUEST UPIU.

  @param[in, out] QueryReq      The base address of QUERY REQUEST UPIU.
  @param[in]      TaskTag       The task tag of request.
  @param[in]      Opcode        The opcode of request.
  @param[in]      DescId        The descriptor ID of request.
  @param[in]      Index         The index of request.
  @param[in]      Selector      The selector of request.
  @param[in]      DataSize      The data size to be read or written.
  @param[in]      Data          The buffer to be read or written.

  @retval EFI_SUCCESS           The initialization succeed.

**/
EFI_STATUS
UfsInitQueryRequestUpiu (
  IN OUT UTP_QUERY_REQ_UPIU  *QueryReq,
  IN     UINT8               TaskTag,
  IN     UINT8               Opcode,
  IN     UINT8               DescId,
  IN     UINT8               Index,
  IN     UINT8               Selector,
  IN     UINTN               DataSize   OPTIONAL,
  IN     UINT8               *Data      OPTIONAL
  )
{
  ASSERT (QueryReq != NULL);

  QueryReq->TransCode = 0x16;
  QueryReq->TaskTag   = TaskTag;
  if ((Opcode == UtpQueryFuncOpcodeRdDesc) || (Opcode == UtpQueryFuncOpcodeRdFlag) || (Opcode == UtpQueryFuncOpcodeRdAttr)) {
    QueryReq->QueryFunc = QUERY_FUNC_STD_READ_REQ;
  } else {
    QueryReq->QueryFunc = QUERY_FUNC_STD_WRITE_REQ;
  }

  if (Opcode == UtpQueryFuncOpcodeWrAttr) {
    UfsFillTsfOfQueryReqUpiu (&QueryReq->Tsf, Opcode, DescId, Index, Selector, 0, *(UINT32 *)Data);
  } else if ((Opcode == UtpQueryFuncOpcodeRdDesc) || (Opcode == UtpQueryFuncOpcodeWrDesc)) {
    UfsFillTsfOfQueryReqUpiu (&QueryReq->Tsf, Opcode, DescId, Index, Selector, (UINT16)DataSize, 0);
  } else {
    UfsFillTsfOfQueryReqUpiu (&QueryReq->Tsf, Opcode, DescId, Index, Selector, 0, 0);
  }

  if (Opcode == UtpQueryFuncOpcodeWrDesc) {
    CopyMem (QueryReq + 1, Data, DataSize);

    SwapLittleEndianToBigEndian ((UINT8 *)&DataSize, sizeof (UINT16));
    QueryReq->DataSegLen = (UINT16)DataSize;
  }

  return EFI_SUCCESS;
}

/**
  Allocate COMMAND/RESPONSE UPIU for filling UTP TRD's command descriptor field.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Lun               The Lun on which the SCSI command is executed.
  @param[in]  Packet            The pointer to the EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET data structure.
  @param[in]  Trd               The pointer to the UTP Transfer Request Descriptor.
  @param[out] CmdDescHost       A pointer to store the base system memory address of the allocated range.
  @param[out] CmdDescMapping    A resulting value to pass to Unmap().

  @retval EFI_SUCCESS           The creation succeed.
  @retval EFI_DEVICE_ERROR      The creation failed.
  @retval EFI_OUT_OF_RESOURCES  The memory resource is insufficient.

**/
EFI_STATUS
UfsCreateScsiCommandDesc (
  IN     UFS_PASS_THRU_PRIVATE_DATA                  *Private,
  IN     UINT8                                       Lun,
  IN     EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet,
  IN     UTP_TRD                                     *Trd,
  OUT VOID                                           **CmdDescHost,
  OUT VOID                                           **CmdDescMapping
  )
{
  UINTN                 TotalLen;
  UINTN                 PrdtNumber;
  UTP_COMMAND_UPIU      *CommandUpiu;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;
  EFI_STATUS            Status;
  UINT32                DataLen;
  UFS_DATA_DIRECTION    DataDirection;

  ASSERT ((Private != NULL) && (Packet != NULL) && (Trd != NULL));

  if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ) {
    DataLen       = Packet->InTransferLength;
    DataDirection = UfsDataIn;
  } else {
    DataLen       = Packet->OutTransferLength;
    DataDirection = UfsDataOut;
  }

  if (DataLen == 0) {
    DataDirection = UfsNoData;
  }

  PrdtNumber = (UINTN)DivU64x32 ((UINT64)DataLen + UFS_MAX_DATA_LEN_PER_PRD - 1, UFS_MAX_DATA_LEN_PER_PRD);

  TotalLen = ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)) + ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)) + PrdtNumber * sizeof (UTP_TR_PRD);

  Status = UfsAllocateAlignCommonBuffer (Private, TotalLen, CmdDescHost, &CmdDescPhyAddr, CmdDescMapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CommandUpiu = (UTP_COMMAND_UPIU *)*CmdDescHost;

  UfsInitCommandUpiu (CommandUpiu, Lun, Private->TaskTag++, Packet->Cdb, Packet->CdbLength, DataDirection, DataLen);

  //
  // Fill UTP_TRD associated fields
  // NOTE: Some UFS host controllers request the Response UPIU and the Physical Region Description Table
  // *MUST* be located at a 64-bit aligned boundary.
  //
  Trd->Int    = UFS_INTERRUPT_COMMAND;
  Trd->Dd     = DataDirection;
  Trd->Ct     = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs    = UFS_HC_TRD_OCS_INIT_VALUE;
  Trd->UcdBa  = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 7);
  Trd->UcdBaU = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32);
  Trd->RuL    = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)), sizeof (UINT32));
  Trd->RuO    = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)), sizeof (UINT32));
  Trd->PrdtL  = (UINT16)PrdtNumber;
  Trd->PrdtO  = (UINT16)DivU64x32 ((UINT64)(ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)) + ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU))), sizeof (UINT32));
  return EFI_SUCCESS;
}

/**
  Allocate QUERY REQUEST/QUERY RESPONSE UPIU for filling UTP TRD's command descriptor field.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Packet            The pointer to the UFS_DEVICE_MANAGEMENT_REQUEST_PACKET data structure.
  @param[in]  Trd               The pointer to the UTP Transfer Request Descriptor.
  @param[out] CmdDescHost       A pointer to store the base system memory address of the allocated range.
  @param[out] CmdDescMapping    A resulting value to pass to Unmap().

  @retval EFI_SUCCESS           The creation succeed.
  @retval EFI_DEVICE_ERROR      The creation failed.
  @retval EFI_OUT_OF_RESOURCES  The memory resource is insufficient.
  @retval EFI_INVALID_PARAMETER The parameter passed in is invalid.

**/
EFI_STATUS
UfsCreateDMCommandDesc (
  IN     UFS_PASS_THRU_PRIVATE_DATA            *Private,
  IN     UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet,
  IN     UTP_TRD                               *Trd,
  OUT VOID                                     **CmdDescHost,
  OUT VOID                                     **CmdDescMapping
  )
{
  UINTN                 TotalLen;
  UTP_QUERY_REQ_UPIU    *QueryReqUpiu;
  UINT8                 Opcode;
  UINT32                DataSize;
  UINT8                 *Data;
  UINT8                 DataDirection;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;
  EFI_STATUS            Status;

  ASSERT ((Private != NULL) && (Packet != NULL) && (Trd != NULL));

  Opcode = Packet->Opcode;
  if ((Opcode > UtpQueryFuncOpcodeTogFlag) || (Opcode == UtpQueryFuncOpcodeNop)) {
    return EFI_INVALID_PARAMETER;
  }

  DataDirection = Packet->DataDirection;
  DataSize      = Packet->TransferLength;
  Data          = Packet->DataBuffer;

  if ((Opcode == UtpQueryFuncOpcodeWrDesc) || (Opcode == UtpQueryFuncOpcodeRdDesc)) {
    if ((DataSize == 0) || (Data == NULL)) {
      return EFI_INVALID_PARAMETER;
    }

    TotalLen = ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU)) + ROUNDUP8 (DataSize);
  } else {
    TotalLen = ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU));
  }

  Status = UfsAllocateAlignCommonBuffer (Private, TotalLen, CmdDescHost, &CmdDescPhyAddr, CmdDescMapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Initialize UTP QUERY REQUEST UPIU
  //
  QueryReqUpiu = (UTP_QUERY_REQ_UPIU *)*CmdDescHost;
  ASSERT (QueryReqUpiu != NULL);
  UfsInitQueryRequestUpiu (
    QueryReqUpiu,
    Private->TaskTag++,
    Opcode,
    Packet->DescId,
    Packet->Index,
    Packet->Selector,
    DataSize,
    Data
    );

  //
  // Fill UTP_TRD associated fields
  // NOTE: Some UFS host controllers request the Query Response UPIU *MUST* be located at a 64-bit aligned boundary.
  //
  Trd->Int    = UFS_INTERRUPT_COMMAND;
  Trd->Dd     = DataDirection;
  Trd->Ct     = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs    = UFS_HC_TRD_OCS_INIT_VALUE;
  Trd->UcdBa  = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 7);
  Trd->UcdBaU = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32);
  if (Opcode == UtpQueryFuncOpcodeWrDesc) {
    Trd->RuL = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU)), sizeof (UINT32));
    Trd->RuO = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (DataSize), sizeof (UINT32));
  } else {
    Trd->RuL = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU)) + ROUNDUP8 (DataSize), sizeof (UINT32));
    Trd->RuO = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)), sizeof (UINT32));
  }

  // UFS DM Command, PRDT Length must be Zero.
  Trd->PrdtL = 0x00;

  return EFI_SUCCESS;
}

/**
  Allocate NOP IN and NOP OUT UPIU for filling UTP TRD's command descriptor field.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Trd               The pointer to the UTP Transfer Request Descriptor.
  @param[out] CmdDescHost       A pointer to store the base system memory address of the allocated range.
  @param[out] CmdDescMapping    A resulting value to pass to Unmap().

  @retval EFI_SUCCESS           The creation succeed.
  @retval EFI_DEVICE_ERROR      The creation failed.
  @retval EFI_OUT_OF_RESOURCES  The memory resource is insufficient.

**/
EFI_STATUS
UfsCreateNopCommandDesc (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     UTP_TRD                     *Trd,
  OUT VOID                           **CmdDescHost,
  OUT VOID                           **CmdDescMapping
  )
{
  UINTN                 TotalLen;
  UTP_NOP_OUT_UPIU      *NopOutUpiu;
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;

  ASSERT ((Private != NULL) && (Trd != NULL));

  TotalLen = ROUNDUP8 (sizeof (UTP_NOP_OUT_UPIU)) + ROUNDUP8 (sizeof (UTP_NOP_IN_UPIU));
  Status   = UfsAllocateAlignCommonBuffer (Private, TotalLen, CmdDescHost, &CmdDescPhyAddr, CmdDescMapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  NopOutUpiu = (UTP_NOP_OUT_UPIU *)*CmdDescHost;
  ASSERT (NopOutUpiu != NULL);
  NopOutUpiu->TaskTag = Private->TaskTag++;

  //
  // Fill UTP_TRD associated fields
  // NOTE: Some UFS host controllers request the Nop Out UPIU *MUST* be located at a 64-bit aligned boundary.
  //
  Trd->Int    = UFS_INTERRUPT_COMMAND;
  Trd->Dd     = 0x00;
  Trd->Ct     = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs    = UFS_HC_TRD_OCS_INIT_VALUE;
  Trd->UcdBa  = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 7);
  Trd->UcdBaU = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32);
  Trd->RuL    = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_NOP_IN_UPIU)), sizeof (UINT32));
  Trd->RuO    = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_NOP_OUT_UPIU)), sizeof (UINT32));

  return EFI_SUCCESS;
}

/**
  Find out available slot in transfer list of a UFS device.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[out] Slot          The available slot.

  @retval EFI_SUCCESS       The available slot was found successfully.
  @retval EFI_NOT_READY     No slot is available at this moment.

**/
EFI_STATUS
UfsFindAvailableSlotInTrl (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  OUT UINT8                          *Slot
  )
{
  UINT8       Nutrs;
  UINT8       Index;
  UINT32      Data;
  EFI_STATUS  Status;

  ASSERT ((Private != NULL) && (Slot != NULL));

  Status = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Nutrs = (UINT8)((Private->UfsHcInfo.Capabilities & UFS_HC_CAP_NUTRS) + 1);

  for (Index = 0; Index < Nutrs; Index++) {
    if ((Data & (((UINT32)1U << Index))) == 0) {
      *Slot = Index;
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_READY;
}

STATIC
EFI_STATUS
UfsReserveAvailableTrlSlot (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN OUT UFS_PASS_THRU_TRANS_REQ     *TransReq
  )
{
  EFI_TPL   OldTpl;
  EFI_STATUS Status;
  UINT32    Doorbell;
  UINT32    SlotMask;
  UINT8     Nutrs;
  UINT8     Slot;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!IsListEmpty (&TransReq->TransferList) || (TransReq->Slot != MAX_UINT8)) {
    gBS->RestoreTPL (OldTpl);
    return EFI_INVALID_PARAMETER;
  }

  if (Private->Nutrs == 0) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Doorbell);
  if (EFI_ERROR (Status)) {
    gBS->RestoreTPL (OldTpl);
    return Status;
  }

  Nutrs = Private->Nutrs;
  if (Nutrs > 32) {
    Nutrs = 32;
  }

  for (Slot = 0; Slot < Nutrs; Slot++) {
    SlotMask = ((UINT32)1U << Slot);
    if (((Doorbell | Private->ReservedTrlSlots) & SlotMask) == 0) {
      Private->ReservedTrlSlots |= SlotMask;
      TransReq->Slot = Slot;
      InsertTailList (&Private->Queue, &TransReq->TransferList);
      gBS->RestoreTPL (OldTpl);
      return EFI_SUCCESS;
    }
  }

  gBS->RestoreTPL (OldTpl);
  return EFI_NOT_READY;
}

/**
  Start specified slot in transfer list of a UFS device.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Slot          The slot to be started.

**/
EFI_STATUS
UfsStartExecCmd (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINT8                       Slot,
  OUT BOOLEAN                     *DoorbellAttempted OPTIONAL
  )
{
  UINT32      Data;
  EFI_STATUS  Status;

  if (DoorbellAttempted != NULL) {
    *DoorbellAttempted = FALSE;
  }

  if ((Slot >= Private->Nutrs) || (Slot >= 32)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTRLRSR_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Data & UFS_HC_UTRLRSR) != UFS_HC_UTRLRSR) {
    Status = UfsMmioWrite32 (Private, UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTRLRSR);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (DoorbellAttempted != NULL) {
    *DoorbellAttempted = TRUE;
  }

  return UfsMmioWrite32 (Private, UFS_HC_UTRLDBR_OFFSET, ((UINT32)1U << Slot));
}

/**
  Stop specified slot in transfer list of a UFS device.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Slot          The slot to be stop.

**/
EFI_STATUS
UfsStopExecCmd (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINT8                       Slot
  )
{
  UINT32      Data;
  EFI_STATUS  Status;
  UINT32      SlotMask;

  if ((Slot >= Private->Nutrs) || (Slot >= 32)) {
    return EFI_INVALID_PARAMETER;
  }

  SlotMask = ((UINT32)1U << Slot);
  Status   = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Data);
  if (EFI_ERROR (Status) || ((Data & SlotMask) == 0)) {
    return Status;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTRLRSR_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Data & UFS_HC_UTRLRSR) == 0) {
    return EFI_NOT_READY;
  }

  // UTRLCLR is a write-zero-to-clear command register. Do not read-modify-write it.
  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLCLR_OFFSET, ~SlotMask);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return UfsWaitMemSet (Private, UFS_HC_UTRLDBR_OFFSET, SlotMask, 0, UFS_TIMEOUT);
}

/**
  Extracts return data from query response upiu.

  @param[in] Packet     Pointer to the UFS_DEVICE_MANAGEMENT_REQUEST_PACKET.
  @param[in] QueryResp  Pointer to the query response.

  @retval EFI_INVALID_PARAMETER Packet or QueryResp are empty or opcode is invalid.
  @retval EFI_DEVICE_ERROR      Data returned from device is invalid.
  @retval EFI_SUCCESS           Data extracted.

**/
EFI_STATUS
UfsGetReturnDataFromQueryResponse (
  IN UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet,
  IN UTP_QUERY_RESP_UPIU                   *QueryResp
  )
{
  UINT16  ReturnDataSize;
  UINT32  ReturnData;

  if ((Packet == NULL) || (QueryResp == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Packet->Opcode) {
    case UtpQueryFuncOpcodeRdDesc:
      ReturnDataSize = QueryResp->Tsf.Length;
      SwapLittleEndianToBigEndian ((UINT8 *)&ReturnDataSize, sizeof (UINT16));
      //
      // Make sure the hardware device does not return more data than expected.
      //
      if (ReturnDataSize > Packet->TransferLength) {
        return EFI_DEVICE_ERROR;
      }

      CopyMem (Packet->DataBuffer, (QueryResp + 1), ReturnDataSize);
      Packet->TransferLength = ReturnDataSize;
      break;
    case UtpQueryFuncOpcodeWrDesc:
      ReturnDataSize = QueryResp->Tsf.Length;
      SwapLittleEndianToBigEndian ((UINT8 *)&ReturnDataSize, sizeof (UINT16));
      Packet->TransferLength = ReturnDataSize;
      break;
    case UtpQueryFuncOpcodeRdFlag:
    case UtpQueryFuncOpcodeSetFlag:
    case UtpQueryFuncOpcodeClrFlag:
    case UtpQueryFuncOpcodeTogFlag:
      //
      // The 'FLAG VALUE' field is at byte offset 3 of QueryResp->Tsf.Value
      //
      *((UINT8 *)(Packet->DataBuffer)) = *((UINT8 *)&(QueryResp->Tsf.Value) + 3);
      break;
    case UtpQueryFuncOpcodeRdAttr:
    case UtpQueryFuncOpcodeWrAttr:
      ReturnData = QueryResp->Tsf.Value;
      SwapLittleEndianToBigEndian ((UINT8 *)&ReturnData, sizeof (UINT32));
      CopyMem (Packet->DataBuffer, &ReturnData, sizeof (UINT32));
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Creates Transfer Request descriptor and sends Query Request to the device.

  @param[in] Private Pointer to the UFS_PASS_THRU_PRIVATE_DATA.
  @param[in] Packet  Pointer to the UFS_DEVICE_MANAGEMENT_REQUEST_PACKET.

  @retval EFI_SUCCESS           The device descriptor was read/written successfully.
  @retval EFI_INVALID_PARAMETER The DescId, Index and Selector fields in Packet are invalid
                                combination to point to a type of UFS device descriptor.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to r/w the device descriptor.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of r/w the device descriptor.

**/
EFI_STATUS
UfsSendDmRequestRetry (
  IN  UFS_PASS_THRU_PRIVATE_DATA            *Private,
  IN  UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet,
  OUT BOOLEAN                               *RequestRetained
  )
{
  UINT8                               Slot;
  UTP_QUERY_RESP_UPIU                 *QueryResp;
  UFS_PASS_THRU_TRANS_REQ             *TransReq;
  UINTN                               RequestSize;
  EFI_STATUS                          Status;
  EFI_STATUS                          CleanupStatus;
  EFI_STATUS                          OriginalStatus;
  BOOLEAN                             Retained;

  if ((Private == NULL) || (Packet == NULL) || (RequestRetained == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *RequestRetained = FALSE;
  TransReq = AllocateZeroPool (sizeof (*TransReq));
  if (TransReq == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  TransReq->Signature = UFS_PASS_THRU_TRANS_REQ_SIG;
  InitializeListHead (&TransReq->TransferList);
  TransReq->Slot = MAX_UINT8;

  if ((Packet->Opcode == UtpQueryFuncOpcodeWrDesc) || (Packet->Opcode == UtpQueryFuncOpcodeRdDesc)) {
    RequestSize = ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU)) + ROUNDUP8 (Packet->TransferLength);
  } else {
    RequestSize = ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU));
  }

  TransReq->CmdDescSize = (UINT32)RequestSize;
  Status = UfsReserveAvailableTrlSlot (Private, TransReq);
  if (EFI_ERROR (Status)) {
    FreePool (TransReq);
    return Status;
  }

  Slot = TransReq->Slot;
  TransReq->Trd = ((UTP_TRD *)Private->UtpTrlBase) + Slot;
  Status = UfsCreateDMCommandDesc (Private, Packet, TransReq->Trd, &TransReq->CmdDescHost, &TransReq->CmdDescMapping);
  if (EFI_ERROR (Status)) {
    CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
    if (EFI_ERROR (CleanupStatus)) {
      UfsRetainOrFreeTransferRequest (Private, TransReq);
      *RequestRetained = TRUE;
    } else {
      UfsUnqueueTransferRequest (Private, TransReq);
      FreePool (TransReq);
    }

    return Status;
  }

  QueryResp = (UTP_QUERY_RESP_UPIU *)((UINT8 *)TransReq->CmdDescHost + TransReq->Trd->RuO * sizeof (UINT32));
  Status = UfsStartExecCmd (Private, Slot, NULL);
  if (EFI_ERROR (Status)) {
    goto AbortRequest;
  }

  Status = UfsWaitMemSet (Private, UFS_HC_UTRLDBR_OFFSET, ((UINT32)1U << Slot), 0, Packet->Timeout);
  if (EFI_ERROR (Status)) {
    goto AbortRequest;
  }

  Status = Private->UfsHostController->Flush (Private->UfsHostController);
  if (EFI_ERROR (Status)) {
    *RequestRetained = TRUE;
    return Status;
  }

  if ((TransReq->Trd->Ocs != 0) || (QueryResp->QueryResp != UfsUtpQueryResponseSuccess)) {
    DEBUG ((DEBUG_ERROR, "Failed to send query request, OCS = %X, QueryResp = %X\n", TransReq->Trd->Ocs, QueryResp->QueryResp));
    DumpQueryResponseResult (QueryResp->QueryResp);

    if ((QueryResp->QueryResp == UfsUtpQueryResponseInvalidSelector) ||
        (QueryResp->QueryResp == UfsUtpQueryResponseInvalidIndex) ||
        (QueryResp->QueryResp == UfsUtpQueryResponseInvalidIdn))
    {
      Status = EFI_INVALID_PARAMETER;
    } else {
      Status = EFI_DEVICE_ERROR;
    }
  } else {
    Status = UfsGetReturnDataFromQueryResponse (Packet, QueryResp);
  }

  CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
  if (EFI_ERROR (CleanupStatus)) {
    *RequestRetained = TRUE;
    if (!EFI_ERROR (Status)) {
      Status = CleanupStatus;
    }

    return Status;
  }

  UfsUnqueueTransferRequest (Private, TransReq);
  FreePool (TransReq);
  return Status;

AbortRequest:
  OriginalStatus = Status;
  Status = UfsStopExecCmd (Private, Slot);
  if (!EFI_ERROR (Status)) {
    Status = Private->UfsHostController->Flush (Private->UfsHostController);
    if (!EFI_ERROR (Status)) {
      CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
      if (!EFI_ERROR (CleanupStatus)) {
        UfsUnqueueTransferRequest (Private, TransReq);
        FreePool (TransReq);
        return OriginalStatus;
      }
    }
  }

  Retained = UfsTransferRequestOwnsResources (TransReq);
  UfsRetainOrFreeTransferRequest (Private, TransReq);
  *RequestRetained = Retained;
  return OriginalStatus;
}

/**
  Sends Query Request to the device. Query is sent until device responds correctly or counter runs out.

  @param[in] Private Pointer to the UFS_PASS_THRU_PRIVATE_DATA.
  @param[in] Packet  Pointer to the UFS_DEVICE_MANAGEMENT_PACKET.

  @retval EFI_SUCCESS           The device responded correctly to the Query request.
  @retval EFI_INVALID_PARAMETER The DescId, Index and Selector fields in Packet are invalid
                                combination to point to a type of UFS device descriptor.
  @retval EFI_DEVICE_ERROR      A device error occurred while waiting for the response from the device.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of the operation.

**/
EFI_STATUS
UfsSendDmRequest (
  IN UFS_PASS_THRU_PRIVATE_DATA            *Private,
  IN UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet
  )
{
  EFI_STATUS  Status;
  UINT8       Retry;
  BOOLEAN     RequestRetained;

  for (Retry = 0; Retry < 5; Retry++) {
    Status = UfsSendDmRequestRetry (Private, Packet, &RequestRetained);
    if (!EFI_ERROR (Status) || RequestRetained) {
      return Status;
    }
  }

  DEBUG ((DEBUG_ERROR, "Failed to get response from the device after %d retries\n", Retry));
  return Status;
}

/**
  Read or write specified device descriptor of a UFS device.

  @param[in]      Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]      Read          The boolean variable to show r/w direction.
  @param[in]      DescId        The ID of device descriptor.
  @param[in]      Index         The Index of device descriptor.
  @param[in]      Selector      The Selector of device descriptor.
  @param[in, out] Descriptor    The buffer of device descriptor to be read or written.
  @param[in, out] DescSize      The size of device descriptor buffer. On input, the size, in bytes,
                                of the data buffer specified by Descriptor. On output, the number
                                of bytes that were actually transferred.

  @retval EFI_SUCCESS           The device descriptor was read/written successfully.
  @retval EFI_INVALID_PARAMETER DescId, Index and Selector are invalid combination to point to a
                                type of UFS device descriptor.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to r/w the device descriptor.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of r/w the device descriptor.

**/
EFI_STATUS
UfsRwDeviceDesc (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     BOOLEAN                     Read,
  IN     UINT8                       DescId,
  IN     UINT8                       Index,
  IN     UINT8                       Selector,
  IN OUT VOID                        *Descriptor,
  IN OUT UINT32                      *DescSize
  )
{
  UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  Packet;
  EFI_STATUS                            Status;

  if (DescSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Packet, sizeof (UFS_DEVICE_MANAGEMENT_REQUEST_PACKET));

  if (Read) {
    Packet.DataDirection = UfsDataIn;
    Packet.Opcode        = UtpQueryFuncOpcodeRdDesc;
  } else {
    Packet.DataDirection = UfsDataOut;
    Packet.Opcode        = UtpQueryFuncOpcodeWrDesc;
  }

  Packet.DataBuffer     = Descriptor;
  Packet.TransferLength = *DescSize;
  Packet.DescId         = DescId;
  Packet.Index          = Index;
  Packet.Selector       = Selector;
  Packet.Timeout        = UFS_TIMEOUT;

  Status = UfsSendDmRequest (Private, &Packet);
  if (EFI_ERROR (Status)) {
    *DescSize = 0;
  } else {
    *DescSize = Packet.TransferLength;
  }

  return Status;
}

/**
  Read or write specified attribute of a UFS device.

  @param[in]      Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]      Read          The boolean variable to show r/w direction.
  @param[in]      AttrId        The ID of Attribute.
  @param[in]      Index         The Index of Attribute.
  @param[in]      Selector      The Selector of Attribute.
  @param[in, out] Attributes    The value of Attribute to be read or written.

  @retval EFI_SUCCESS           The Attribute was read/written successfully.
  @retval EFI_INVALID_PARAMETER AttrId, Index and Selector are invalid combination to point to a
                                type of UFS device descriptor.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to r/w the Attribute.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of r/w the Attribute.

**/
EFI_STATUS
UfsRwAttributes (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     BOOLEAN                     Read,
  IN     UINT8                       AttrId,
  IN     UINT8                       Index,
  IN     UINT8                       Selector,
  IN OUT UINT32                      *Attributes
  )
{
  UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  Packet;

  ZeroMem (&Packet, sizeof (UFS_DEVICE_MANAGEMENT_REQUEST_PACKET));

  if (Read) {
    Packet.DataDirection = UfsDataIn;
    Packet.Opcode        = UtpQueryFuncOpcodeRdAttr;
  } else {
    Packet.DataDirection = UfsDataOut;
    Packet.Opcode        = UtpQueryFuncOpcodeWrAttr;
  }

  Packet.DataBuffer = Attributes;
  Packet.DescId     = AttrId;
  Packet.Index      = Index;
  Packet.Selector   = Selector;
  Packet.Timeout    = UFS_TIMEOUT;

  return UfsSendDmRequest (Private, &Packet);
}

/**
  Read or write specified flag of a UFS device.

  @param[in]      Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]      Read          The boolean variable to show r/w direction.
  @param[in]      FlagId        The ID of flag to be read or written.
  @param[in, out] Value         The value to set or clear flag.

  @retval EFI_SUCCESS           The flag was read/written successfully.
  @retval EFI_INVALID_PARAMETER FlagId is an invalid UFS flag ID.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to r/w the flag.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of r/w the flag.

**/
EFI_STATUS
UfsRwFlags (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     BOOLEAN                     Read,
  IN     UINT8                       FlagId,
  IN OUT UINT8                       *Value
  )
{
  UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  Packet;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Packet, sizeof (UFS_DEVICE_MANAGEMENT_REQUEST_PACKET));

  if (Read) {
    ASSERT (Value != NULL);
    Packet.DataDirection = UfsDataIn;
    Packet.Opcode        = UtpQueryFuncOpcodeRdFlag;
  } else {
    Packet.DataDirection = UfsDataOut;
    if (*Value == 1) {
      Packet.Opcode = UtpQueryFuncOpcodeSetFlag;
    } else if (*Value == 0) {
      Packet.Opcode = UtpQueryFuncOpcodeClrFlag;
    } else {
      return EFI_INVALID_PARAMETER;
    }
  }

  Packet.DataBuffer = Value;
  Packet.DescId     = FlagId;
  Packet.Index      = 0;
  Packet.Selector   = 0;
  Packet.Timeout    = UFS_TIMEOUT;

  return UfsSendDmRequest (Private, &Packet);
}

/**
  Set specified flag to 1 on a UFS device.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  FlagId            The ID of flag to be set.

  @retval EFI_SUCCESS           The flag was set successfully.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to set the flag.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of setting the flag.

**/
EFI_STATUS
UfsSetFlag (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINT8                       FlagId
  )
{
  EFI_STATUS  Status;
  UINT8       Value;

  Value  = 1;
  Status = UfsRwFlags (Private, FALSE, FlagId, &Value);

  return Status;
}

/**
  Read specified flag from a UFS device.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  FlagId            The ID of flag to be read.
  @param[out] Value             The flag's value.

  @retval EFI_SUCCESS           The flag was read successfully.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to read the flag.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of reading the flag.

**/
EFI_STATUS
UfsReadFlag (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     UINT8                       FlagId,
  OUT UINT8                          *Value
  )
{
  EFI_STATUS  Status;

  Status = UfsRwFlags (Private, TRUE, FlagId, Value);

  return Status;
}

/**
  Sends NOP IN cmd to a UFS device for initialization process request.
  For more details, please refer to UFS 2.0 spec Figure 13.3.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS           The NOP IN command was sent by the host. The NOP OUT response was
                                received successfully.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to execute NOP IN command.
  @retval EFI_OUT_OF_RESOURCES  The resource for transfer is not available.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the NOP IN command to execute.

**/
EFI_STATUS
UfsExecNopCmds (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS              Status;
  EFI_STATUS              CleanupStatus;
  EFI_STATUS              OriginalStatus;
  UINT8                   Slot;
  UTP_NOP_IN_UPIU         *NopInUpiu;
  UFS_PASS_THRU_TRANS_REQ *TransReq;
  UINTN                   RequestSize;

  TransReq = AllocateZeroPool (sizeof (*TransReq));
  if (TransReq == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  TransReq->Signature = UFS_PASS_THRU_TRANS_REQ_SIG;
  InitializeListHead (&TransReq->TransferList);
  TransReq->Slot = MAX_UINT8;
  RequestSize = ROUNDUP8 (sizeof (UTP_NOP_OUT_UPIU)) + ROUNDUP8 (sizeof (UTP_NOP_IN_UPIU));
  TransReq->CmdDescSize = (UINT32)RequestSize;

  Status = UfsReserveAvailableTrlSlot (Private, TransReq);
  if (EFI_ERROR (Status)) {
    FreePool (TransReq);
    return Status;
  }

  Slot = TransReq->Slot;
  TransReq->Trd = ((UTP_TRD *)Private->UtpTrlBase) + Slot;
  Status = UfsCreateNopCommandDesc (Private, TransReq->Trd, &TransReq->CmdDescHost, &TransReq->CmdDescMapping);
  if (EFI_ERROR (Status)) {
    CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
    if (EFI_ERROR (CleanupStatus)) {
      UfsRetainOrFreeTransferRequest (Private, TransReq);
    } else {
      UfsUnqueueTransferRequest (Private, TransReq);
      FreePool (TransReq);
    }

    return Status;
  }

  NopInUpiu = (UTP_NOP_IN_UPIU *)((UINT8 *)TransReq->CmdDescHost + TransReq->Trd->RuO * sizeof (UINT32));
  Status = UfsStartExecCmd (Private, Slot, NULL);
  if (EFI_ERROR (Status)) {
    goto AbortRequest;
  }

  Status = UfsWaitMemSet (Private, UFS_HC_UTRLDBR_OFFSET, ((UINT32)1U << Slot), 0, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    goto AbortRequest;
  }

  Status = Private->UfsHostController->Flush (Private->UfsHostController);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (NopInUpiu->Resp != 0) {
    Status = EFI_DEVICE_ERROR;
  }

  CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
  if (EFI_ERROR (CleanupStatus)) {
    return EFI_ERROR (Status) ? Status : CleanupStatus;
  }

  UfsUnqueueTransferRequest (Private, TransReq);
  FreePool (TransReq);
  return Status;

AbortRequest:
  OriginalStatus = Status;
  Status = UfsStopExecCmd (Private, Slot);
  if (!EFI_ERROR (Status)) {
    Status = Private->UfsHostController->Flush (Private->UfsHostController);
    if (!EFI_ERROR (Status)) {
      CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
      if (!EFI_ERROR (CleanupStatus)) {
        UfsUnqueueTransferRequest (Private, TransReq);
        FreePool (TransReq);
        return OriginalStatus;
      }
    }
  }

  UfsRetainOrFreeTransferRequest (Private, TransReq);
  return OriginalStatus;
}

/**
  Cleanup data buffers after data transfer. This function
  also takes care to copy all data to user memory pool for
  unaligned data transfers.

  @param[in] Private   Pointer to the UFS_PASS_THRU_PRIVATE_DATA
  @param[in] TransReq  Pointer to the transfer request
**/
VOID
UfsReconcileDataTransferBuffer (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN UFS_PASS_THRU_TRANS_REQ     *TransReq
  )
{
  EFI_STATUS  Status;

  if (TransReq->DataBufMapping != NULL) {
    Status = Private->UfsHostController->Unmap (Private->UfsHostController, TransReq->DataBufMapping);
    if (EFI_ERROR (Status)) {
      return;
    }

    TransReq->DataBufMapping = NULL;
  }

  if (TransReq->AlignedDataBuf != NULL) {
    ZeroMem (TransReq->AlignedDataBuf, TransReq->AlignedDataBufSize);
    FreeAlignedPages (TransReq->AlignedDataBuf, EFI_SIZE_TO_PAGES (TransReq->AlignedDataBufSize));
    TransReq->AlignedDataBuf     = NULL;
    TransReq->AlignedDataBufSize = 0;
  }
}

/**
  Prepare data buffer for transfer.

  @param[in]      Private   Pointer to the UFS_PASS_THRU_PRIVATE_DATA
  @param[in, out] TransReq  Pointer to the transfer request

  @retval EFI_DEVICE_ERROR  Failed to prepare buffer for transfer
  @retval EFI_SUCCESS       Buffer ready for transfer
**/
EFI_STATUS
UfsPrepareDataTransferBuffer (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN OUT UFS_PASS_THRU_TRANS_REQ     *TransReq
  )
{
  EFI_STATUS                           Status;
  EFI_STATUS                           CleanupStatus;
  VOID                                 *DataBuf;
  UINT32                               DataLen;
  UINTN                                MapLength;
  EFI_PHYSICAL_ADDRESS                 DataBufPhyAddr;
  EDKII_UFS_HOST_CONTROLLER_OPERATION  Flag;
  UTP_TR_PRD                           *PrdtBase;

  DataBufPhyAddr = 0;
  DataBuf        = NULL;
  DataLen        = 0;
  MapLength      = 0;

  if (TransReq->Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ) {
    DataLen = TransReq->Packet->InTransferLength;
    if ((DataLen != 0) && (TransReq->Packet->InDataBuffer == NULL)) {
      return EFI_INVALID_PARAMETER;
    }

    Flag = EdkiiUfsHcOperationBusMasterWrite;
  } else if (TransReq->Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_WRITE) {
    DataLen = TransReq->Packet->OutTransferLength;
    if ((DataLen != 0) && (TransReq->Packet->OutDataBuffer == NULL)) {
      return EFI_INVALID_PARAMETER;
    }

    Flag = EdkiiUfsHcOperationBusMasterRead;
  } else {
    return EFI_INVALID_PARAMETER;
  }

  if (DataLen != 0) {
    if (DataLen > MAX_UINT32 - 3) {
      return EFI_BAD_BUFFER_SIZE;
    }

    TransReq->AlignedDataBufSize = (DataLen + 3) & ~((UINTN)3);
    TransReq->AlignedDataBuf     = AllocateAlignedPages (EFI_SIZE_TO_PAGES (TransReq->AlignedDataBufSize), 4);
    if (TransReq->AlignedDataBuf == NULL) {
      TransReq->AlignedDataBufSize = 0;
      return EFI_OUT_OF_RESOURCES;
    }

    ZeroMem (TransReq->AlignedDataBuf, TransReq->AlignedDataBufSize);
    if (Flag == EdkiiUfsHcOperationBusMasterRead) {
      CopyMem (TransReq->AlignedDataBuf, TransReq->Packet->OutDataBuffer, DataLen);
    }

    DataBuf  = TransReq->AlignedDataBuf;
    MapLength = TransReq->AlignedDataBufSize;
    Status   = Private->UfsHostController->Map (
                                            Private->UfsHostController,
                                            Flag,
                                            DataBuf,
                                            &MapLength,
                                            &DataBufPhyAddr,
                                            &TransReq->DataBufMapping
                                            );

    if (!EFI_ERROR (Status) && (MapLength == TransReq->AlignedDataBufSize) && (TransReq->DataBufMapping != NULL) &&
        ((Private->UfsHcInfo.Capabilities & UFS_HC_CAP_64ADDR) == 0) &&
        ((DataBufPhyAddr >= 0x100000000ULL) || (MapLength > (0x100000000ULL - DataBufPhyAddr))))
    {
      Status = EFI_DEVICE_ERROR;
    }

    if (EFI_ERROR (Status) || (MapLength != TransReq->AlignedDataBufSize) || (TransReq->DataBufMapping == NULL)) {
      if (TransReq->DataBufMapping != NULL) {
        CleanupStatus = Private->UfsHostController->Unmap (Private->UfsHostController, TransReq->DataBufMapping);
        if (!EFI_ERROR (CleanupStatus)) {
          TransReq->DataBufMapping = NULL;
        }
      }

      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
  }

  PrdtBase = (UTP_TR_PRD *)((UINT8 *)TransReq->CmdDescHost + ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)) + ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)));
  if (PrdtBase == NULL) {
    return EFI_DEVICE_ERROR;
  }

  UfsInitUtpPrdt (PrdtBase, (VOID *)(UINTN)DataBufPhyAddr, (UINT32)TransReq->AlignedDataBufSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
UfsValidateScsiResponse (
  IN  UFS_PASS_THRU_TRANS_REQ  *TransReq,
  OUT BOOLEAN                 *CopyReadData
  )
{
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet;
  UTP_RESPONSE_UPIU                           *Response;
  UTP_COMMAND_UPIU                            *Request;
  UINTN                                       ResponseOffset;
  UINT16                                      SenseDataLen;
  UINT16                                      DataSegLen;
  UINT32                                      Residual;
  UINT32                                      Requested;

  if ((TransReq == NULL) || (TransReq->Packet == NULL) || (TransReq->Trd == NULL) ||
      (TransReq->CmdDescHost == NULL) || (CopyReadData == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Packet       = TransReq->Packet;
  *CopyReadData = FALSE;
  ResponseOffset = (UINTN)TransReq->Trd->RuO * sizeof (UINT32);
  if ((TransReq->CmdDescSize < sizeof (UTP_COMMAND_UPIU)) ||
      (ResponseOffset != ROUNDUP8 (sizeof (UTP_COMMAND_UPIU))) ||
      (TransReq->Trd->RuL != ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)) / sizeof (UINT32)) ||
      (ResponseOffset > TransReq->CmdDescSize) ||
      (sizeof (UTP_RESPONSE_UPIU) > TransReq->CmdDescSize - ResponseOffset))
  {
    return EFI_DEVICE_ERROR;
  }

  Request  = (UTP_COMMAND_UPIU *)TransReq->CmdDescHost;
  Response = (UTP_RESPONSE_UPIU *)((UINT8 *)TransReq->CmdDescHost + ResponseOffset);
  if ((Response->TransCode != 0x21) || (Response->TaskTag != Request->TaskTag) ||
      (Response->Lun != Request->Lun) ||
      ((Response->Flags & BIT6) != 0))
  {
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
    return EFI_DEVICE_ERROR;
  }

  DataSegLen = Response->DataSegLen;
  SwapLittleEndianToBigEndian ((UINT8 *)&DataSegLen, sizeof (UINT16));
  SenseDataLen = 0;
  // With an empty segment the bytes after the basic response header are not
  // received sense data. A GOOD response may legally include HPB information;
  // validate its wire lengths independently of the caller's sense allocation.
  if (DataSegLen != 0) {
    SenseDataLen = Response->SenseDataLen;
    SwapLittleEndianToBigEndian ((UINT8 *)&SenseDataLen, sizeof (UINT16));
  }

  if ((Response->EhsLen != 0) ||
      ((DataSegLen != 0) &&
       ((DataSegLen < sizeof (Response->SenseDataLen)) ||
        (DataSegLen > sizeof (Response->SenseDataLen) + sizeof (Response->SenseData)) ||
        (SenseDataLen != DataSegLen - sizeof (Response->SenseDataLen))))) {
    Packet->SenseDataLength   = 0;
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
    return EFI_DEVICE_ERROR;
  }

  Packet->TargetStatus = Response->Status;
  if ((Response->Response != 0) || (TransReq->Trd->Ocs != 0)) {
    return EFI_DEVICE_ERROR;
  }

  if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ) {
    Requested = Packet->InTransferLength;
  } else if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_WRITE) {
    Requested = Packet->OutTransferLength;
  } else {
    Requested = 0;
  }

  if ((Response->Flags & BIT5) != 0) {
    if (Requested == 0) {
      Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
      return EFI_DEVICE_ERROR;
    }

    Residual = Response->ResTranCount;
    SwapLittleEndianToBigEndian ((UINT8 *)&Residual, sizeof (UINT32));
    if (Residual > Requested) {
      Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
      return EFI_DEVICE_ERROR;
    }

    if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ) {
      Packet->InTransferLength = Requested - Residual;
    } else if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_WRITE) {
      Packet->OutTransferLength = Requested - Residual;
    }
  }

  if (Packet->SenseDataLength != 0) {
    if ((Packet->SenseData != NULL) && (SenseDataLen <= Packet->SenseDataLength)) {
      if (SenseDataLen != 0) {
        CopyMem (Packet->SenseData, Response->SenseData, SenseDataLen);
      }

      Packet->SenseDataLength = (UINT8)SenseDataLen;
    } else {
      Packet->SenseDataLength = 0;
    }
  }

  *CopyReadData = (BOOLEAN)(Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ);
  return EFI_SUCCESS;
}

/**
  Sends a UFS-supported SCSI Request Packet to a UFS device that is attached to the UFS host controller.

  @param[in]      Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]      Lun           The LUN of the UFS device to send the SCSI Request Packet.
  @param[in, out] Packet        A pointer to the SCSI Request Packet to send to a specified Lun of the
                                UFS device.
  @param[in]      Event         If nonblocking I/O is not supported then Event is ignored, and blocking
                                I/O is performed. If Event is NULL, then blocking I/O is performed. If
                                Event is not NULL and non blocking I/O is supported, then
                                nonblocking I/O is performed, and Event will be signaled when the
                                SCSI Request Packet completes.

  @retval EFI_SUCCESS           The SCSI Request Packet was sent by the host. For bi-directional
                                commands, InTransferLength bytes were transferred from
                                InDataBuffer. For write and bi-directional commands,
                                OutTransferLength bytes were transferred by
                                OutDataBuffer.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to send the SCSI Request
                                Packet.
  @retval EFI_OUT_OF_RESOURCES  The resource for transfer is not available.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the SCSI Request Packet to execute.

**/
STATIC
EFI_STATUS
UfsExecScsiCmdsInternal (
  IN     UFS_PASS_THRU_PRIVATE_DATA                  *Private,
  IN     UINT8                                       Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet,
  IN     EFI_EVENT                                   Event    OPTIONAL,
  OUT    BOOLEAN                                    *CommandCompleted OPTIONAL
  )
{
  EFI_STATUS                          Status;
  EFI_STATUS                          CleanupStatus;
  EFI_STATUS                          OriginalStatus;
  UFS_PASS_THRU_TRANS_REQ             *TransReq;
  UINT32                              DataLen;
  UINTN                               PrdtNumber;
  UINTN                               RequestSize;
  BOOLEAN                             CopyReadData;

  if ((Private == NULL) || (Packet == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (CommandCompleted != NULL) {
    *CommandCompleted = FALSE;
  }

  Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK;
  CopyReadData = FALSE;
  if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ) {
    DataLen = Packet->InTransferLength;
  } else if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_WRITE) {
    DataLen = Packet->OutTransferLength;
  } else {
    return EFI_INVALID_PARAMETER;
  }

  PrdtNumber = (UINTN)DivU64x32 ((UINT64)DataLen + UFS_MAX_DATA_LEN_PER_PRD - 1, UFS_MAX_DATA_LEN_PER_PRD);
  RequestSize = ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)) + ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)) + PrdtNumber * sizeof (UTP_TR_PRD);

  TransReq = AllocateZeroPool (sizeof (*TransReq));
  if (TransReq == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  TransReq->Signature = UFS_PASS_THRU_TRANS_REQ_SIG;
  InitializeListHead (&TransReq->TransferList);
  TransReq->TimeoutRemain = Packet->Timeout;
  TransReq->Packet        = Packet;
  TransReq->CallerEvent   = NULL;
  TransReq->Slot          = MAX_UINT8;
  TransReq->CmdDescSize   = (UINT32)RequestSize;

  Status = UfsReserveAvailableTrlSlot (Private, TransReq);
  if (EFI_ERROR (Status)) {
    FreePool (TransReq);
    return Status;
  }

  TransReq->Trd = ((UTP_TRD *)Private->UtpTrlBase) + TransReq->Slot;
  Status = UfsCreateScsiCommandDesc (
             Private,
             Lun,
             Packet,
             TransReq->Trd,
             &TransReq->CmdDescHost,
             &TransReq->CmdDescMapping
             );
  if (EFI_ERROR (Status)) {
    TransReq->Packet = NULL;
    CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
    if (EFI_ERROR (CleanupStatus)) {
      UfsRetainOrFreeTransferRequest (Private, TransReq);
    } else {
      UfsUnqueueTransferRequest (Private, TransReq);
      FreePool (TransReq);
    }

    return Status;
  }

  Status = UfsPrepareDataTransferBuffer (Private, TransReq);
  if (EFI_ERROR (Status)) {
    TransReq->Packet = NULL;
    CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
    if (EFI_ERROR (CleanupStatus)) {
      UfsRetainOrFreeTransferRequest (Private, TransReq);
    } else {
      UfsUnqueueTransferRequest (Private, TransReq);
      FreePool (TransReq);
    }

    return Status;
  }

  if ((Event == NULL) && Private->Stopping && Private->StopBusy &&
      (Lun == UFS_WLUN_UFS_DEV) && (Packet->Cdb != NULL) &&
      (Packet->CdbLength == 6))
  {
    CONST UINT8 PowerDownCdb[6] = { 0x1b, 0, 0, 0, 0x30, 0 };
    TransReq->InternalPowerDown = (BOOLEAN)(CompareMem (Packet->Cdb, PowerDownCdb,
                                                       sizeof (PowerDownCdb)) == 0);
  }

  if (TransReq->InternalPowerDown) {
    // A doorbell write may reach the controller even when its provider returns
    // an error. Retain the response and DMA ownership until the outcome is known.
    Private->DevicePowerDownUncertain = TRUE;
  }

  Status = UfsStartExecCmd (Private, TransReq->Slot, &TransReq->DoorbellAttempted);
  if (EFI_ERROR (Status)) {
    goto AbortRequest;
  }

  if (Event != NULL) {
    EFI_TPL  OldTpl;

    OldTpl                = gBS->RaiseTPL (TPL_NOTIFY);
    TransReq->Packet      = Packet;
    TransReq->CallerEvent = Event;
    gBS->RestoreTPL (OldTpl);
    return EFI_SUCCESS;
  }

  Status = UfsWaitMemSet (Private, UFS_HC_UTRLDBR_OFFSET, ((UINT32)1U << TransReq->Slot), 0, Packet->Timeout);
  if (EFI_ERROR (Status)) {
    Packet->HostAdapterStatus = (Status == EFI_TIMEOUT) ?
                                EFI_EXT_SCSI_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND :
                                EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
    goto AbortRequest;
  }

  Status = Private->UfsHostController->Flush (Private->UfsHostController);
  if (EFI_ERROR (Status)) {
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
    TransReq->Packet = NULL;
    return Status;
  }

  Status = UfsValidateScsiResponse (TransReq, &CopyReadData);
  if (TransReq->InternalPowerDown) {
    if (EFI_ERROR (Status)) {
      // An unreadable/invalid response cannot prove whether POWERDOWN happened.
      // Keep it for a later synchronized read; never cancel or resend blindly.
      TransReq->Packet = NULL;
      return Status;
    }

    TransReq->PowerDownResponseKnown = TRUE;
    Private->DevicePowerDownUncertain = FALSE;
    if (Packet->TargetStatus == EFI_EXT_SCSI_STATUS_TARGET_GOOD) {
      Private->DevicePowerDownComplete = TRUE;
    }
  }

  if (!EFI_ERROR (Status) && (CommandCompleted != NULL) &&
      (Packet->TargetStatus == EFI_EXT_SCSI_STATUS_TARGET_GOOD))
  {
    // Keep the device response separate from later Unmap/FreeBuffer failures.
    // A successful SSU must never be sent again merely because DMA retirement
    // failed after the device already entered POWERDOWN.
    *CommandCompleted = TRUE;
  }

  if (EFI_ERROR (Status) && (Packet->HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK)) {
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
  }

  CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, (BOOLEAN)(!EFI_ERROR (Status) && CopyReadData));
  if (EFI_ERROR (CleanupStatus)) {
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
    TransReq->Packet = NULL;
    return EFI_ERROR (Status) ? Status : CleanupStatus;
  }

  UfsUnqueueTransferRequest (Private, TransReq);
  FreePool (TransReq);
  return Status;

AbortRequest:
  OriginalStatus = Status;
  if (TransReq->InternalPowerDown) {
    // Timeout may race a natural completion. UTRLCLR/HCE reset would destroy
    // the only response that can distinguish acceptance from cancellation.
    Packet->HostAdapterStatus = (OriginalStatus == EFI_TIMEOUT) ?
                                EFI_EXT_SCSI_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND :
                                EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
    TransReq->Packet = NULL;
    TransReq->CallerEvent = NULL;
    return OriginalStatus;
  }

  CleanupStatus = UfsStopExecCmd (Private, TransReq->Slot);
  if (!EFI_ERROR (CleanupStatus)) {
    CleanupStatus = Private->UfsHostController->Flush (Private->UfsHostController);
    if (!EFI_ERROR (CleanupStatus)) {
      CleanupStatus = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
      if (!EFI_ERROR (CleanupStatus)) {
        UfsUnqueueTransferRequest (Private, TransReq);
        FreePool (TransReq);
        return OriginalStatus;
      }
    }
  }

  Packet->HostAdapterStatus = (OriginalStatus == EFI_TIMEOUT) ?
                              EFI_EXT_SCSI_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND :
                              EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
  TransReq->Packet      = NULL;
  TransReq->CallerEvent = NULL;
  UfsRetainOrFreeTransferRequest (Private, TransReq);
  return OriginalStatus;
}

EFI_STATUS
UfsExecScsiCmds (
  IN     UFS_PASS_THRU_PRIVATE_DATA                  *Private,
  IN     UINT8                                       Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet,
  IN     EFI_EVENT                                   Event OPTIONAL
  )
{
  return UfsExecScsiCmdsInternal (Private, Lun, Packet, Event, NULL);
}

/**
  Send UIC command.

  @param[in]      Private     The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in, out] UicCommand  UIC command descriptor. On exit contains UIC command results.

  @return EFI_SUCCESS      Successfully execute this UIC command and detect attached UFS device.
  @return EFI_DEVICE_ERROR Fail to execute this UIC command and detect attached UFS device.

**/
EFI_STATUS
UfsExecUicCommands (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN OUT EDKII_UIC_COMMAND        *UicCommand
  )
{
  EFI_STATUS  Status;
  UINT32      Data;

  Status = UfsMmioRead32 (Private, UFS_HC_IS_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Data & UFS_HC_IS_UCCS) == UFS_HC_IS_UCCS) {
    //
    // Clear IS.BIT10 UIC Command Completion Status (UCCS) at first.
    //
    Status = UfsMmioWrite32 (Private, UFS_HC_IS_OFFSET, Data);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  //
  // When programming UIC command registers, host software shall set the register UICCMD
  // only after all the UIC command argument registers (UICCMDARG1, UICCMDARG2 and UICCMDARG3)
  // are set.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UCMD_ARG1_OFFSET, UicCommand->Arg1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UCMD_ARG2_OFFSET, UicCommand->Arg2);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UCMD_ARG3_OFFSET, UicCommand->Arg3);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Host software shall only set the UICCMD if HCS.UCRDY is set to 1.
  //
  Status = UfsWaitMemSet (Private, UFS_HC_STATUS_OFFSET, UFS_HC_HCS_UCRDY, UFS_HC_HCS_UCRDY, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UIC_CMD_OFFSET, UicCommand->Opcode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // UFS 2.0 spec section 5.3.1 Offset:0x20 IS.Bit10 UIC Command Completion Status (UCCS)
  // This bit is set to '1' by the host controller upon completion of a UIC command.
  //
  Status = UfsWaitMemSet (Private, UFS_HC_IS_OFFSET, UFS_HC_IS_UCCS, UFS_HC_IS_UCCS, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (UicCommand->Opcode != UfsUicDmeReset) {
    Status = UfsMmioRead32 (Private, UFS_HC_UCMD_ARG2_OFFSET, &UicCommand->Arg2);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = UfsMmioRead32 (Private, UFS_HC_UCMD_ARG3_OFFSET, &UicCommand->Arg3);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((UicCommand->Arg2 & 0xFF) != 0) {
      DEBUG_CODE_BEGIN ();
      DumpUicCmdExecResult ((UINT8)UicCommand->Opcode, (UINT8)(UicCommand->Arg2 & 0xFF));
      DEBUG_CODE_END ();
      return EFI_DEVICE_ERROR;
    }
  }

  return EFI_SUCCESS;
}

/**
  Allocate common buffer for host and UFS bus master access simultaneously.

  @param[in]  Private                The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Size                   The length of buffer to be allocated.
  @param[out] CmdDescHost            A pointer to store the base system memory address of the allocated range.
  @param[out] CmdDescPhyAddr         The resulting map address for the UFS bus master to use to access the hosts CmdDescHost.
  @param[out] CmdDescMapping         A resulting value to pass to Unmap().

  @retval EFI_SUCCESS                The common buffer was allocated successfully.
  @retval EFI_DEVICE_ERROR           The allocation fails.
  @retval EFI_OUT_OF_RESOURCES       The memory resource is insufficient.

**/
EFI_STATUS
UfsAllocateAlignCommonBuffer (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     UINTN                       Size,
  OUT VOID                           **CmdDescHost,
  OUT EFI_PHYSICAL_ADDRESS           *CmdDescPhyAddr,
  OUT VOID                           **CmdDescMapping
  )
{
  EFI_STATUS                          Status;
  EFI_STATUS                          CleanupStatus;
  UINTN                               Bytes;
  UINTN                               AllocatedBytes;
  BOOLEAN                             Is32BitAddr;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;

  if ((Private == NULL) || (CmdDescHost == NULL) || (CmdDescPhyAddr == NULL) || (CmdDescMapping == NULL) || (Size == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *CmdDescHost    = NULL;
  *CmdDescPhyAddr = 0;
  *CmdDescMapping = NULL;
  AllocatedBytes  = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (Size));
  Is32BitAddr     = ((Private->UfsHcInfo.Capabilities & UFS_HC_CAP_64ADDR) == 0);
  UfsHc           = Private->UfsHostController;

  Status = UfsHc->AllocateBuffer (
                    UfsHc,
                    AllocateAnyPages,
                    EfiBootServicesData,
                    EFI_SIZE_TO_PAGES (Size),
                    CmdDescHost,
                    0
                    );
  if (EFI_ERROR (Status)) {
    *CmdDescHost = NULL;
    return Status;
  }

  Bytes = AllocatedBytes;
  Status = UfsHc->Map (
                    UfsHc,
                    EdkiiUfsHcOperationBusMasterCommonBuffer,
                    *CmdDescHost,
                    &Bytes,
                    CmdDescPhyAddr,
                    CmdDescMapping
                    );

  if (EFI_ERROR (Status) || (Bytes != AllocatedBytes) || (*CmdDescMapping == NULL)) {
    if (*CmdDescMapping != NULL) {
      CleanupStatus = UfsHc->Unmap (UfsHc, *CmdDescMapping);
      if (EFI_ERROR (CleanupStatus)) {
        return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
      }

      *CmdDescMapping = NULL;
    }

    CleanupStatus = UfsHc->FreeBuffer (UfsHc, EFI_SIZE_TO_PAGES (Size), *CmdDescHost);
    if (EFI_ERROR (CleanupStatus)) {
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }

    *CmdDescHost    = NULL;
    *CmdDescPhyAddr = 0;
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  if (Is32BitAddr && ((*CmdDescPhyAddr >= 0x100000000ULL) || (Bytes > (0x100000000ULL - *CmdDescPhyAddr)))) {
    CleanupStatus = UfsHc->Unmap (UfsHc, *CmdDescMapping);
    if (EFI_ERROR (CleanupStatus)) {
      return EFI_DEVICE_ERROR;
    }

    *CmdDescMapping = NULL;
    CleanupStatus   = UfsHc->FreeBuffer (UfsHc, EFI_SIZE_TO_PAGES (Size), *CmdDescHost);
    if (EFI_ERROR (CleanupStatus)) {
      return EFI_DEVICE_ERROR;
    }

    *CmdDescHost    = NULL;
    *CmdDescPhyAddr = 0;
    return EFI_DEVICE_ERROR;
  }

  ZeroMem (*CmdDescHost, AllocatedBytes);
  return EFI_SUCCESS;
}

/**
  Enable the UFS host controller for accessing.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The UFS host controller enabling was executed successfully.
  @retval EFI_DEVICE_ERROR           A device error occurred while enabling the UFS host controller.

**/
EFI_STATUS
UfsEnableHostController (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINT32      Data;

  if ((Private->UfsHcPlatform != NULL) && (Private->UfsHcPlatform->Callback != NULL)) {
    Status = Private->UfsHcPlatform->Callback (Private->Handle, EdkiiUfsHcPreHce, &Private->UfsHcDriverInterface);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failure from platform driver during EdkiiUfsHcPreHce, Status = %r\n", Status));
      return Status;
    }
  }

  if ((Private->UfsHcPlatform == NULL) ||
      ((Private->UfsHcPlatform->Version >= 3) && !Private->UfsHcPlatform->SkipHceReenable))
  {
    //
    // UFS 2.0 spec section 7.1.1 - Host Controller Initialization
    //
    // Reinitialize the UFS host controller if HCE bit of HC register is set.
    //
    Status = UfsMmioRead32 (Private, UFS_HC_ENABLE_OFFSET, &Data);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Data & UFS_HC_HCE_EN) == UFS_HC_HCE_EN) {
      //
      // Write a 0 to the HCE register at first to disable the host controller.
      //
      Status = UfsMmioWrite32 (Private, UFS_HC_ENABLE_OFFSET, 0);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      //
      // Wait until HCE is read as '0' before continuing.
      //
      Status = UfsWaitMemSet (Private, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN, 0, UFS_TIMEOUT);
      if (EFI_ERROR (Status)) {
        return EFI_DEVICE_ERROR;
      }
    }

    //
    // Write a 1 to the HCE register to enable the UFS host controller.
    //
    Status = UfsMmioWrite32 (Private, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  //
  // Wait until HCE is read as '1' before continuing.
  //
  Status = UfsWaitMemSet (Private, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN, UFS_HC_HCE_EN, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  if ((Private->UfsHcPlatform != NULL) && (Private->UfsHcPlatform->Callback != NULL)) {
    Status = Private->UfsHcPlatform->Callback (Private->Handle, EdkiiUfsHcPostHce, &Private->UfsHcDriverInterface);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failure from platform driver during EdkiiUfsHcPostHce, Status = %r\n", Status));
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  Detect if a UFS device attached.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The UFS device detection was executed successfully.
  @retval EFI_NOT_FOUND              Not found a UFS device attached.
  @retval EFI_DEVICE_ERROR           A device error occurred while detecting the UFS device.

**/
EFI_STATUS
UfsDeviceDetection (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  UINTN              Retry;
  EFI_STATUS         Status;
  UINT32             Data;
  EDKII_UIC_COMMAND  LinkStartupCommand;

  if ((Private->UfsHcPlatform != NULL) && (Private->UfsHcPlatform->Callback != NULL)) {
    Status = Private->UfsHcPlatform->Callback (Private->Handle, EdkiiUfsHcPreLinkStartup, &Private->UfsHcDriverInterface);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failure from platform driver during EdkiiUfsHcPreLinkStartup, Status = %r\n", Status));
      return Status;
    }
  }

  if ((Private->UfsHcPlatform != NULL) && (Private->UfsHcPlatform->Version >= 3) && Private->UfsHcPlatform->SkipLinkStartup) {
    return EFI_SUCCESS;
  }

  //
  // Start UFS device detection.
  // Try up to 3 times for establishing data link with device.
  //
  for (Retry = 0; Retry < 3; Retry++) {
    LinkStartupCommand.Opcode = UfsUicDmeLinkStartup;
    LinkStartupCommand.Arg1   = 0;
    LinkStartupCommand.Arg2   = 0;
    LinkStartupCommand.Arg3   = 0;
    Status                    = UfsExecUicCommands (Private, &LinkStartupCommand);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    Status = UfsMmioRead32 (Private, UFS_HC_STATUS_OFFSET, &Data);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    if ((Data & UFS_HC_HCS_DP) == 0) {
      Status = UfsWaitMemSet (Private, UFS_HC_IS_OFFSET, UFS_HC_IS_ULSS, UFS_HC_IS_ULSS, UFS_TIMEOUT);
      if (EFI_ERROR (Status)) {
        return EFI_DEVICE_ERROR;
      }
    } else {
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

/**
  Initialize UFS task management request list related h/w context.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The UFS task management list was initialzed successfully.
  @retval EFI_DEVICE_ERROR           The initialization fails.

**/
EFI_STATUS
UfsInitTaskManagementRequestList (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  UINT8                 Nutmrs;
  VOID                  *CmdDescHost;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;
  VOID                  *CmdDescMapping;
  EFI_STATUS            Status;

  //
  // Initial h/w and s/w context for future operations.
  //
  CmdDescHost    = NULL;
  CmdDescMapping = NULL;
  CmdDescPhyAddr = 0;

  //
  // Allocate and initialize UTP Task Management Request List.
  //
  Nutmrs = (UINT8)(RShiftU64 ((Private->UfsHcInfo.Capabilities & UFS_HC_CAP_NUTMRS), 16) + 1);
  Status = UfsAllocateAlignCommonBuffer (Private, Nutmrs * sizeof (UTP_TMRD), &CmdDescHost, &CmdDescPhyAddr, &CmdDescMapping);
  Private->UtpTmrlBase = CmdDescHost;
  Private->Nutmrs = Nutmrs;
  Private->TmrlMapping = CmdDescMapping;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Program the UTP Task Management Request List Base Address and UTP Task Management
  // Request List Base Address with a 64-bit address allocated at step 6.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTMRLBA_OFFSET, (UINT32)(UINTN)CmdDescPhyAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UTMRLBAU_OFFSET, (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->UtpTmrlBase = CmdDescHost;
  Private->Nutmrs      = Nutmrs;
  Private->TmrlMapping = CmdDescMapping;

  //
  // Enable the UTP Task Management Request List by setting the UTP Task Management
  // Request List RunStop Register (UTMRLRSR) to '1'.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTMRLRSR_OFFSET, UFS_HC_UTMRLRSR);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Initialize UFS transfer request list related h/w context.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The UFS transfer list was initialzed successfully.
  @retval EFI_DEVICE_ERROR           The initialization fails.

**/
EFI_STATUS
UfsInitTransferRequestList (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  UINT8                 Nutrs;
  VOID                  *CmdDescHost;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;
  VOID                  *CmdDescMapping;
  EFI_STATUS            Status;

  //
  // Initial h/w and s/w context for future operations.
  //
  CmdDescHost    = NULL;
  CmdDescMapping = NULL;
  CmdDescPhyAddr = 0;

  //
  // Allocate and initialize UTP Transfer Request List.
  //
  Nutrs  = (UINT8)((Private->UfsHcInfo.Capabilities & UFS_HC_CAP_NUTRS) + 1);
  Status = UfsAllocateAlignCommonBuffer (Private, Nutrs * sizeof (UTP_TRD), &CmdDescHost, &CmdDescPhyAddr, &CmdDescMapping);
  Private->UtpTrlBase = CmdDescHost;
  Private->Nutrs = Nutrs;
  Private->TrlMapping = CmdDescMapping;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Program the UTP Transfer Request List Base Address and UTP Transfer Request List
  // Base Address with a 64-bit address allocated at step 8.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLBA_OFFSET, (UINT32)(UINTN)CmdDescPhyAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLBAU_OFFSET, (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->UtpTrlBase = CmdDescHost;
  Private->Nutrs      = Nutrs;
  Private->TrlMapping = CmdDescMapping;

  //
  // Enable the UTP Transfer Request List by setting the UTP Transfer Request List
  // RunStop Register (UTRLRSR) to '1'.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTRLRSR);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Initialize the UFS host controller.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The Ufs Host Controller is initialized successfully.
  @retval Others                     A device error occurred while initializing the controller.

**/
EFI_STATUS
UfsControllerInit (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;

  Status = UfsEnableHostController (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsControllerInit: Enable Host Controller Fails, Status = %r\n", Status));
    return Status;
  }

  Status = UfsDeviceDetection (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsControllerInit: Device Detection Fails, Status = %r\n", Status));
    return Status;
  }

  Status = UfsInitTaskManagementRequestList (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsControllerInit: Task management list initialization Fails, Status = %r\n", Status));
    return Status;
  }

  Status = UfsInitTransferRequestList (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsControllerInit: Transfer list initialization Fails, Status = %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "UfsControllerInit Finished\n"));
  return EFI_SUCCESS;
}

/**
  Stop the UFS host controller.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The Ufs Host Controller is stopped successfully.
  @retval Others                     A device error occurred while stopping the controller.

**/
EFI_STATUS
UfsControllerStop (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINT32      Data;
  UINTN       Index;
  UINTN       BaseOffsets[4];

  if (Private == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Private->ControllerStopComplete = FALSE;

  // Stop task management commands first, then transfer commands. Every write is read back.
  Status = UfsMmioRead32 (Private, UFS_HC_UTMRLRSR_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data == MAX_UINT32)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  if ((Data & UFS_HC_UTMRLRSR) != 0) {
    Status = UfsMmioWrite32 (Private, UFS_HC_UTMRLRSR_OFFSET, 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = UfsWaitMemSet (Private, UFS_HC_UTMRLRSR_OFFSET, UFS_HC_UTMRLRSR, 0, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTMRLRSR_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data != 0)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTRLRSR_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data == MAX_UINT32)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  if ((Data & UFS_HC_UTRLRSR) != 0) {
    Status = UfsMmioWrite32 (Private, UFS_HC_UTRLRSR_OFFSET, 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = UfsWaitMemSet (Private, UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTRLRSR, 0, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTRLRSR_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data != 0)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_ENABLE_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data == MAX_UINT32)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  if ((Data & UFS_HC_HCE_EN) != 0) {
    Status = UfsMmioWrite32 (Private, UFS_HC_ENABLE_OFFSET, Data & ~UFS_HC_HCE_EN);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = UfsWaitMemSet (Private, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN, 0, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_ENABLE_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data != 0)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data != 0)) {
    return EFI_ERROR (Status) ? Status : ((Data == MAX_UINT32) ? EFI_DEVICE_ERROR : EFI_NOT_READY);
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTMRLDBR_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data != 0)) {
    return EFI_ERROR (Status) ? Status : ((Data == MAX_UINT32) ? EFI_DEVICE_ERROR : EFI_NOT_READY);
  }

  BaseOffsets[0] = UFS_HC_UTRLBA_OFFSET;
  BaseOffsets[1] = UFS_HC_UTRLBAU_OFFSET;
  BaseOffsets[2] = UFS_HC_UTMRLBA_OFFSET;
  BaseOffsets[3] = UFS_HC_UTMRLBAU_OFFSET;
  for (Index = 0; Index < ARRAY_SIZE (BaseOffsets); Index++) {
    Status = UfsMmioWrite32 (Private, BaseOffsets[Index], 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = UfsMmioRead32 (Private, BaseOffsets[Index], &Data);
    if (EFI_ERROR (Status) || (Data != 0)) {
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_IE_OFFSET, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_IE_OFFSET, &Data);
  if (EFI_ERROR (Status) || (Data != 0)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Private->ControllerStopComplete = TRUE;
  DEBUG ((DEBUG_INFO, "UfsController is stopped and request DMA is quiesced\n"));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
UfsFinishAsyncRequest (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN UFS_PASS_THRU_TRANS_REQ     *TransReq,
  IN BOOLEAN                    CopyReadData
  )
{
  EFI_STATUS                                  Status;
  EFI_STATUS                                  SignalStatus;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet;
  EFI_EVENT                                   CallerEvent;

  Packet      = TransReq->Packet;
  CallerEvent = TransReq->CallerEvent;
  if (CallerEvent == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = UfsReleaseTransferRequestResources (Private, TransReq, CopyReadData);
  if (EFI_ERROR (Status) && (Packet != NULL)) {
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
  }

  // Detach before signaling so a reentrant callback cannot observe half-released state.
  TransReq->Packet      = NULL;
  TransReq->CallerEvent = NULL;
  SignalStatus = gBS->SignalEvent (CallerEvent);
  if (EFI_ERROR (SignalStatus)) {
    TransReq->Packet      = Packet;
    TransReq->CallerEvent = CallerEvent;
    return SignalStatus;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UfsUnqueueTransferRequest (Private, TransReq);
  FreePool (TransReq);
  return EFI_SUCCESS;
}

/**
  Internal helper function which will signal the caller event and clean up
  resources.

  @param[in] Private   The pointer to the UFS_PASS_THRU_PRIVATE_DATA data
                       structure.
  @param[in] TransReq  The pointer to the UFS_PASS_THRU_TRANS_REQ data
                       structure.

**/
VOID
EFIAPI
SignalCallerEvent (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN UFS_PASS_THRU_TRANS_REQ     *TransReq
  )
{
  EFI_STATUS  Status;

  if ((Private == NULL) || (TransReq == NULL) || (TransReq->CallerEvent == NULL)) {
    return;
  }

  Status = UfsFinishAsyncRequest (Private, TransReq, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to safely complete async UFS request: %r\n", Status));
  }
}

/**
  Call back function when the timer event is signaled.

  @param[in]  Event     The Event this notify function registered to.
  @param[in]  Context   Pointer to the context data registered to the Event.

**/
STATIC
VOID
UfsProcessAsyncTasks (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN BOOLEAN                     FromStop
  )
{
  LIST_ENTRY                                  *Entry;
  LIST_ENTRY                                  *NextEntry;
  UFS_PASS_THRU_TRANS_REQ                     *TransReq;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet;
  UINT32                                      SlotsMap;
  UINT32                                      Value;
  BOOLEAN                                     CopyReadData;
  EFI_STATUS                                  Status;

  if (Private == NULL) {
    return;
  }

  if (FromStop) {
    // The cleanup caller pins Private through StopBusy. Public entry points and
    // timer callbacks remain closed, including reentry from SignalEvent.
    if (!Private->Stopping || !Private->StopBusy || (Private->ActiveCalls != 0)) {
      return;
    }
  } else if (EFI_ERROR (UfsEnterCall (Private, FALSE, TRUE))) {
    return;
  }

  SlotsMap = 0;
  if (!IsListEmpty (&Private->Queue)) {
    BASE_LIST_FOR_EACH_SAFE (Entry, NextEntry, &Private->Queue) {
      TransReq = UFS_PASS_THRU_TRANS_REQ_FROM_THIS (Entry);
      Packet   = TransReq->Packet;

      // A failed synchronous request may retain its DMA owners after its
      // caller's stack is gone. Stop may retire those owners only after a
      // fresh doorbell-zero observation and a successful host DMA flush.
      if ((Packet == NULL) || (TransReq->CallerEvent == NULL)) {
        if (FromStop && (Packet == NULL) && (TransReq->CallerEvent == NULL) &&
            (TransReq->Slot < 32))
        {
          Status = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Value);
          if (!EFI_ERROR (Status) && (Value != MAX_UINT32) &&
              ((Value & ((UINT32)1U << TransReq->Slot)) == 0))
          {
            Status = Private->UfsHostController->Flush (Private->UfsHostController);
            if (!EFI_ERROR (Status)) {
              if (TransReq->InternalPowerDown && !TransReq->DoorbellAttempted) {
                // RSR/read failures precede the doorbell write. With DBR zero
                // and a successful DMA flush this request was never submitted.
                TransReq->PowerDownResponseKnown = TRUE;
                Private->DevicePowerDownUncertain = FALSE;
              }

              if (TransReq->InternalPowerDown && !TransReq->PowerDownResponseKnown) {
                EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET PowerPacket;
                BOOLEAN CopyData;
                ZeroMem (&PowerPacket, sizeof (PowerPacket));
                PowerPacket.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
                PowerPacket.TargetStatus = MAX_UINT8;
                TransReq->Packet = &PowerPacket;
                Status = UfsValidateScsiResponse (TransReq, &CopyData);
                TransReq->Packet = NULL;
                if (EFI_ERROR (Status)) {
                  continue;
                }

                TransReq->PowerDownResponseKnown = TRUE;
                Private->DevicePowerDownUncertain = FALSE;
                if (PowerPacket.TargetStatus == EFI_EXT_SCSI_STATUS_TARGET_GOOD) {
                  Private->DevicePowerDownComplete = TRUE;
                }
              }

              Status = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
              if (!EFI_ERROR (Status)) {
                UfsUnqueueTransferRequest (Private, TransReq);
                FreePool (TransReq);
              }
            }
          }
        }

        continue;
      }

      if ((TransReq->Slot >= 32) || ((SlotsMap & (((UINT32)1U << TransReq->Slot))) != 0)) {
        Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
        continue;
      }

      SlotsMap |= ((UINT32)1U << TransReq->Slot);
      Status = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Value);
      if (EFI_ERROR (Status)) {
        // TPL_NOTIFY cannot wait for a stop or release resources with uncertain DMA.
        Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
        continue;
      }

      if ((Value & (((UINT32)1U << TransReq->Slot))) != 0) {
        if (Packet->HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK) {
          if (TransReq->TimeoutRemain > UFS_HC_ASYNC_TIMER) {
            TransReq->TimeoutRemain -= UFS_HC_ASYNC_TIMER;
          } else {
            Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND;
          }
        }

        continue;
      }

      // The previous completion released buffers but SignalEvent failed. Retry just that signal.
      if ((TransReq->CmdDescHost == NULL) && (TransReq->CmdDescMapping == NULL) &&
          (TransReq->AlignedDataBuf == NULL) && (TransReq->DataBufMapping == NULL))
      {
        (VOID)UfsFinishAsyncRequest (Private, TransReq, FALSE);
        continue;
      }

      Status = Private->UfsHostController->Flush (Private->UfsHostController);
      if (EFI_ERROR (Status)) {
        Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
        continue;
      }

      if (Packet->HostAdapterStatus != EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK) {
        (VOID)UfsFinishAsyncRequest (Private, TransReq, FALSE);
        continue;
      }

      Status = UfsValidateScsiResponse (TransReq, &CopyReadData);
      if (EFI_ERROR (Status) && (Packet->HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK)) {
        Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
      }

      Status = UfsFinishAsyncRequest (Private, TransReq, (BOOLEAN)(!EFI_ERROR (Status) && CopyReadData));
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_VERBOSE, "ProcessAsyncTaskList(): request completion remains owned: %r\n", Status));
      }
    }
  }

  if (!FromStop) {
    UfsLeaveCall (Private);
  }
}

VOID
EFIAPI
ProcessAsyncTaskList (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Event;
  UfsProcessAsyncTasks ((UFS_PASS_THRU_PRIVATE_DATA *)Context, FALSE);
}

/**
  Prepare the initialized device for an opt-in, supply-retaining provider Stop.
  No device supply is cut by this contract, so power-on write protection stays
  powered and its flag is never cleared. Public SCSI/configuration permissions
  do not change. All commands here are internal to a claimed cleanup attempt.
**/
EFI_STATUS
UfsPrepareDevicePowerDown (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS                                 Status;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  Packet;
  UFS_UNIT_DESC                              Unit[8];
  UINT32                                     UnitSize;
  UINT32                                     Value;
  UINTN                                      Index;
  UINTN                                      Elapsed;
  UINT8                                      Flag;
  UINT8                                      Cdb[10];
  BOOLEAN                                    Completed;

  if ((Private == NULL) || !Private->Stopping || !Private->StopBusy ||
      !Private->DeviceInitializationComplete || (Private->ActiveCalls != 0))
  {
    return EFI_NOT_READY;
  }

  // The timer has been canceled, so a cleanup owner drains completions itself.
  // Never stop HCE to make this drain succeed: a busy/uncertain device keeps
  // its clocks and supplies and the same cleanup owner can retry later.
  for (Elapsed = 0; !IsListEmpty (&Private->Queue); Elapsed += 1000) {
    UfsProcessAsyncTasks (Private, TRUE);
    if (IsListEmpty (&Private->Queue)) {
      break;
    }

    if (Elapsed >= 3000000) {
      return EFI_NOT_READY;
    }

    MicroSecondDelay (1000);
  }

  if ((Private->ReservedTrlSlots != 0) || Private->DevicePowerDownUncertain) {
    return EFI_NOT_READY;
  }

  if (Private->DevicePowerDownComplete) {
    return EFI_SUCCESS;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_ENABLE_OFFSET, &Value);
  if (EFI_ERROR (Status) || (Value != UFS_HC_HCE_EN)) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }

  Status = UfsMmioRead32 (Private, UFS_HC_UTMRLDBR_OFFSET, &Value);
  if (EFI_ERROR (Status) || (Value != 0)) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }

  // Re-read every common-LU descriptor. The public discovery mask can omit a
  // LU whose descriptor query failed during Start and is not a flush proof.
  for (Index = 0; Index < ARRAY_SIZE (Unit); Index++) {
    ZeroMem (&Unit[Index], sizeof (Unit[Index]));
    UnitSize = sizeof (Unit[Index]);
    Status = UfsRwDeviceDesc (Private, TRUE, UfsUnitDesc, (UINT8)Index, 0,
                             &Unit[Index], &UnitSize);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((UnitSize < OFFSET_OF (UFS_UNIT_DESC, LunEn) + sizeof (Unit[Index].LunEn)) ||
        (Unit[Index].Length > UnitSize) ||
        (Unit[Index].Length < OFFSET_OF (UFS_UNIT_DESC, LunEn) + sizeof (Unit[Index].LunEn)) ||
        (Unit[Index].DescType != UfsUnitDesc) || (Unit[Index].UnitIdx != Index) ||
        (Unit[Index].LunEn > 1))
    {
      return EFI_DEVICE_ERROR;
    }
  }

  // Match the UFS core non-runtime shutdown policy. Auto background work must
  // be disabled before device POWERDOWN; write-protection flags are untouched.
  Status = UfsReadFlag (Private, UfsFlagBgOpsEn, &Flag);
  if (EFI_ERROR (Status) || (Flag > 1)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  if (Flag != 0) {
    Flag = 0;
    Status = UfsRwFlags (Private, FALSE, UfsFlagBgOpsEn, &Flag);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = UfsReadFlag (Private, UfsFlagBgOpsEn, &Flag);
    if (EFI_ERROR (Status) || (Flag != 0)) {
      return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    }
  }

  for (Index = 0; Index < ARRAY_SIZE (Unit); Index++) {
    if (Unit[Index].LunEn == 0) {
      continue;
    }

    ZeroMem (Cdb, sizeof (Cdb));
    Cdb[0] = 0x35; // SYNCHRONIZE CACHE(10), all logical blocks.
    ZeroMem (&Packet, sizeof (Packet));
    Packet.Timeout = UFS_TIMEOUT;
    Packet.Cdb = Cdb;
    Packet.CdbLength = sizeof (Cdb);
    Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
    Packet.TargetStatus = MAX_UINT8;
    Completed = FALSE;
    Status = UfsExecScsiCmdsInternal (Private, (UINT8)Index, &Packet, NULL, &Completed);
    if (EFI_ERROR (Status) || !Completed) {
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
  }

  ZeroMem (Cdb, sizeof (Cdb));
  Cdb[0] = 0x1B; // START STOP UNIT to device WLUN, POWERDOWN condition.
  Cdb[4] = 0x30;
  ZeroMem (&Packet, sizeof (Packet));
  Packet.Timeout = UFS_TIMEOUT;
  Packet.Cdb = Cdb;
  Packet.CdbLength = 6;
  Packet.DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ;
  Packet.TargetStatus = MAX_UINT8;
  Completed = FALSE;
  Status = UfsExecScsiCmdsInternal (Private, UFS_WLUN_UFS_DEV, &Packet, NULL, &Completed);
  if (Completed) {
    Private->DevicePowerDownComplete = TRUE;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Completed ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

EFI_STATUS
UfsAbortAsyncRequests (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  LIST_ENTRY                                  *Entry;
  LIST_ENTRY                                  *NextEntry;
  UFS_PASS_THRU_TRANS_REQ                     *TransReq;
  EFI_STATUS                                  Status;
  EFI_STATUS                                  FirstError;
  EFI_STATUS                                  SignalStatus;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet;
  EFI_EVENT                                   CallerEvent;

  if ((Private == NULL) || !Private->ControllerStopComplete) {
    return EFI_NOT_READY;
  }

  FirstError = EFI_SUCCESS;
  BASE_LIST_FOR_EACH_SAFE (Entry, NextEntry, &Private->Queue) {
    TransReq    = UFS_PASS_THRU_TRANS_REQ_FROM_THIS (Entry);
    Packet      = TransReq->Packet;
    CallerEvent = TransReq->CallerEvent;

    // The controller stop proof is necessary but the cache operation must also succeed.
    Status = Private->UfsHostController->Flush (Private->UfsHostController);
    if (EFI_ERROR (Status)) {
      if (!EFI_ERROR (FirstError)) {
        FirstError = Status;
      }

      continue;
    }

    Status = UfsReleaseTransferRequestResources (Private, TransReq, FALSE);
    if (CallerEvent != NULL) {
      if ((Packet != NULL) && (Packet->HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK)) {
        Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
      }

      TransReq->Packet      = NULL;
      TransReq->CallerEvent = NULL;
      SignalStatus = gBS->SignalEvent (CallerEvent);
      if (EFI_ERROR (SignalStatus)) {
        TransReq->Packet      = Packet;
        TransReq->CallerEvent = CallerEvent;
        if (!EFI_ERROR (FirstError)) {
          FirstError = SignalStatus;
        }

        continue;
      }
    } else {
      TransReq->Packet = NULL;
    }

    if (EFI_ERROR (Status)) {
      if (!EFI_ERROR (FirstError)) {
        FirstError = Status;
      }

      continue;
    }

    UfsUnqueueTransferRequest (Private, TransReq);
    FreePool (TransReq);
  }

  return FirstError;
}

/**
  Execute UIC command.

  @param[in]      This        Pointer to driver interface produced by the UFS controller.
  @param[in, out] UicCommand  Descriptor of the command that will be executed.

  @retval EFI_SUCCESS            Command executed successfully.
  @retval EFI_INVALID_PARAMETER  This or UicCommand is NULL.
  @retval Others                 Command failed to execute.
**/
EFI_STATUS
EFIAPI
UfsHcDriverInterfaceExecUicCommand (
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *This,
  IN OUT EDKII_UIC_COMMAND              *UicCommand
  )
{
  UFS_PASS_THRU_PRIVATE_DATA  *Private;
  EFI_STATUS                  Status;
  EFI_TPL                     OldTpl;
  BOOLEAN                     StopCommand;

  if ((This == NULL) || (UicCommand == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (EfiGetCurrentTpl () > TPL_CALLBACK) {
    return EFI_NOT_READY;
  }

  Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_DRIVER_INTF (This);
  StopCommand = (BOOLEAN)((UicCommand->Opcode == UfsUicDmeHibernateEnter) ||
                          (UicCommand->Opcode == UfsUicDmeHibernateExit) ||
                          (UicCommand->Opcode == UfsUicDmeGet) ||
                          (UicCommand->Opcode == UfsUicDmeSet));
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Private->Stopping &&
      !(Private->StopUicAllowed && Private->StopBusy &&
        Private->DevicePowerDownComplete && !Private->DevicePowerDownUncertain &&
        !Private->ControllerStopComplete && (Private->ActiveCalls == 0) && StopCommand))
  {
    Status = EFI_NOT_READY;
  } else if (Private->ActiveCalls == MAX_UINTN) {
    Status = EFI_OUT_OF_RESOURCES;
  } else {
    Private->ActiveCalls++;
    Status = EFI_SUCCESS;
  }
  gBS->RestoreTPL (OldTpl);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsExecUicCommands (Private, UicCommand);
  UfsLeaveCall (Private);
  return Status;
}

/**
  Initializes UfsHcInfo field in private data.

  @param[in] Private  Pointer to host controller private data.

  @retval EFI_SUCCESS  UfsHcInfo initialized successfully.
  @retval Others       Failed to initalize UfsHcInfo.
**/
EFI_STATUS
GetUfsHcInfo (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  UINT32      Data;
  EFI_STATUS  Status;

  Status = UfsMmioRead32 (Private, UFS_HC_VER_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->UfsHcInfo.Version = Data;

  Status = UfsMmioRead32 (Private, UFS_HC_CAP_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->UfsHcInfo.Capabilities = Data;

  if ((Private->UfsHcPlatform != NULL) && (Private->UfsHcPlatform->OverrideHcInfo != NULL)) {
    Status = Private->UfsHcPlatform->OverrideHcInfo (Private->Handle, &Private->UfsHcInfo);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failure from platform on OverrideHcInfo, Status = %r\n", Status));
      return Status;
    }
  }

  return EFI_SUCCESS;
}
