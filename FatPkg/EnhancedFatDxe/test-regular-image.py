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
import os,shutil,subprocess,tempfile,argparse,struct
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sd-bridge',action='store_true',help='route actual FAT I/O through actual SdDxe and a file-backed PassThru model')
parser.add_argument('--msdc-backend',type=Path,help='test-only packet ABI library; requires --sd-bridge')
parser.add_argument('--screenshot',action='store_true',help='also write deterministic 1280x800 BMP24, not an actual capture')
parser.add_argument('--devicecheck-registry',type=Path,help='root checkout for actual DeviceCheck discovery with mock metadata registry')
parser.add_argument('--devicecheck-export',action='store_true',help='actual manual core/adapter export; requires registry')
args=parser.parse_args()
if args.devicecheck_export and not args.devicecheck_registry:parser.error('--devicecheck-export requires --devicecheck-registry')
if args.msdc_backend and not args.sd_bridge:parser.error('--msdc-backend requires --sd-bridge')
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
    if args.devicecheck_registry:
        board=args.devicecheck_registry.resolve()/'Platforms/Teclast/t70proPkg'
        app=board/'Applications/DeviceCheck'
        extra+=['-DFAT_REGISTRY_FIXTURE','-I'+str(app),'-I'+str(board/'Tests/DeviceCheck')]
        extra+=[str(app/n) for n in ('SdFatTarget.c','SdFatExport.c','ManualExportCore.c')]
        if args.devicecheck_export:extra+=['-DFAT_REGISTRY_EXPORT',str(app/'ManualExportChoice.c')]
    if args.screenshot:extra+=['-DFAT_SCREENSHOT_FIXTURE']
    if args.msdc_backend:extra+=['-DFAT_MSDC_BACKEND','-ldl']
    subprocess.run(['clang',*extra,'-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(here),
      '-I'+str(root/'MdePkg/Include'),'-I'+str(root/'MdePkg/Include/AArch64'),'-I'+str(root/'FatPkg/Include'),
      *[str(here/(n+'.c')) for n in names],str(here/'TestRegularImage.c'),'-o',str(out)],check=True)
    with image.open('r+b') as f:
        subprocess.run([str(out),str(f.fileno())]+([str(args.msdc_backend.resolve())] if args.msdc_backend else []),pass_fds=(f.fileno(),),check=True)
    restored=t/'readback.bin'
    subprocess.run(['mcopy','-i',str(image),'::T70-12345678.txt',str(restored)],check=True)
    expected=bytes((i*37+11)&255 for i in range(8193))
    assert restored.read_bytes()==expected
    print('Independent fresh mcopy: 8193 bytes matched after FAT unmount')

    if args.screenshot:
        bmp=t/'readback.bmp'
        subprocess.run(['mcopy','-i',str(image),'::T70-12345678.bmp',str(bmp)],check=True)
        expected=bytearray(54)
        expected[0:2]=b'BM'
        for offset,value in ((2,3072054),(10,54),(14,40),(18,1280),(22,800),(34,3072000)):
            struct.pack_into('<I',expected,offset,value)
        struct.pack_into('<H',expected,26,1);struct.pack_into('<H',expected,28,24)
        expected+=bytes((i*13+i//3840*17)&255 for i in range(3072000))
        actual=bmp.read_bytes()
        assert len(actual)==3072054 and actual==expected
        print('Independent fresh mcopy: BMP 3072054 bytes including final tail exactly matched after collision; log also intact')

    if args.devicecheck_export:
        manual=t/'manual.txt'
        subprocess.run(['mcopy','-i',str(image),'::T70-ABCDEF01.txt',str(manual)],check=True)
        assert manual.read_bytes()==b'manual export fixture\n'
        for name in ('T70-ABCDEF00.txt','T70-ABCDEF02.txt'):
            missing=subprocess.run(['mcopy','-i',str(image),'::'+name,str(t/name)],capture_output=True)
            assert missing.returncode!=0 and not (t/name).exists()
        print('Independent mcopy: confirmed adapter file intact after collision; cancelled/replayed and stale names absent')
