#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Extract exact current production functions, not a duplicate implementation.
from pathlib import Path
import subprocess,tempfile
here=Path(__file__).resolve().parent
root=here.parents[1]
s=(here/'Init.c').read_text()
functions=[]
for name in ('FatAllocateVolume','FatAbandonVolume'):
    a=s.index('\n'+name+' (')+1
    b=s.index('{',a);depth=1;e=b+1
    while depth:
        depth+=(s[e]=='{')-(s[e]=='}');e+=1
    functions.append('EFI_STATUS\n'+s[a:e])
source=(here/'TestVolumeLifecycle.c').read_text().replace('#include "InitLifecycle.inc"','\n'.join(functions))
with tempfile.TemporaryDirectory(prefix='fat-lifecycle-') as tmp:
    c=Path(tmp)/'test.c';out=Path(tmp)/'test';c.write_text(source)
    subprocess.run(['clang','-fshort-wchar','-DMDEPKG_NDEBUG','-include','Base.h',
      '-I'+str(here),'-I'+str(root/'MdePkg/Include'),'-I'+str(root/'MdePkg/Include/AArch64'),
      '-I'+str(root/'FatPkg/Include'),str(c),'-o',str(out)],check=True)
    subprocess.run([str(out)],check=True)
