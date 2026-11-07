"""Build independent D88MAP; system RAM/IRAM are read only, test buffers owned."""
from pathlib import Path
import argparse,hashlib,json,sys,zlib
from build import headers,run
ROOT=Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,default=Path(r'D:\Downloads\app_env_9288'))
    p.add_argument('--toolchain',type=Path,default=ROOT.parent/'_tools/s1c33-9288')
    p.add_argument('--native-hook',action='store_true',help='Independent emulator fixture; never writes dist')
    a=p.parse_args();tc=a.toolchain.resolve();sdk=a.sdk.resolve()
    out=ROOT/'build'/('layout-native' if a.native_hook else 'layout');out.mkdir(parents=True,exist_ok=True)
    headers(sdk,out/'sdk-headers')
    sources=[ROOT/x for x in ['device/startup.c','device/power_idle.c','memory/memory_heap.c',
                             'memory_layout/main.c','memory_layout/static_arena.c']]
    if a.native_hook:sources.append(ROOT/'tests/native_service_launcher_hook.c')
    inputs=sources+list((ROOT/'memory_layout').glob('*.h'))+[ROOT/x for x in
        ['memory/memory_heap.h','device/power_idle.h','device/profiles.h','memory_layout/link.ld','build_layout.py']]
    digest=hashlib.sha256()
    for path in sorted(set(inputs)):digest.update(path.relative_to(ROOT).as_posix().encode()+b'\0'+path.read_bytes())
    image=zlib.crc32(digest.digest());clang=tc/'bin/clang.exe'
    flags=['--target=s1c33-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-jump-tables',
           '-fomit-frame-pointer','-fno-strict-aliasing','-fno-delete-null-pointer-checks',
           '-Wall','-Wextra','-Werror','-Wno-int-to-pointer-cast','-Wno-nonportable-include-path',
           '-DDL_DOWN','-D_RLS_',f'-DLAYOUT_IMAGE_ID={image}u']
    for path in [ROOT/'device/compat',out/'sdk-headers',ROOT/'device',ROOT/'memory',ROOT/'memory_layout']:
        flags.extend(['-I',path])
    objects=[]
    for i,source in enumerate(sources):
        obj=out/f'{i:02d}-{source.stem}.o';run([clang,*flags,'-c',source,'-o',obj]);objects.append(obj)
    elf=out/'9288LayoutProbe.elf'
    run([clang,'--target=s1c33-none-elf','-nostdlib',f'-Wl,-T,{ROOT/"memory_layout/link.ld"}',
         f'-Wl,-Map,{out/"9288LayoutProbe.map"}',*objects,'-o',elf])
    binary=out/'9288LayoutProbe.bin';run([tc/'bin/llvm-objcopy.exe','-O','binary','--gap-fill','255',elf,binary])
    from elftools.elf.elffile import ELFFile
    with elf.open('rb') as f:
        e=ELFFile(f);arena=e.get_section_by_name('.app_arena');table=e.get_section_by_name('.symtab')
        symbols={s.name:int(s['st_value']) for s in table.iter_symbols() if s.name and s['st_shndx']!='SHN_UNDEF'}
        report=next(s for s in table.iter_symbols() if s.name=='d88_layout_result')
        assert arena['sh_type']=='SHT_NOBITS' and arena['sh_addr']==0x02740000 and arena['sh_size']==786432
        assert symbols['__bss_end']<0x02740000 and symbols['__payload_end']<=0x02740000
        assert symbols['__app_arena_end']==0x02800000 and binary.stat().st_size<262144
        assert not [s.name for s in table.iter_symbols() if s.name and s['st_shndx']=='SHN_UNDEF']
        report_bytes=int(report['st_size'])
    result={'image_id':f'{image:08x}','source_sha256':digest.hexdigest(),'target':'hardware-capture-0215ebc4',
        'binary':str(binary),'binary_bytes':binary.stat().st_size,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'report_address':symbols['d88_layout_result'],'report_bytes':report_bytes,'symbols':symbols,
        'native_hook':a.native_hook,'arena':{'type':'SHT_NOBITS','base':0x02740000,'bytes':786432,'end':0x02800000,'included_in_file':False},
        'source_files':{path.relative_to(ROOT).as_posix():hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(set(inputs))},
        'writes':'only own download image/768KiB NOLOAD arena and guarded malloc block; IRAM/system SDRAM read only',
        'hardware_verified':False}
    if not a.native_hook:
        dist=ROOT/'dist';dist.mkdir(exist_ok=True);app=dist/'9288LayoutProbe.exe'
        run([sdk/'apmk/gen_ap_kf2.exe','-module','9','-icon1',sdk/'apmk/ico1.bin','-icon2',sdk/'apmk/ico2.bin',
             '-name','D88MAP','-i',binary,'-o',app])
        result.update(file=app.name,menu_title='D88MAP',size=app.stat().st_size,sha256=hashlib.sha256(app.read_bytes()).hexdigest())
        app.with_suffix('.exe.sha256').write_text(f'{result["sha256"]}  {app.name}\n',encoding='ascii')
        (dist/'layout-build.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    (out/'build.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:result[k] for k in ['image_id','binary_bytes','report_address','report_bytes','arena']},indent=2))
if __name__=='__main__':main()
