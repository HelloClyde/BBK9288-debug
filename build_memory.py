"""Build independent D88MEM; does not rebuild or replace the resident debugger."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

from build import headers,run

ROOT=Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,default=Path(r'D:\Downloads\app_env_9288'))
    p.add_argument('--toolchain',type=Path,default=ROOT.parent/'_tools/s1c33-9288')
    a=p.parse_args();sdk=a.sdk.resolve();tc=a.toolchain.resolve()
    out=ROOT/'build/memory';out.mkdir(parents=True,exist_ok=True)
    dist=ROOT/'dist';dist.mkdir(exist_ok=True)
    headers(sdk,out/'sdk-headers')
    sources=[ROOT/'device/startup.c',ROOT/'device/power_idle.c']+[ROOT/'memory'/n for n in ['main.c','memory_heap.c','memory_map.c','memory_speed.c']]
    inputs=sources+list((ROOT/'memory').glob('*.h'))+[ROOT/'device/profiles.h',ROOT/'device/power_idle.h',ROOT/'device/link.ld',Path(__file__).resolve()]
    digest=hashlib.sha256()
    for path in sorted(set(inputs)):
        digest.update(path.relative_to(ROOT).as_posix().encode()+b'\0'+path.read_bytes())
    image_id=zlib.crc32(digest.digest())
    sys.path.insert(0,str(ROOT/'tools'))
    from build_resident import verify_abi
    clang=tc/'bin/clang.exe';verify_abi(clang)
    flags=['--target=s1c33-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-jump-tables',
           '-fomit-frame-pointer','-fno-strict-aliasing','-fno-delete-null-pointer-checks',
           '-Wall','-Wextra','-Werror','-Wno-int-to-pointer-cast','-Wno-nonportable-include-path',
           '-DDL_DOWN','-D_RLS_',f'-DMEMORY_IMAGE_ID={image_id}u']
    for include in [ROOT/'device/compat',out/'sdk-headers',ROOT/'device',ROOT/'memory']:
        flags.extend(['-I',include])
    objects=[]
    for i,source in enumerate(sources):
        obj=out/f'{i:02d}-{source.stem}.o'
        run([clang,*flags,'-c',source,'-o',obj]);objects.append(obj)
        if source.name=='memory_speed.c':run([clang,*flags,'-S',source,'-o',out/'memory_speed.s'])
    elf=out/'9288MemoryProbe.elf'
    run([clang,'--target=s1c33-none-elf','-nostdlib',f'-Wl,-T,{ROOT/"device/link.ld"}',
         f'-Wl,-Map,{out/"9288MemoryProbe.map"}',*objects,'-o',elf])
    binary=out/'9288MemoryProbe.bin'
    run([tc/'bin/llvm-objcopy.exe','-O','binary','--gap-fill','255',elf,binary])
    app=dist/'9288MemoryProbe.exe'
    run([sdk/'apmk/gen_ap_kf2.exe','-module','9','-icon1',sdk/'apmk/ico1.bin','-icon2',sdk/'apmk/ico2.bin',
         '-name','D88MEM','-i',binary,'-o',app])
    from elftools.elf.elffile import ELFFile
    with elf.open('rb') as f:
        symbols={s.name:int(s['st_value']) for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()
                 if s.name in ['d88_memory_report','d88_memory_run','DL_AppMain','__bss_start','__bss_end','__payload_end']}
    assert symbols['d88_memory_report']>=0x02700000 and symbols['d88_memory_report']+8192<=0x02800000
    sha=hashlib.sha256(app.read_bytes()).hexdigest()
    app.with_suffix('.exe.sha256').write_text(f'{sha}  {app.name}\n',encoding='ascii')
    result={'file':app.name,'menu_title':'D88MEM','size':app.stat().st_size,'sha256':sha,
            'image_id':f'{image_id:08x}','source_sha256':digest.hexdigest(),
            'target':'hardware-capture-0215ebc4','report_address':symbols['d88_memory_report'],
            'report_bytes':8192,'symbols':symbols,'hardware_verified':False,
            'source_files':{path.relative_to(ROOT).as_posix():hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(set(inputs))},
            'debugger_updated':False,'timing':'device RTC ticks; nominal frequency separately identified; no USB-throughput substitution',
            'writes':'only private native malloc block; 128KiB GUI reserve; block released before report completion'}
    (dist/'memory-build.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
