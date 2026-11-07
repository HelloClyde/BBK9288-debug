"""Run D88MAP's actual entry in native GUI1 on a private copied-NAND QEMU.

No physical COM/disk, resident USB protocol, clock reconfiguration or RAM-speed
measurement is involved. The offline launch fixture injects only the program,
its owned NOLOAD arena initial contents, and a once-only native GUI callback.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools.test_service_native_model import Remote

HEAP_FIELDS=('version','bytes','result','start','end','data_top','directory_bottom','record_count',
 'allocated_records','free_records','allocated_payload','free_holes','largest_hole','tail_gap',
 'tail_capacity','max_request','free_capacity','metadata_bytes','directory_crc32')
ARENA_FIELDS=('version','structure_bytes','result','flags','base','arena_bytes','end','rounds_completed',
 'fill_words','verified_words','yield_calls','active_round','expected_crc1','expected_crc2',
 'actual_crc1','actual_crc2','first_bad_address','first_bad_expected','first_bad_actual','error_phase')


def sha(data):return hashlib.sha256(data).hexdigest()
def words(raw):return struct.unpack('<'+'I'*(len(raw)//4),raw)
def signed(value):return value if value<0x80000000 else value-0x100000000


def decode_and_validate(raw,manifest):
    assert len(raw)==manifest['report_bytes'] and len(raw)%4==0
    h=words(raw[:256]);assert h[:4]==(0x504d3844,1,len(raw),int(manifest['image_id'],16))
    assert h[4]==3 and h[5]==0x100 and not h[6],(h[4:9],signed(h[6]))
    assert h[7]==128 and h[8]==63 and h[18]==9,h[:24]
    assert zlib.crc32(raw[:-4])&0xffffffff==words(raw[-4:])[0]
    assert h[9]&0x710==0x10 and h[11]==0 and h[12]==0x02068b7c
    assert 0x02000000<=h[10]<0x02500000,'GUI stack unexpectedly overlaps heap/download'
    sections={};ranges=[]
    for i in range(h[18]):
        kind,offset,size,count=h[24+4*i:28+4*i]
        assert kind not in sections and 1<=kind<=9 and offset>=256 and not offset&3
        assert offset+size<=len(raw)-4 and not size&3
        assert not any(offset<end and start<offset+size for start,end in ranges)
        ranges.append((offset,offset+size));sections[kind]=(raw[offset:offset+size],count)
    assert set(sections)==set(range(1,10))
    for kind,size,count in [(1,16384,1),(2,16384,1),(3,4096,128),(4,76,1),(5,76,1),
                            (6,24,1),(7,80,1),(8,648,9)]:
        assert len(sections[kind][0])==size and sections[kind][1]==count
    before=dict(zip(HEAP_FIELDS,words(sections[4][0])))
    after=dict(zip(HEAP_FIELDS,words(sections[5][0])))
    work=dict(zip(('version','bytes','result','address','allocation_bytes','active'),words(sections[6][0])))
    arena=dict(zip(ARENA_FIELDS,words(sections[7][0])))
    assert before==after and not before['result']
    assert before['start']==0x02500000 and before['end']==0x02700000
    assert work['version']==1 and work['bytes']==24 and not work['result'] and not work['active']
    assert work['allocation_bytes']==1572864,'Native preferred1.5MiB allocation must succeed for this acceptance'
    assert before['start']<=work['address'] and work['address']+work['allocation_bytes']<=before['end']
    assert h[20]==h[21]==1572864 and h[22]==786432 and h[19]==2359296
    assert arena['version']==1 and arena['structure_bytes']==80 and not arena['result'] and arena['flags']==7
    assert (arena['base'],arena['arena_bytes'],arena['end'])==(0x02740000,786432,0x02800000)
    assert arena['rounds_completed']==2 and arena['fill_words']==arena['verified_words']==393216
    assert arena['yield_calls']==192 and arena['active_round']==2 and arena['error_phase']==0
    assert not any(arena[name] for name in ('first_bad_address','first_bad_expected','first_bad_actual'))
    assert arena['expected_crc1']==arena['actual_crc1']==0xfa684127
    assert arena['expected_crc2']==arena['actual_crc2']==0x32911f49
    assert len(sections[9][0])==256*24 and sections[9][1]==before['record_count']
    pages=[]
    for i in range(128):
        fields=struct.unpack_from('<8I',sections[3][0],32*i)
        address,size,crc,zero,ff,first,last,reserved=fields
        assert address==0x02000000+i*65536 and size==65536 and zero+ff<=65536
        pages.append(dict(zip(('address','bytes','crc32','zero_bytes','ff_bytes','first_word','last_word','reserved'),fields)))
    prefill_crc=zlib.crc32(bytes([0x7b])*65536)&0xffffffff
    for page in pages[116:]:
        assert page['crc32']==prefill_crc and page['zero_bytes']==page['ff_bytes']==0
        assert page['first_word']==page['last_word']==0x7b7b7b7b,'NOLOAD arena unexpectedly cleared before explicit fill'
    iram_first,iram_second=sections[1][0],sections[2][0]
    iram={'first_crc32':f'{zlib.crc32(iram_first)&0xffffffff:08x}',
          'second_crc32':f'{zlib.crc32(iram_second)&0xffffffff:08x}',
          'changed_bytes':sum(a!=b for a,b in zip(iram_first,iram_second)),
          'first_sha256':sha(iram_first),'second_sha256':sha(iram_second),
          'snapshot_native_ticks':[h[14],h[15]],
          'boundary':'Direct native IRAM reads; mutable words may change. Empty bytes are not proof of free RAM.'}
    return {'header':list(h[:24]),'crc32':'passed','sha256':sha(raw),'heap_before':before,'heap_after':after,
            'heap_work':work,'arena':arena,'simultaneous_private_bytes':h[19],
            'simultaneous_private_mib':h[19]/1048576,'iram':iram,'pages':pages},sections


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=ROOT/'build/layout-native/build.json')
    parser.add_argument('--output',type=Path,default=ROOT/'build/memory-layout-research/native')
    parser.add_argument('--port',type=int,default=12393,help='Loopback GDB TCP port, never a serial port')
    parser.add_argument('--timeout',type=float,default=240)
    args=parser.parse_args()
    if not 1024<=args.port<=65535 or args.port in (12389,12391):
        raise ValueError('Choose an independent loopback GDB port, excluding12389/12391')
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    manifest=json.loads(args.build.read_text(encoding='utf-8'))
    assert manifest['native_hook'] and manifest['binary_bytes']<0x40000
    assert manifest['arena']=={'type':'SHT_NOBITS','base':0x02740000,'bytes':786432,'end':0x02800000,'included_in_file':False}
    symbols=manifest['symbols'];binary=Path(manifest['binary']).read_bytes()
    assert sha(binary)==manifest['binary_sha256']
    for name in ('DL_AppMain','guest_launcher_hook','guest_hook_status','guest_hook_result','d88_layout_result'):
        assert 0x02700000<=symbols[name]<0x02740000
    assert symbols['__bss_end']<0x02740000 and symbols['__payload_end']<=0x02740000
    assert symbols['arena_words']==0x02740000 and symbols['__app_arena_end']==0x02800000
    source=ROOT/'build/hardware-profile-emulator/native-no-update.raw'
    nand=out/'independent-native-no-update.raw'
    shutil.copyfile(source,nand)
    env=dict(os.environ);env['PATH']=r'C:\msys64\ucrt64\bin;'+env.get('PATH','')
    observation={'build':manifest,'gdb_loopback_port':args.port,'nand_source':str(source),
        'physical_device_touched':False,'native_speed_claim':False,'passed':False,
        'scope':'Actual DL_AppMain/App_Main in native GUI1 via one-time callback; native malloc/ROS; no resident preloaded and no physical USB. This fixture does not exercise KF2 file loading.'}
    log=(out/'qemu.stderr.log').open('w',encoding='utf-8')
    process=remote=None;responses=queue.Queue();sequence=0

    def qmp(command,arguments=None):
        nonlocal sequence
        sequence+=1;request={'execute':command,'id':sequence}
        if arguments is not None:request['arguments']=arguments
        process.stdin.write(json.dumps(request)+'\n');process.stdin.flush()
        while True:
            reply=responses.get(timeout=15)
            if reply.get('id')==sequence:
                if 'error' in reply:raise RuntimeError(reply)
                return reply.get('return')

    def reader():
        for line in process.stdout:
            try:responses.put(json.loads(line))
            except ValueError:pass

    def read(address,size):
        value=b''.join(remote.read(address+off,min(2048,size-off)) for off in range(0,size,2048))
        assert len(value)==size
        return value

    def screenshot(name):
        ppm=out/f'{name}.ppm';png=out/f'{name}.png'
        qmp('human-monitor-command',{'command-line':f'screendump {ppm.as_posix()}'})
        from PIL import Image
        with Image.open(ppm) as image:image.save(png)
        return {'path':str(png),'sha256':sha(png.read_bytes())}

    def state():
        return {'heap':remote.words(0x02069000,5),'current_tcb':remote.words(0x02068664,1)[0],
                'ros_flags':read(0x0206866c,4).hex(),'callback':remote.words(0x0203857c,1)[0],
                'native_tick':remote.words(0x02068674,1)[0]}

    try:
        process=subprocess.Popen([str(ROOT.parent/'bbk9288-qemu/_build/qemu-system-s1c33.exe'),
            '-M',f'bbk9288,usb-connected=off,nand-image={nand.as_posix()},nand-id=0xADDA8015',
            '-cpu','c33l05,exit-on-halt=off','-bios',str(ROOT.parent/'9288-boot-capture/captures/2026-10-01-v3/BOOT0.BIN'),
            '-display','none','-serial','none','-monitor','none','-qmp','stdio','-no-shutdown',
            '-gdb',f'tcp:127.0.0.1:{args.port}'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,
            text=True,encoding='utf-8',env=env)
        observation['qemu_pid']=process.pid
        threading.Thread(target=reader,daemon=True).start();responses.get(timeout=10);qmp('qmp_capabilities')
        time.sleep(8);remote=Remote(args.port,lambda:qmp('stop'));remote.stop()
        observation['before']=state();assert observation['before']['callback']==0
        remote.write(0x02700000,binary)
        remote.write(0x02740000,bytes([0x7b])*786432)
        remote.write(0x0203857c,struct.pack('<I',symbols['guest_launcher_hook']))
        remote.resume();qmp('human-monitor-command',{'command-line':'sendkey ret 300'})
        observation['begin_confirmations']=0
        until=time.monotonic()+args.timeout;last=None
        while time.monotonic()<until:
            time.sleep(0.5);remote.stop()
            hook=remote.words(symbols['guest_hook_status'],2)
            h=remote.words(manifest['report_address'],24)
            if h[0]==0x504d3844:
                progress=(h[4],h[5],h[7],h[19])
                if progress!=last:
                    observation.setdefault('progress',[]).append({'state':h[4],'phase':h[5],'pages':h[7],
                        'private_bytes':h[19],'host_monotonic':time.monotonic()})
                    print('native layout',progress,flush=True);last=progress
                if h[4] in (3,4):break
            elif hook[0]==1 and not observation['begin_confirmations']:
                observation['start_screenshot']=screenshot('native-start')
                observation['begin_confirmations']=1
                remote.resume();qmp('human-monitor-command',{'command-line':'sendkey ret 300'});continue
            elif hook[0]==2:
                raise AssertionError(f'Native entry returned before report: hook={hook}, header={h}')
            remote.resume()
        else:raise TimeoutError('Native layout probe did not complete')
        raw=read(manifest['report_address'],manifest['report_bytes']);(out/'D88MAP.BIN').write_bytes(raw)
        report,sections=decode_and_validate(raw,manifest);observation['report']=report
        for kind,name in ((1,'iram-first.bin'),(2,'iram-second.bin')):(out/name).write_bytes(sections[kind][0])
        final_arena=read(0x02740000,786432)
        assert zlib.crc32(final_arena)&0xffffffff==0x32911f49
        for index,(value,) in enumerate(struct.iter_unpack('<I',final_arena)):
            expected=~(((index^(index<<9)^(index>>7))+0xa55a3cc3)&0xffffffff)&0xffffffff
            assert value==expected,(index,value,expected)
        observation['final_arena_readback']={'bytes':len(final_arena),'crc32':'32911f49','sha256':sha(final_arena),
                                            'word_pattern_verified':True}
        observation['done_screenshot']=screenshot('native-done')
        observation['at_done']=state();observation['registers_at_done']=qmp('human-monitor-command',{'command-line':'info registers'})
        remote.resume();qmp('human-monitor-command',{'command-line':'sendkey ret 300'})
        until=time.monotonic()+10
        while time.monotonic()<until:
            time.sleep(0.5);remote.stop()
            hook=remote.words(symbols['guest_hook_status'],2)
            if hook[0]==2:break
            remote.resume()
        assert hook==[2,0],hook
        observation['hook_after_return']=hook;observation['after_return']=state()
        assert observation['after_return']['callback']==0
        observation['menu_screenshot']=screenshot('native-return')
        observation['passed']=True
        print(json.dumps({'native_passed':True,'simultaneous_bytes':report['simultaneous_private_bytes'],
            'heap_bytes':report['heap_work']['allocation_bytes'],'arena_bytes':report['arena']['arena_bytes'],
            'heap_restored':report['heap_before']==report['heap_after'],'iram':report['iram']},indent=2),flush=True)
    except BaseException as error:
        observation['error']={'type':type(error).__name__,'message':str(error)}
        if remote is not None:
            try:remote.stop();observation['failure_state']=state();observation['failure_registers']=qmp('human-monitor-command',{'command-line':'info registers'})
            except Exception:pass
        raise
    finally:
        if remote is not None:remote.close()
        if process is not None and process.poll() is None:
            try:qmp('quit');process.wait(timeout=5)
            except Exception:process.terminate();process.wait(timeout=5)
        observation['qemu_exit_code']=process.returncode if process is not None else None
        log.close();(out/'observations.json').write_text(json.dumps(observation,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
