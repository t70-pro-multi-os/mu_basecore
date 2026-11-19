// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef FAT_CREATE_NEW_H
#define FAT_CREATE_NEW_H
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#define EFI_FAT_CREATE_NEW_PROTOCOL_GUID {0x4720e0ed,0x40f5,0x4c44,{0xa9,0x50,0x29,0xd1,0x70,0x44,0x37,0xfa}}
typedef struct _EFI_FAT_CREATE_NEW_PROTOCOL EFI_FAT_CREATE_NEW_PROTOCOL;
/* Optional, synchronous, root-only diagnostic create-if-absent. No standard
 * File.Open flags are changed. Caller must hold a root handle on This volume,
 * select the volume explicitly and supply its current MediaId. No writes to
 * an existing name, no automatic retry/delete. Newly created partial files may
 * remain on an error. Close root/new handles before releasing protocol use. */
typedef EFI_STATUS (EFIAPI *EFI_FAT_CREATE_NEW)(EFI_FAT_CREATE_NEW_PROTOCOL *This,
 EFI_HANDLE VolumeHandle, UINT32 MediaId, EFI_FILE_PROTOCOL *Root,
 CONST CHAR16 *Name, EFI_FILE_PROTOCOL **NewFile);
struct _EFI_FAT_CREATE_NEW_PROTOCOL { UINT64 Revision; EFI_FAT_CREATE_NEW CreateNew; };
extern EFI_GUID gEfiFatCreateNewProtocolGuid;
#endif
