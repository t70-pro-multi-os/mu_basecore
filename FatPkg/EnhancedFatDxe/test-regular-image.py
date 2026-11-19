#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Actual EnhancedFatDxe on a private regular FAT image; never mounts a volume.
Host services and ASCII collation are adapters. FAT code is compiled unchanged.
mformat initializes the image; independent mcopy verifies after driver teardown.
No command-line path is accepted. No downloads, device nodes, or RO changes.
"""
from pathlib import Path
import os,shutil,subprocess,tempfile
here=Path(__file__).resolve().parent;root=here.parents[1]
for cmd in ('clang','mformat','mcopy'):
    if not shutil.which(cmd):raise SystemExit('Missing existing dependency: '+cmd)
names='DirectoryCache DiskCache FileName Hash DirectoryManage ReadWrite OpenVolume Open CreateNew Misc Init Info FileSpace Flush Delete Data'.split()
with tempfile.TemporaryDirectory(prefix='fat-native-') as td:
    t=Path(td);image=t/'test.fat';out=t/'native'
    with image.open('xb') as f:f.truncate(32*1024*1024)
    subprocess.run(['mformat','-i',str(image),'::'],check=True)
    subprocess.run(['clang','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(here),
      '-I'+str(root/'MdePkg/Include'),'-I'+str(root/'MdePkg/Include/AArch64'),'-I'+str(root/'FatPkg/Include'),
      *[str(here/(n+'.c')) for n in names],str(here/'TestRegularImage.c'),'-o',str(out)],check=True)
    with image.open('r+b') as f:
        subprocess.run([str(out),str(f.fileno())],pass_fds=(f.fileno(),),check=True)
    restored=t/'readback.bin'
    subprocess.run(['mcopy','-i',str(image),'::T70-12345678.txt',str(restored)],check=True)
    expected=bytes((i*37+11)&255 for i in range(8193))
    assert restored.read_bytes()==expected
    print('Independent fresh mcopy: 8193 bytes matched after FAT unmount')
