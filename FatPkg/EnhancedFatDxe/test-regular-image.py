#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Actual EnhancedFatDxe on a private regular FAT image; never mounts a volume.
Host services and ASCII collation are adapters. FAT code is compiled unchanged.
mformat initializes the image; independent mcopy verifies after driver teardown.
Optional --sd-bridge adds actual SdDxe synchronous BlockIo. A byte DiskIo
shim adapts partial sectors; a PassThru card model alone uses pread/pwrite.
Every modeled write must receive CMD13 READY_FOR_DATA+TRAN before other IO.
Bridge Flush calls real SdFlushBlocks; host fsync separately makes the private
image durable. This is NOT proof of physical card programming/durability.
Synthetic SD_DEVICE is initialized directly; discovery, partition enumeration,
MSDC MMIO and real firmware services are outside the test. Production RO PCD
is left TRUE and the isolated synthetic media is writable for this test.
No command-line path is accepted. No downloads, device nodes, or RO changes.
"""
from pathlib import Path
import os,shutil,subprocess,tempfile,argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sd-bridge',action='store_true',help='route actual FAT I/O through actual SdDxe and a file-backed PassThru model')
args=parser.parse_args()
here=Path(__file__).resolve().parent;root=here.parents[1]
for cmd in ('clang','mformat','mcopy'):
    if not shutil.which(cmd):raise SystemExit('Missing existing dependency: '+cmd)
names='DirectoryCache DiskCache FileName Hash DirectoryManage ReadWrite OpenVolume Open CreateNew Misc Init Info FileSpace Flush Delete Data'.split()
with tempfile.TemporaryDirectory(prefix='fat-native-') as td:
    t=Path(td);image=t/'test.fat';out=t/'native'
    with image.open('xb') as f:f.truncate(32*1024*1024)
    subprocess.run(['mformat','-i',str(image),'::'],check=True)
    extra=[]
    if args.sd_bridge:
        sd=root/'MdeModulePkg/Bus/Sd/SdDxe'
        extra=['-include',str(here/'TestSdBuild.h'),'-DFAT_SD_BRIDGE','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
               '-I'+str(root/'MdeModulePkg/Include'),str(sd/'SdBlockIo.c'),str(sd/'SdDxe.c')]
    subprocess.run(['clang',*extra,'-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(here),
      '-I'+str(root/'MdePkg/Include'),'-I'+str(root/'MdePkg/Include/AArch64'),'-I'+str(root/'FatPkg/Include'),
      *[str(here/(n+'.c')) for n in names],str(here/'TestRegularImage.c'),'-o',str(out)],check=True)
    with image.open('r+b') as f:
        subprocess.run([str(out),str(f.fileno())],pass_fds=(f.fileno(),),check=True)
    restored=t/'readback.bin'
    subprocess.run(['mcopy','-i',str(image),'::T70-12345678.txt',str(restored)],check=True)
    expected=bytes((i*37+11)&255 for i in range(8193))
    assert restored.read_bytes()==expected
    print('Independent fresh mcopy: 8193 bytes matched after FAT unmount')
