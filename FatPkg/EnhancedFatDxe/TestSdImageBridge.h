// SPDX-License-Identifier: BSD-2-Clause-Patent
// Test-only byte DiskIo shim over actual SdDxe BlockIo and a file-backed card.
#include "../../MdeModulePkg/Bus/Sd/SdDxe/SdDxe.h"
BOOLEAN EFIAPI DebugCodeEnabled(VOID){return FALSE;}
UINTN EFIAPI DevicePathNodeLength(CONST VOID *Node){CONST EFI_DEVICE_PATH_PROTOCOL *P=Node;return P->Length[0]+((UINTN)P->Length[1]<<8);}
UINT64 EFIAPI MultU64x32(UINT64 V,UINT32 M){return V*M;}
#ifdef FAT_MSDC_BACKEND
#include <dlfcn.h>
static CONST char *BackendPath;
static EFI_STATUS (*BackendPacket)(EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *);
static void (*BackendStats)(UINT64 *);
static void LoadBackend(void){
 VOID *L=dlopen(BackendPath,RTLD_NOW|RTLD_LOCAL);if(!L)fprintf(stderr,"%s\n",dlerror());assert(L);
 int (*Init)(int)=dlsym(L,"T70MsdcImageInit");
 BackendPacket=dlsym(L,"T70MsdcImagePacket");BackendStats=dlsym(L,"T70MsdcImageStats");
 assert(Init&&BackendPacket&&BackendStats&&Init(Fd)==0);
}
#endif
static SD_DEVICE Sd;
static SD_DRIVER_PRIVATE_DATA SdPrivate;
static EFI_SD_MMC_PASS_THRU_PROTOCOL SdPass;
static unsigned SdReads,SdWrites,SdStatuses,SdFlushes,SdCmd24,SdCmd25;
static BOOLEAN CardPending;
static EFI_TPL Tpl=TPL_APPLICATION;
static EFI_TPL EFIAPI Raise(EFI_TPL Next){EFI_TPL Old=Tpl;assert(Next>=Old);Tpl=Next;return Old;}
static VOID EFIAPI Restore(EFI_TPL Old){assert(Old<=Tpl);Tpl=Old;}
static EFI_STATUS EFIAPI Stall(UINTN Us){(void)Us;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI BuildPath(EFI_SD_MMC_PASS_THRU_PROTOCOL *P,UINT8 Slot,EFI_DEVICE_PATH_PROTOCOL **Out){
 assert(P==&SdPass&&Slot==0);SD_DEVICE_PATH *D=AllocateZeroPool(sizeof(*D));
 D->Header.Type=MESSAGING_DEVICE_PATH;D->Header.SubType=MSG_SD_DP;D->Header.Length[0]=sizeof(*D);D->SlotNumber=0;*Out=(VOID *)D;return 0;
}
static EFI_STATUS EFIAPI Card(EFI_SD_MMC_PASS_THRU_PROTOCOL *P,UINT8 Slot,EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,EFI_EVENT Event){
 assert(P==&SdPass&&Slot==0&&!Event&&Packet&&Packet->SdMmcCmdBlk&&Packet->SdMmcStatusBlk);
#ifdef FAT_MSDC_BACKEND
 UINT32 Cmd=Packet->SdMmcCmdBlk->CommandIndex;
 assert(Cmd==13?CardPending:!CardPending);
 EFI_STATUS S=BackendPacket(Packet);if(EFI_ERROR(S))return S;
 if(Cmd==13){SdStatuses++;CardPending=FALSE;}
 else if(Cmd==24||Cmd==25){SdWrites++;CardPending=TRUE;if(Cmd==24)SdCmd24++;else SdCmd25++;}
 else {assert(Cmd==17||Cmd==18);SdReads++;}
 return S;
#else
 EFI_SD_MMC_COMMAND_BLOCK *C=Packet->SdMmcCmdBlk;memset(Packet->SdMmcStatusBlk,0,sizeof(*Packet->SdMmcStatusBlk));
 if(C->CommandIndex==13){
  assert(CardPending&&C->CommandArgument==(1U<<16));SdStatuses++;CardPending=FALSE;
  Packet->SdMmcStatusBlk->Resp0=BIT8|(4U<<9);Packet->TransactionStatus=0;return 0;
 }
 assert(!CardPending);BOOLEAN Write=C->CommandIndex==24||C->CommandIndex==25;
 assert(Write||C->CommandIndex==17||C->CommandIndex==18);
 UINTN N=Write?Packet->OutTransferLength:Packet->InTransferLength;
 VOID *B=Write?Packet->OutDataBuffer:Packet->InDataBuffer;
 UINT64 Offset=(UINT64)C->CommandArgument*512;
 assert(N&&!(N%512)&&Offset<=Length&&N<=Length-Offset);
 assert(C->CommandIndex==(Write?(N==512?24:25):(N==512?17:18)));
 if(Write){assert(pwrite(Fd,B,N,Offset)==(ssize_t)N);SdWrites++;if(C->CommandIndex==24)SdCmd24++;else SdCmd25++;CardPending=TRUE;}
 else {assert(pread(Fd,B,N,Offset)==(ssize_t)N);SdReads++;}
 Packet->TransactionStatus=0;return 0;
#endif
}
// Byte requests are explicitly adapted with read-modify-write for partial sectors.
static EFI_STATUS BridgeIo(BOOLEAN Write,UINT32 Mid,UINT64 Offset,UINTN Size,VOID *Buffer){
 assert(Mid==7&&Offset<=Length&&Size<=Length-Offset);
 UINT8 *B=Buffer;UINT8 Sector[512];EFI_STATUS S;
 while(Size){
  UINTN Skip=Offset%512,N=MIN(Size,512-Skip);EFI_LBA Lba=Offset/512;
  if(!Skip&&Size>=512){N=Size-(Size%512);
   S=Write?Sd.BlockIo.WriteBlocks(&Sd.BlockIo,Mid,Lba,N,B):Sd.BlockIo.ReadBlocks(&Sd.BlockIo,Mid,Lba,N,B);
  }else{
   S=Sd.BlockIo.ReadBlocks(&Sd.BlockIo,Mid,Lba,512,Sector);
   if(!EFI_ERROR(S)){if(Write){memcpy(Sector+Skip,B,N);S=Sd.BlockIo.WriteBlocks(&Sd.BlockIo,Mid,Lba,512,Sector);}else memcpy(B,Sector+Skip,N);}
  }
  if(EFI_ERROR(S))return S;B+=N;Offset+=N;Size-=N;
 }
 assert(!CardPending);return 0;
}
static EFI_STATUS BridgeFlush(void){
 SdFlushes++;EFI_STATUS S=SdFlushBlocks(&Sd.BlockIo);assert(!CardPending);
 // fsync is host-image durability only, not a command claimed to exist on SD.
 if(!EFI_ERROR(S)&&fsync(Fd)!=0)return EFI_DEVICE_ERROR;return S;
}
static void InitBridge(EFI_BOOT_SERVICES *Bs){
#ifdef FAT_MSDC_BACKEND
 LoadBackend();
#endif
 Bs->RaiseTPL=Raise;Bs->RestoreTPL=Restore;Bs->Stall=Stall;
 SdPass.PassThru=Card;SdPass.BuildDevicePath=BuildPath;SdPrivate.PassThru=&SdPass;
 Sd.Signature=SD_DEVICE_SIGNATURE;Sd.Private=&SdPrivate;Sd.SectorAddressing=TRUE;Sd.Rca=1;
 Sd.BlockMedia.MediaId=7;Sd.BlockMedia.MediaPresent=TRUE;Sd.BlockMedia.BlockSize=512;Sd.BlockMedia.LastBlock=Length/512-1;
 Sd.BlockIo.Media=&Sd.BlockMedia;Sd.BlockIo.ReadBlocks=SdReadBlocks;Sd.BlockIo.WriteBlocks=SdWriteBlocks;Sd.BlockIo.FlushBlocks=SdFlushBlocks;
 InitializeListHead(&Sd.Queue);InitializeListHead(&Sd.OrderedIo);
}
