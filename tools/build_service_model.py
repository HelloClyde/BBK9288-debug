"""Offline native-ROS integration image; never writes dist or a device disk."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from build import headers,run

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',type=Path,default=Path(r'D:\Downloads\app_env_9288'))
    parser.add_argument('--toolchain',type=Path,default=ROOT.parent/'_tools/s1c33-9288')
    args=parser.parse_args();toolchain=args.toolchain.resolve()
    output=ROOT/'build/native-service-model';output.mkdir(parents=True,exist_ok=True)
    headers(args.sdk.resolve(),output/'sdk-headers')
    include=[output/'sdk-headers',ROOT/'device/compat',ROOT/'device',output]
    definitions=['DL_DOWN','_RLS_','DEBUG_IMAGE_ID=0x4d4f4431u',
                 'D9288_USB_PLATFORM_VERIFIED=1','D9288_USB_TIMER_GUARDED=1',
                 'USB_CDC_TEST=1','USB_PLATFORM_TEST=1',
                 'USB_PLATFORM_TEST_NATIVE_GUEST=1']
    # Test-only handoff runs after the owned desktop callback clears busy.
    # Never overwrite an active resident hook to invoke the launcher again.
    # Keep the production sources untouched and hash these generated inputs.
    model_sources=output/'model-sources';model_sources.mkdir(exist_ok=True)
    gui_source=(ROOT/'device/gui_jobs.c').read_text(encoding='utf8')
    anchor='    (void)run_one_locked();\n    return 0;\n}'
    assert gui_source.count(anchor)==1,'GUI hook handoff anchor changed'
    gui_source='extern void guest_after_gui_job(void);\n'+gui_source.replace(
        anchor,'    (void)run_one_locked();\n    guest_after_gui_job();\n    return 0;\n}')
    model_gui=model_sources/'gui_jobs.c';model_gui.write_text(gui_source,encoding='utf8')
    usb_source=(ROOT/'tests/native_usb_guest_model.c').read_text(encoding='utf8')
    anchor='u8 usb_test_read(unsigned address)\n{'
    assert usb_source.count(anchor)==1,'guest USB worker mailbox anchor changed'
    usb_source='extern void guest_file_worker_tick(void);\n'+usb_source.replace(
        anchor,anchor+'\n    guest_file_worker_tick();')
    model_usb=model_sources/'native_usb_guest_model.c';model_usb.write_text(usb_source,encoding='utf8')
    # Observe private ownership without assuming a stale structure layout.
    file_source=(ROOT/'device/file_service.c').read_text(encoding='utf8')
    file_source+='\ntypedef __typeof__(tx) guest_file_tx_type;\nconst unsigned guest_file_layout[4]={'+\
        '__builtin_offsetof(guest_file_tx_type,stream),'+\
        '__builtin_offsetof(guest_file_tx_type,temp),'+\
        '__builtin_offsetof(guest_file_tx_type,temp_owned),sizeof(tx)};\n'
    model_file=model_sources/'file_service.c';model_file.write_text(file_source,encoding='utf8')
    sources=[ROOT/'device'/name for name in
             ['resident_runtime.c','debug_core.c','usb_cdc.c','usb_platform.c','usb_delay.c','input_jobs.c','power_idle.c','gui_jobs.c','file_native.c','file_service.c','file_runtime.c','usb_timer_guard.S']]
    sources=[model_gui if path.name=='gui_jobs.c' else model_file if path.name=='file_service.c' else path
             for path in sources]
    sources.extend([model_usb,ROOT/'tests/native_file_guest.c'])
    cmd=[sys.executable,ROOT/'tools/build_resident.py','--toolchain',toolchain,
         '--output',output/'resident','--header',output/'resident_blob.h']
    for source in sources:cmd.extend(['--source',source])
    for directory in include:cmd.extend(['--include',directory])
    for define in definitions:cmd.extend(['--define',define])
    for symbol in ['resident_control','resident_worker','resident_start',
                   'resident_request_stop','guest_model_counts','guest_model_fault','usb_platform_check',
                   'usb_platform_timer_guard','guest_model_vbus','guest_model_signals',
                   'guest_misc_counts','guest_misc_clock_signals','usb_diagnostic','usb_platform_misc_failure',
                   'device_descriptor','usb_test_read','power_idle_diagnostic',
                   'd88_files_gui_progress','files_diagnostic','gui_hook',
                   'guest_file_mailbox','guest_file_input','guest_file_output','guest_file_handoff','tx',
                   'guest_file_layout']:
        cmd.extend(['--export',symbol])
    run(cmd)
    clang=toolchain/'bin/clang.exe'
    flags=['--target=s1c33-none-elf','-Os','-ffreestanding','-fno-builtin',
           '-fno-jump-tables','-fomit-frame-pointer','-fno-strict-aliasing',
           '-Wall','-Wextra','-Wno-nonportable-include-path','-Wno-int-to-pointer-cast']
    for directory in include:flags.extend(['-I',directory])
    for define in definitions:flags.append('-D'+define)
    objects=[]
    launcher=[ROOT/'device'/name for name in ['startup.c','launcher.c','resident_loader.c','usb_log.c']]
    launcher.append(ROOT/'tests/native_service_launcher_hook.c')
    for source in launcher:
        obj=output/(source.stem+'.o');run([clang,*flags,'-c',source,'-o',obj]);objects.append(obj)
    elf=output/'native-service-model.elf'
    run([clang,'--target=s1c33-none-elf','-nostdlib',f'-Wl,-T,{ROOT/"device/link.ld"}',
         f'-Wl,-Map,{output/"native-service-model.map"}',*objects,'-o',elf])
    binary=output/'native-service-model.bin'
    run([toolchain/'bin/llvm-objcopy.exe','-O','binary','--gap-fill','255',elf,binary])
    # Symbols allow the runner to set the one-shot native GUI hook and inspect it.
    sys.path.insert(0,str(ROOT/'tools'))
    from elftools.elf.elffile import ELFFile
    with elf.open('rb') as handle:
        table=ELFFile(handle).get_section_by_name('.symtab')
        wanted={'guest_launcher_hook','guest_hook_status','guest_hook_result','DL_AppMain',
                'usb_log_last_report','usb_log_last_result','usb_log_last_path','usb_last_dialog_state','saved_report'}
        symbols={s.name:int(s['st_value']) for s in table.iter_symbols() if s.name in wanted}
    (output/'launcher-symbols.json').write_text(json.dumps(symbols,indent=2)+'\n')
    inputs=sources+launcher+list((ROOT/'device').rglob('*.h'))+[
        ROOT/'device/gui_jobs.c',ROOT/'device/file_service.c',ROOT/'tests/native_usb_guest_model.c',
        ROOT/'tools/build_resident.py',Path(__file__).resolve()]
    record={'model_image_id':'4d4f4431','usb_backend':'guest USB registers/SRAM and Misc one-write protection; VBUS high (native QEMU USB remains disconnected)',
            'ros_backend':'actual captured firmware syscalls',
            'sources':{str(path.relative_to(ROOT)).replace('\\','/'):hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in sorted(set(inputs))},
            'launcher_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
            'compiler_sha256':hashlib.sha256(clang.read_bytes()).hexdigest()}
    record['source_sha256']=hashlib.sha256(json.dumps(record['sources'],sort_keys=True,
                                                   separators=(',',':')).encode()).hexdigest()
    (output/'model-build.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'binary':str(binary),'symbols':symbols},indent=2))

if __name__=='__main__':main()
