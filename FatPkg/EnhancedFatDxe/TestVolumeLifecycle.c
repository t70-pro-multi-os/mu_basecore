// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Init.c functions, mocked protocols and allocation; no device IO.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "Fat.h"
#include "InitLifecycle.inc"
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiSimpleFileSystemProtocolGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFatCreateNewProtocolGuid=EFI_FAT_CREATE_NEW_PROTOCOL_GUID;
EFI_GUID gEfiEventBeforeExitBootServicesGuid={0};
static FAT_VOLUME *V;
static EFI_BLOCK_IO_MEDIA Media;
static EFI_BLOCK_IO_PROTOCOL Block;
static int Allocated,Freed,Installed,Removed,Cleaned,EventCount,Locked,Errors;
static int FailInstall,FailRemove,FailOpen,FailCache,FailEvent,FailAllocate;
VOID *EFIAPI AllocateZeroPool(UINTN N){assert(!Allocated);if(FailAllocate)return NULL;V=calloc(1,N);assert(V);Allocated=1;return V;}
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *L){L->ForwardLink=L->BackLink=L;return L;}
VOID FatFreeVolume(FAT_VOLUME *P){assert(P==V&&!Installed&&Allocated);free(P);V=NULL;Allocated=0;Freed++;}
EFI_STATUS FatOpenDevice(FAT_VOLUME *P){assert(P==V);return FailOpen?EFI_UNSUPPORTED:EFI_SUCCESS;}
EFI_STATUS FatInitializeDiskCache(FAT_VOLUME *P){assert(P==V);return FailCache?EFI_OUT_OF_RESOURCES:EFI_SUCCESS;}
EFI_STATUS EFIAPI FatOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *S,EFI_FILE_PROTOCOL **F){(void)S;(void)F;assert(0);return EFI_UNSUPPORTED;}
EFI_STATUS EFIAPI FatCreateNewDiagnostic(EFI_FAT_CREATE_NEW_PROTOCOL *S,EFI_HANDLE H,UINT32 M,EFI_FILE_PROTOCOL *R,CONST CHAR16 *N,EFI_FILE_PROTOCOL **F){(void)S;(void)H;(void)M;(void)R;(void)N;(void)F;assert(0);return EFI_UNSUPPORTED;}
VOID EFIAPI FatOnBeforeExitBootServices(EFI_EVENT E,VOID *C){(void)E;(void)C;assert(0);}
EFI_STATUS FatAcquireLockOrFail(VOID){if(Locked)return EFI_ACCESS_DENIED;Locked=1;return EFI_SUCCESS;}
VOID FatReleaseLock(VOID){assert(Locked);Locked=0;}
VOID FatSetVolumeError(FAT_OFILE *O,EFI_STATUS S){assert(O==V->Root&&S==EFI_MEDIA_CHANGED);Errors++;}
EFI_STATUS FatCleanupVolume(FAT_VOLUME *P,FAT_OFILE *O,EFI_STATUS S,FAT_TASK *T){
 assert(P==V&&!O&&!T&&!S&&Locked&&!P->Valid&&!Installed);Cleaned++;return EFI_SUCCESS;
}
static void Pair(va_list Args){
 assert(va_arg(Args,EFI_GUID *)==&gEfiSimpleFileSystemProtocolGuid);
 assert(va_arg(Args,VOID *)==&V->VolumeInterface);
 assert(va_arg(Args,EFI_GUID *)==&gEfiFatCreateNewProtocolGuid);
 assert(va_arg(Args,VOID *)==&V->CreateNewInterface);
 assert(va_arg(Args,VOID *)==NULL);
 assert(V->CreateNewInterface.Revision==1&&V->CreateNewInterface.CreateNew==FatCreateNewDiagnostic);
 assert(V->VolumeInterface.OpenVolume==FatOpenVolume);
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
 assert(H==&V->Handle&&*H==(VOID *)1&&!Installed&&!V->Valid);
 va_list A;va_start(A,H);Pair(A);va_end(A);
 if(FailInstall)return EFI_OUT_OF_RESOURCES;Installed=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Remove(EFI_HANDLE H,...){
 assert(H==V->Handle&&Installed&&V->Valid);
 va_list A;va_start(A,H);Pair(A);va_end(A);
 if(FailRemove)return EFI_ACCESS_DENIED;Installed=0;Removed++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Out){
 assert(Type==EVT_NOTIFY_SIGNAL&&Tpl==TPL_CALLBACK&&Notify==FatOnBeforeExitBootServices&&Context==V&&Group==&gEfiEventBeforeExitBootServicesGuid&&Installed&&V->Valid);
 EventCount++;*Out=(VOID *)2;return FailEvent?EFI_OUT_OF_RESOURCES:EFI_SUCCESS;
}
static void Reset(void){
 assert(!Allocated&&!V);Freed=Installed=Removed=Cleaned=EventCount=Locked=Errors=0;
 FailInstall=FailRemove=FailOpen=FailCache=FailEvent=FailAllocate=0;
 memset(&Media,0,sizeof Media);Media.BlockSize=512;Media.MediaId=7;Media.MediaPresent=TRUE;Block.Media=&Media;
}
int main(void){
 EFI_BOOT_SERVICES Bs={0};Bs.InstallMultipleProtocolInterfaces=Install;Bs.UninstallMultipleProtocolInterfaces=Remove;Bs.CreateEventEx=Event;gBS=&Bs;
 unsigned Cases=0;
 for(unsigned K=0;K<8;K++){
  Reset();FailAllocate=K==1;FailOpen=K==2;FailCache=K==3;FailInstall=K==4;FailEvent=K==5;FailRemove=K==6;
  EFI_STATUS S=FatAllocateVolume((VOID *)1,NULL,NULL,&Block);
  if(K>=1&&K<=4){assert(EFI_ERROR(S)&&!V&&!Installed&&!Allocated&&!EventCount&&Freed==(K==1?0:1));Cases++;continue;}
  assert(S==EFI_SUCCESS&&V&&V->Valid&&Installed&&EventCount==1&&!Freed);
  assert(V->FlushEvent==(K==5?NULL:(VOID *)2));
  FAT_OFILE Root={0};V->Root=&Root;
  if(K==6){
   FAT_VOLUME Before=*V;S=FatAbandonVolume(V);
   assert(S==EFI_ACCESS_DENIED&&Installed&&!Removed&&!Cleaned&&!Errors&&!Locked&&!memcmp(&Before,V,sizeof Before));FailRemove=0;
  }
  if(K==7)Locked=1;
  assert(FatAbandonVolume(V)==EFI_SUCCESS&&!Installed&&Removed==1&&!V->Valid&&Errors==1);
  assert(Cleaned==(K==7?0:1)&&Locked==(K==7?1:0));Locked=0;FatFreeVolume(V);Cases++;
 }
 printf("FAT lifecycle: %u actual-function scenarios passed; protocol/cache operations mocked\n",Cases);return 0;
}
