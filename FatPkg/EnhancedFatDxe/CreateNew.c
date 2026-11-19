// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Fat.h"
/* Only fixed-format diagnostic basenames, never paths. */
STATIC BOOLEAN FatDiagnosticName(CONST CHAR16 *N) {
 UINTN I;
 if(!N||N[0]!=L'T'||N[1]!=L'7'||N[2]!=L'0'||N[3]!=L'-')return FALSE;
 for(I=4;I<12;I++)if(!((N[I]>=L'0'&&N[I]<=L'9')||(N[I]>=L'A'&&N[I]<=L'F')))return FALSE;
 return N[12]==L'.'&&
  ((N[13]==L't'&&N[14]==L'x'&&N[15]==L't')||(N[13]==L'b'&&N[14]==L'm'&&N[15]==L'p'))&&N[16]==0;
}
EFI_STATUS EFIAPI FatCreateNewDiagnostic(EFI_FAT_CREATE_NEW_PROTOCOL *This,
 EFI_HANDLE Handle,UINT32 MediaId,EFI_FILE_PROTOCOL *Root,CONST CHAR16 *Name,
 EFI_FILE_PROTOCOL **NewFile) {
 FAT_VOLUME *Volume;FAT_IFILE *RootFile,*New=NULL;FAT_OFILE *Parent;
 CHAR16 Remaining[EFI_PATH_STRING_LENGTH];EFI_STATUS Status;
 if(!This||!Root||!NewFile||!FatDiagnosticName(Name))return EFI_INVALID_PARAMETER;
 *NewFile=NULL;
 Volume=CR(This,FAT_VOLUME,CreateNewInterface,FAT_VOLUME_SIGNATURE);
 RootFile=IFILE_FROM_FHAND(Root);
 if(Handle!=Volume->Handle||RootFile->OFile->Volume!=Volume||RootFile->OFile!=Volume->Root)
  return EFI_ACCESS_DENIED;
 FatWaitNonblockingTask(RootFile);
 Status=FatAcquireLockOrFail();if(EFI_ERROR(Status))return Status;
 if(!Volume->Valid||!Volume->BlockIo->Media->MediaPresent){Status=EFI_NO_MEDIA;goto Done;}
 if(MediaId!=Volume->MediaId||MediaId!=Volume->BlockIo->Media->MediaId){Status=EFI_MEDIA_CHANGED;goto Done;}
 if(Volume->ReadOnly||Volume->BlockIo->Media->ReadOnly||RootFile->ReadOnly){Status=EFI_WRITE_PROTECTED;goto Done;}
 if(EFI_ERROR(RootFile->OFile->Error)){Status=RootFile->OFile->Error;goto Done;}
 Parent=RootFile->OFile;
 Status=FatLocateOFile(&Parent,(CHAR16 *)Name,0,Remaining);
 if(EFI_ERROR(Status)){Status=FatCleanupVolume(Volume,NULL,Status,NULL);goto Done;}
 if(!Remaining[0]){Status=FatCleanupVolume(Volume,NULL,EFI_ACCESS_DENIED,NULL);goto Done;}
 // No lock release between name lookup and normal creation. All FAT opens and
 // renames serialize on this same lock. No probe/create race or private flags.
 Status=FatOFileOpen(RootFile->OFile,&New,(CHAR16 *)Name,
  EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
 if(EFI_ERROR(Status)&&New){FatIFileClose(New);New=NULL;}
 Status=FatCleanupVolume(Volume,NULL,Status,NULL);
 if(EFI_ERROR(Status)&&New){FatIFileClose(New);New=NULL;}
 if(!EFI_ERROR(Status)&&New)*NewFile=&New->Handle;
 else if(!EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
Done:
 FatReleaseLock();return Status;
}
