// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-only actual new callback with mocked FAT operations, no storage.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "CreateNew.c"
#include "Open.c"
static int Locked,Exists,Opened,Cleaned,Closed,FailOpen,FailCleanup;
static FAT_VOLUME V;static FAT_OFILE O;static FAT_IFILE Root,New;
static EFI_BLOCK_IO_PROTOCOL Block;static EFI_BLOCK_IO_MEDIA Media;
VOID FatWaitNonblockingTask(FAT_IFILE *I){assert(I==&Root);}
EFI_STATUS FatAcquireLockOrFail(VOID){if(Locked)return EFI_ACCESS_DENIED;Locked=1;return 0;}
VOID FatReleaseLock(VOID){assert(Locked);Locked=0;}
EFI_STATUS FatLocateOFile(FAT_OFILE **P,CHAR16 *N,UINT8 A,CHAR16 *R){assert(Locked&&*P==&O&&!A&&N);R[0]=Exists?0:L'T';R[1]=0;return 0;}
static FAT_DIRENT Entry;
EFI_STATUS FatCreateDirEnt(FAT_OFILE *P,CHAR16 *N,UINT8 A,FAT_DIRENT **Out){assert(Locked&&P==&O&&N&&!A);*Out=&Entry;return 0;}
EFI_STATUS FatOpenDirEnt(FAT_OFILE *P,FAT_DIRENT *D){assert(P==&O&&D==&Entry);D->OFile=&O;return 0;}
EFI_STATUS FatCreateDotDirEnts(FAT_OFILE *P){(void)P;return 0;}
EFI_STATUS FatAllocateIFile(FAT_OFILE *P,FAT_IFILE **Out){assert(Locked&&P==&O);Opened++;*Out=&New;return FailOpen?EFI_DEVICE_ERROR:0;}
EFI_STATUS FatOFileFlush(FAT_OFILE *P){assert(Locked&&P==&O);return 0;}

EFI_STATUS FatCleanupVolume(FAT_VOLUME *P,FAT_OFILE *F,EFI_STATUS S,FAT_TASK *T){assert(Locked&&P==&V&&!F&&!T);Cleaned++;return FailCleanup?EFI_DEVICE_ERROR:S;}
EFI_STATUS FatIFileClose(FAT_IFILE *P){assert(Locked&&P==&New);Closed++;return 0;}
static void reset(void){memset(&V,0,sizeof V);memset(&O,0,sizeof O);memset(&Root,0,sizeof Root);V.Signature=FAT_VOLUME_SIGNATURE;V.Handle=(void *)1;V.Root=&O;V.Valid=1;V.MediaId=7;V.BlockIo=&Block;Block.Media=&Media;memset(&Media,0,sizeof Media);Media.MediaId=7;Media.MediaPresent=1;Root.Signature=FAT_IFILE_SIGNATURE;Root.OFile=&O;O.Volume=&V;memset(&Entry,0,sizeof Entry);O.DirEnt=&Entry;Locked=Exists=Opened=Cleaned=Closed=FailOpen=FailCleanup=0;}
int main(void){EFI_FILE_PROTOCOL *Out;unsigned Cases=0;
 for(unsigned K=0;K<9;K++){reset();Out=NULL;
  if(K==1)Exists=1;if(K==2)FailOpen=1;if(K==3)FailCleanup=1;if(K==4)Media.ReadOnly=1;if(K==5)Media.MediaId++;if(K==6)Media.MediaPresent=0;if(K==7)Locked=1;
  EFI_STATUS S=FatCreateNewDiagnostic(&V.CreateNewInterface,V.Handle,7,&Root.Handle,K==8?L"../old":L"T70-12345678.txt",&Out);
  if(!K){assert(S==0&&Out==&New.Handle&&Opened==1&&Cleaned==1&&!Closed);}
  else{assert(EFI_ERROR(S)&&!Out);if(K==2||K==3)assert(Closed==1);else assert(!Opened);}
  if(K!=7)assert(!Locked);Cases++;
 }
 reset();Exists=1;Locked=1;FAT_IFILE *Old=NULL;
 assert(FatOFileOpen(&O,&Old,L"T70-12345678.txt",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0)==EFI_SUCCESS&&Old==&New&&Opened==1);Locked=0;Cases++;
 reset();assert(FatDiagnosticName(L"T70-00000000.bmp"));assert(!FatDiagnosticName(L"T70-00000000."));assert(!FatDiagnosticName(L"T70-00000000.txt/child"));
 printf("FAT CreateNew: %u callback cases plus name bounds passed\n",Cases);return 0;}
