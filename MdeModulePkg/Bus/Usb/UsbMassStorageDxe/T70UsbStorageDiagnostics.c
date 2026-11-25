/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbMass.h"
#ifdef T70PRO_RAM_DIAGNOSTICS
#include <Protocol/T70UsbStorageDiagnostics.h>
STATIC EFI_GUID mDiagGuid = T70_USB_STORAGE_DIAGNOSTICS_GUID;
STATIC T70_USB_STORAGE_DIAGNOSTICS mDiag = {T70_USB_DIAG_VERSION};

VOID T70UsbStorageDiagnosticInit (EFI_HANDLE Image) {
  /* Boot-services-only static storage; no NVRAM, device identifiers or payloads. */
  (VOID)gBS->InstallProtocolInterface (&Image, &mDiagGuid, EFI_NATIVE_INTERFACE, &mDiag);
}

VOID T70UsbStorageDiagnostic (EFI_HANDLE Controller, UINT32 Stage,
  EFI_STATUS Status, UINT8 Opcode, UINT8 Lun, UINT32 Result,
  UINT8 Key, UINT8 Asc, UINT8 Ascq) {
  UINTN I;
  EFI_TPL Old = gBS->RaiseTPL (TPL_NOTIFY);
  T70_USB_STORAGE_RECORD *R = NULL;
  for (I = 0; I < mDiag.Count; I++)
    if (mDiag.Records[I].Controller == Controller) { R = &mDiag.Records[I]; break; }
  if (R == NULL && mDiag.Count < T70_USB_DIAG_MAX) R = &mDiag.Records[mDiag.Count++];
  if (R == NULL) {
    // Reuse a slot so the current bind result is available after ConnectController.
    R = &mDiag.Records[mDiag.Dropped++ % T70_USB_DIAG_MAX];
    ZeroMem (R, sizeof (*R));
  }
  if (Stage == 1) { ZeroMem (R, sizeof (*R)); R->BindStatus = EFI_NOT_READY; }
  R->Controller = Controller; R->Stage = Stage; R->Sequence = ++mDiag.Sequence;
  if (Stage == 2 || Stage == 5) R->BindStatus = Status;
  if (Stage == 3) {
    R->Opcode = Opcode; R->Lun = Lun; R->CommandResult = Result; R->CommandStatus = Status;
    if (EFI_ERROR (Status) || Result != USB_MASS_CMD_SUCCESS) {
      R->LastError = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; R->ErrorOpcode = Opcode;
    }
  }
  if (Stage == 4) { R->SenseKey = Key; R->Asc = Asc; R->Ascq = Ascq; }
  if (EFI_ERROR (Status)) R->LastError = Status;
  gBS->RestoreTPL (Old);
}
#endif
