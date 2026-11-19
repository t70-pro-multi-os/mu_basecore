// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host adapters only. All FAT geometry, allocation, file and cache code is real.
#include "Fat.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
EFI_BOOT_SERVICES *gBS; EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiSimpleFileSystemProtocolGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFatCreateNewProtocolGuid=EFI_FAT_CREATE_NEW_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid=EFI_FILE_INFO_ID;
EFI_GUID gEfiFileSystemInfoGuid=EFI_FILE_SYSTEM_INFO_ID;
EFI_GUID gEfiFileSystemVolumeLabelInfoIdGuid=EFI_FILE_SYSTEM_VOLUME_LABEL_ID;
EFI_GUID gEfiEventBeforeExitBootServicesGuid={0};
static int Fd,Flushes,Writes,Reads,Installed,Allocations;
static UINT64 Length;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
static EFI_FAT_CREATE_NEW_PROTOCOL *New;
VOID *EFIAPI AllocateZeroPool(UINTN N){VOID *P=calloc(1,N);assert(P);Allocations++;return P;}
VOID *EFIAPI AllocateCopyPool(UINTN N,CONST VOID *S){VOID *P=AllocateZeroPool(N);memcpy(P,S,N);return P;}
VOID EFIAPI FreePool(VOID *P){assert(P&&Allocations>0);Allocations--;free(P);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){return !memcmp(A,B,sizeof(*A));}
UINT64 EFIAPI LShiftU64(UINT64 V,UINTN N){return V<<N;}
UINT64 EFIAPI RShiftU64(UINT64 V,UINTN N){return V>>N;}
INTN EFIAPI HighBitSet32(UINT32 V){return V?31-__builtin_clz(V):-1;}
UINTN EFIAPI StrLen(CONST CHAR16 *S){UINTN N=0;while(S[N])N++;return N;}
UINTN EFIAPI StrSize(CONST CHAR16 *S){return (StrLen(S)+1)*2;}
INTN EFIAPI StrCmp(CONST CHAR16 *A,CONST CHAR16 *B){while(*A&&*A==*B){A++;B++;}return *A-*B;}
RETURN_STATUS EFIAPI StrnCpyS(CHAR16 *D,UINTN M,CONST CHAR16 *S,UINTN N){UINTN L=StrLen(S);if(L>N)L=N;assert(L<M);memmove(D,S,L*2);D[L]=0;return 0;}
RETURN_STATUS EFIAPI StrCpyS(CHAR16 *D,UINTN M,CONST CHAR16 *S){return StrnCpyS(D,M,S,StrLen(S));}
RETURN_STATUS EFIAPI StrCatS(CHAR16 *D,UINTN M,CONST CHAR16 *S){UINTN N=StrLen(D);assert(N<M);return StrCpyS(D+N,M-N,S);}
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *L){L->ForwardLink=L->BackLink=L;return L;}
LIST_ENTRY *EFIAPI GetFirstNode(CONST LIST_ENTRY *L){return L->ForwardLink;}
LIST_ENTRY *EFIAPI GetNextNode(CONST LIST_ENTRY *L,CONST LIST_ENTRY *N){(void)L;return (LIST_ENTRY *)N;}
BOOLEAN EFIAPI IsNull(CONST LIST_ENTRY *L,CONST LIST_ENTRY *N){return L==N;}
BOOLEAN EFIAPI IsListEmpty(CONST LIST_ENTRY *L){return L->ForwardLink==L;}
LIST_ENTRY *EFIAPI InsertHeadList(LIST_ENTRY *L,LIST_ENTRY *N){N->ForwardLink=L->ForwardLink;N->BackLink=L;L->ForwardLink->BackLink=N;L->ForwardLink=N;return L;}
LIST_ENTRY *EFIAPI InsertTailList(LIST_ENTRY *L,LIST_ENTRY *N){N->BackLink=L->BackLink;N->ForwardLink=L;L->BackLink->ForwardLink=N;L->BackLink=N;return L;}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *N){N->BackLink->ForwardLink=N->ForwardLink;N->ForwardLink->BackLink=N->BackLink;return (LIST_ENTRY *)N;}
EFI_STATUS EFIAPI EfiAcquireLockOrFail(EFI_LOCK *L){if(L->Lock==EfiLockAcquired)return EFI_ACCESS_DENIED;L->Lock=EfiLockAcquired;return 0;}
VOID EFIAPI EfiAcquireLock(EFI_LOCK *L){assert(EfiAcquireLockOrFail(L)==0);}
VOID EFIAPI EfiReleaseLock(EFI_LOCK *L){assert(L->Lock==EfiLockAcquired);L->Lock=EfiLockReleased;}
// ASCII-only collation. Reject any accidental expansion to arbitrary Unicode.
INTN FatStriCmp(CHAR16 *A,CHAR16 *B){while(*A&&*B){assert(*A<128&&*B<128);int D=toupper(*A)-toupper(*B);if(D)return D;A++;B++;}return *A-*B;}
VOID FatStrUpr(CHAR16 *S){for(;*S;S++){assert(*S<128);*S=toupper(*S);}}
VOID FatStrLwr(CHAR16 *S){for(;*S;S++){assert(*S<128);*S=tolower(*S);}}
VOID FatFatToStr(UINTN N,CHAR8 *F,CHAR16 *S){for(UINTN I=0;I<N;I++){assert((UINT8)F[I]<128);S[I]=(UINT8)F[I];}S[N]=0;}
BOOLEAN FatStrToFat(CHAR16 *S,UINTN N,CHAR8 *F){for(UINTN I=0;I<N;I++){assert(S[I]<128);F[I]=(CHAR8)toupper(S[I]);}return FALSE;}
#ifdef FAT_SD_BRIDGE
#include "TestSdImageBridge.h"
#endif
static EFI_STATUS EFIAPI DiskRead(EFI_DISK_IO_PROTOCOL *D,UINT32 M,UINT64 O,UINTN N,VOID *B){(void)D;Reads++;assert(M==7&&O<=Length&&N<=Length-O);
#ifdef FAT_SD_BRIDGE
 return BridgeIo(FALSE,M,O,N,B);
#else
 return pread(Fd,B,N,O)==(ssize_t)N?0:EFI_DEVICE_ERROR;
#endif
}
static EFI_STATUS EFIAPI DiskWrite(EFI_DISK_IO_PROTOCOL *D,UINT32 M,UINT64 O,UINTN N,VOID *B){(void)D;assert(M==7&&O<=Length&&N<=Length-O);Writes++;
#ifdef FAT_SD_BRIDGE
 return BridgeIo(TRUE,M,O,N,B);
#else
 return pwrite(Fd,B,N,O)==(ssize_t)N?0:EFI_DEVICE_ERROR;
#endif
}
static EFI_STATUS EFIAPI BlockFlush(EFI_BLOCK_IO_PROTOCOL *B){(void)B;Flushes++;
#ifdef FAT_SD_BRIDGE
 return BridgeFlush();
#else
 return fsync(Fd)==0?0:EFI_DEVICE_ERROR;
#endif
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
 assert(*H==(VOID *)1&&!Installed);va_list A;va_start(A,H);
 assert(va_arg(A,EFI_GUID *)==&gEfiSimpleFileSystemProtocolGuid);Fs=va_arg(A,VOID *);
 assert(va_arg(A,EFI_GUID *)==&gEfiFatCreateNewProtocolGuid);New=va_arg(A,VOID *);
 assert(va_arg(A,VOID *)==NULL);va_end(A);Installed=1;return 0;
}
static EFI_STATUS EFIAPI Remove(EFI_HANDLE H,...){
 assert(H==(VOID *)1&&Installed);va_list A;va_start(A,H);
 assert(va_arg(A,EFI_GUID *)==&gEfiSimpleFileSystemProtocolGuid&&va_arg(A,VOID *)==Fs);
 assert(va_arg(A,EFI_GUID *)==&gEfiFatCreateNewProtocolGuid&&va_arg(A,VOID *)==New);
 assert(va_arg(A,VOID *)==NULL);va_end(A);Installed=0;return 0;
}
static EFI_STATUS EFIAPI Event(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){(void)T;(void)P;(void)N;(void)C;(void)G;*E=(VOID *)2;return 0;}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E){assert(E==(VOID *)2);return 0;}
static EFI_STATUS EFIAPI Time(EFI_TIME *T,EFI_TIME_CAPABILITIES *C){(void)C;memset(T,0,sizeof(*T));T->Year=2026;T->Month=11;T->Day=19;return 0;}
static EFI_STATUS EFIAPI Crc(VOID *Data,UINTN N,UINT32 *Out){UINT32 V=~0U;UINT8 *B=Data;for(UINTN I=0;I<N;I++){V^=B[I];for(int J=0;J<8;J++)V=(V>>1)^(0xedb88320U&-(V&1));}*Out=~V;return 0;}
#ifdef FAT_REGISTRY_FIXTURE
#include "FatRegistryFixture.h"
#endif
#ifdef FAT_SCREENSHOT_FIXTURE
static void Le32(UINT8 *P,UINT32 V){for(unsigned I=0;I<4;I++)P[I]=(UINT8)(V>>(8*I));}
static void ScreenshotFixture(EFI_FILE_PROTOCOL *Root){
 // Deterministic BMP24, NOT an actual GOP capture. Positive height = bottom-up.
 const UINTN Size=54+1280*800*3;UINT8 *Bmp=calloc(1,Size);assert(Bmp);
 Bmp[0]='B';Bmp[1]='M';Le32(Bmp+2,Size);Le32(Bmp+10,54);Le32(Bmp+14,40);
 Le32(Bmp+18,1280);Le32(Bmp+22,800);Bmp[26]=1;Bmp[28]=24;Le32(Bmp+34,Size-54);
 for(UINTN I=54;I<Size;I++)Bmp[I]=(UINT8)((I-54)*13+(I-54)/3840*17);
 EFI_FILE_PROTOCOL *File=NULL;assert(New->CreateNew(New,(VOID *)1,7,Root,L"T70-12345678.bmp",&File)==0&&File);
 UINTN N=Size;assert(File->Write(File,&N,Bmp)==0&&N==Size);
 int Before=Flushes;assert(File->Flush(File)==0&&Flushes>Before);assert(File->Close(File)==0);
 File=(VOID *)99;assert(New->CreateNew(New,(VOID *)1,7,Root,L"T70-12345678.bmp",&File)==EFI_ACCESS_DENIED&&File==NULL);
 free(Bmp);printf("BMP fixture: 3072054 bytes in one File.Write, Flush/Close and collision rejection passed\n");
}
#endif
int main(int argc,char **argv){
#ifdef FAT_MSDC_BACKEND
 assert(argc==3);BackendPath=argv[2];
#else
 assert(argc==2);
#endif
 Fd=atoi(argv[1]);struct stat St;assert(fstat(Fd,&St)==0&&S_ISREG(St.st_mode)&&St.st_nlink==1&&St.st_size==32*1024*1024);Length=St.st_size;
 EFI_BOOT_SERVICES Bs={0};Bs.InstallMultipleProtocolInterfaces=Install;Bs.UninstallMultipleProtocolInterfaces=Remove;Bs.CreateEventEx=Event;Bs.CloseEvent=CloseEvent;Bs.CalculateCrc32=Crc;gBS=&Bs;
 #ifdef FAT_SD_BRIDGE
 InitBridge(&Bs);
#endif
 EFI_RUNTIME_SERVICES Rt={0};Rt.GetTime=Time;gRT=&Rt;
 EFI_DISK_IO_PROTOCOL Disk={0};Disk.ReadDisk=DiskRead;Disk.WriteDisk=DiskWrite;
 EFI_BLOCK_IO_MEDIA Media={0};Media.MediaId=7;Media.MediaPresent=1;Media.BlockSize=512;Media.LastBlock=Length/512-1;
 EFI_BLOCK_IO_PROTOCOL Block={0};Block.Media=&Media;Block.FlushBlocks=BlockFlush;
 assert(FatAllocateVolume((VOID *)1,&Disk,NULL,&Block)==0&&Installed);
#ifdef FAT_REGISTRY_FIXTURE
 RegistryBefore(&Bs,&Block);
#endif
 EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;assert(Fs->OpenVolume(Fs,&Root)==0&&Root);
 int InitialWrites=Writes;
 assert(New->CreateNew(New,(VOID *)1,8,Root,L"T70-12345678.txt",&File)==EFI_MEDIA_CHANGED&&!File&&Writes==InitialWrites);
 Media.ReadOnly=TRUE;
 assert(New->CreateNew(New,(VOID *)1,7,Root,L"T70-12345678.txt",&File)==EFI_WRITE_PROTECTED&&!File&&Writes==InitialWrites);
 Media.ReadOnly=FALSE; // Only synthetic test media, never a board PCD or device.
 assert(New->CreateNew(New,(VOID *)1,7,Root,L"T70-12345678.txt",&File)==0&&File);
 unsigned char Payload[8193];for(unsigned I=0;I<sizeof(Payload);I++)Payload[I]=(I*37+11)&255;
 UINTN N=sizeof(Payload);assert(File->Write(File,&N,Payload)==0&&N==sizeof(Payload));
 int Before=Flushes;assert(File->Flush(File)==0&&Flushes>Before);assert(File->Close(File)==0);
 File=(VOID *)99;assert(New->CreateNew(New,(VOID *)1,7,Root,L"T70-12345678.txt",&File)==EFI_ACCESS_DENIED&&File==NULL);
 #ifdef FAT_SCREENSHOT_FIXTURE
 ScreenshotFixture(Root);
#endif
 assert(Root->Close(Root)==0);
 FAT_VOLUME *Volume=CR(Fs,FAT_VOLUME,VolumeInterface,FAT_VOLUME_SIGNATURE);
 assert(FatAbandonVolume(Volume)==0&&!Installed&&!Allocations);
#ifdef FAT_REGISTRY_FIXTURE
 RegistryAfter(&Bs);
#endif
 #ifdef FAT_SD_BRIDGE
#ifdef FAT_MSDC_BACKEND
 UINT64 BackendCounts[5];BackendStats(BackendCounts);
 assert(BackendCounts[0]==SdWrites+SdReads+SdStatuses&&BackendCounts[1]>0&&BackendCounts[2]>0&&BackendCounts[3]==SdStatuses&&BackendCounts[4]==SdCmd25);
 printf("Actual MSDC FIFO: RX=%llu TX=%llu bytes, CMD13=%llu CMD25=%llu\n",(unsigned long long)BackendCounts[1],(unsigned long long)BackendCounts[2],(unsigned long long)BackendCounts[3],(unsigned long long)BackendCounts[4]);
#endif
 assert(SdWrites>0&&SdReads>0&&SdStatuses==SdWrites&&SdFlushes==Flushes&&!CardPending&&Tpl==TPL_APPLICATION);
 printf("Actual SdDxe: %u read packets, CMD24=%u CMD25=%u, %u ordered CMD13, %u FlushBlocks calls\n",SdReads,SdCmd24,SdCmd25,SdStatuses,SdFlushes);
#endif
 assert(Writes>0&&Flushes>0);printf("Actual FAT create/write/flush/collision/unmount passed; %d disk writes, %d block flushes\n",Writes,Flushes);return 0;
}
