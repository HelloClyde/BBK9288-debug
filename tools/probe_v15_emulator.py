"""Read the original V1.5 kernel's live USB state from an isolated QEMU NAND."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

from test_service_native_model import Remote


def relocate_ros_body(body, source, target):
    """Re-encode external C33 branches in the emulator fixture's copied code."""
    words=list(struct.unpack(f'<{len(body)//2}H',body))
    prefixes=[]
    for index, opcode in enumerate(words):
        if opcode&0xe000==0xc000:
            prefixes.append(index)
            continue
        if 0x800<=opcode<=0x1fff:
            bits=9 if not prefixes else 22 if len(prefixes)==1 else 32
            displacement=(opcode&255)<<1
            if prefixes:
                displacement|=(words[prefixes[-1]]&8191)<<9
            if len(prefixes)==2:
                displacement|=((words[prefixes[0]]&8191)>>3)<<22
            sign=1<<(bits-1)
            displacement=(displacement^sign)-sign
            if not 0<=index*2+displacement<len(body):
                displacement+=source-target
                assert -sign<=displacement<sign,(hex(source),index,displacement)
                displacement&=(1<<bits)-1
                words[index]=opcode&0xff00 | (displacement>>1)&255
                if prefixes:
                    words[prefixes[-1]]=0xc000 | (displacement>>9)&8191
                if len(prefixes)==2:
                    words[prefixes[0]]=0xc000 | ((displacement>>22)<<3) | (words[prefixes[0]]&7)
        prefixes=[]
    return struct.pack(f'<{len(words)}H',*words)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('emulator', 'boot-rom', 'source-nand', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--port', type=int, default=12394)
    parser.add_argument('--ros-guest', type=Path, help='Run the isolated USB task/IRQ suspend-resume fixture')
    parser.add_argument('--ros-entry', type=lambda value: int(value,0), default=0x02701800)
    parser.add_argument('--resident-model', type=Path)
    parser.add_argument('--relocate-ros-table', action='store_true',
                        help='Force the unknown-fingerprint fallback in the isolated emulator')
    parser.add_argument('--relocate-ros-code', action='store_true')
    parser.add_argument('--test-keys', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    nand = args.output / 'v15-probe.raw'
    if nand.exists():
        parser.error('Choose a fresh output directory for the private NAND copy')
    shutil.copyfile(args.source_nand, nand)
    env = dict(os.environ)
    env['PATH'] = r'C:\msys64\ucrt64\bin;' + env.get('PATH', '')
    with (args.output / 'qemu.log').open('w') as log, subprocess.Popen([
        str(args.emulator), '-M', f'bbk9288,usb-connected=off,nand-image={nand.as_posix()},nand-id=0xADDA8015',
        '-cpu', 'c33l05,exit-on-halt=off', '-bios', str(args.boot_rom), '-display', 'none',
        '-serial', 'none', '-monitor', 'none', '-qmp', 'stdio', '-gdb', f'tcp:127.0.0.1:{args.port}',
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, env=env) as process:
        def qmp(command, arguments=None):
            request={'execute':command,'id':command}
            if arguments is not None:
                request['arguments']=arguments
            process.stdin.write(json.dumps(request) + '\n')
            process.stdin.flush()
            for line in process.stdout:
                message = json.loads(line)
                if message.get('id') == command:
                    if 'error' in message:
                        raise RuntimeError(message['error'])
                    return
            raise RuntimeError('Emulator closed QMP')

        try:
            qmp('qmp_capabilities')
            time.sleep(10)
            if args.test_keys:
                for key in ('ret','esc','f5','9'):
                    qmp('human-monitor-command',{'command-line':f'sendkey {key} 120'})
                    time.sleep(.4)
            remote = Remote(args.port, lambda: qmp('stop'))
            try:
                remote.stop()
                def word(address):
                    return remote.words(address, 1)[0]

                roots = remote.words(0x02000200, 6)
                assert roots == [0x021806ec, 0x02180058, 0x0217ffe4, 0x0217fed4, 0x0200021c, 0x0217ff40], roots
                tcb = word(0x02240d88)
                assert 0x02000000 <= tcb <= 0x027fffff - 9 * 72, hex(tcb)
                task3 = remote.read(tcb + 3 * 72, 72)
                entry = int.from_bytes(task3[24:28], 'little')
                assert entry == 0x02133648, hex(entry)
                snapshot = {
                    'roots': roots, 'ros_entries': [word(roots[0] + 4*i) for i in (9, 10, 23, 24)],
                    'tcb': tcb, 'usb_task': task3.hex(), 'slot8': remote.read(tcb + 8 * 72, 72).hex(),
                    'timer3b_vector': word(0x020000a8), 'heap_words': remote.words(0x022a9fac, 5),
                    'usb_globals': {hex(address): word(address) for address in
                                    (0x024ef004, 0x0228a60c, 0x0228a654, 0x0228a610, 0x022411b4)},
                }
                if args.ros_guest:
                    base=0x02700000
                    # objcopy starts this fixture's flat image at .text.
                    remote.write(0x02701800,args.ros_guest.read_bytes())
                    remote.write(base+0x8000,bytes(36))
                    registers=bytearray.fromhex(remote.command('g'))
                    assert len(registers)==21*4,len(registers)
                    struct.pack_into('<I',registers,16*4,args.ros_entry)  # PC
                    struct.pack_into('<I',registers,18*4,base+0xa000)  # SP
                    assert remote.command('G'+registers.hex())=='OK'
                    remote.resume()
                    time.sleep(0.5)
                    remote.stop()
                    report=remote.words(base+0x8000,9)
                    print('V1.5 native USB task/IRQ report',report,flush=True)
                    assert report[0:3]==[2,0,15] and base<=report[3]<base+0x8000,report
                    assert report[4:]==[0,7,0x02133df4,1,0],report
                    snapshot['native_ros_usb_restore']=report
                if args.resident_model:
                    model=args.resident_model
                    symbols=json.loads((model/'launcher-symbols.json').read_text())
                    exports=json.loads((model/'resident/resident.json').read_text())['exports']
                    print('V1.5 scheduling baseline',remote.words(0x022a9610,5),hex(word(0x02240db8)),flush=True)
                    remote.write(0x02700000,(model/'native-service-model.bin').read_bytes())
                    caller_registers=bytearray.fromhex(remote.command('g'))
                    caller=word(0x022a9610)
                    if args.relocate_ros_table:
                        table=0x02780000
                        remote.write(table,remote.read(roots[0],55*4))
                        remote.write(0x02000200,struct.pack('<I',table))
                        snapshot['relocated_ros_table']=table
                    else:
                        table=roots[0]
                    moved=[]
                    if args.relocate_ros_code:
                        for index,(slot,size) in enumerate(zip((9,10,23,24),(114,114,42,58))):
                            original=word(table+4*slot)
                            candidate=0x022f0000+0x100*index
                            body=relocate_ros_body(remote.read(original,size),original,candidate)
                            remote.write(candidate,body)
                            remote.write(table+4*slot,struct.pack('<I',candidate))
                            moved.append((original,candidate))
                        snapshot['relocated_ros_code']=moved
                    saved_irqs=[remote.read(address,1)[0]&0x40 for address in (0x00040273,0x000402a6)]

                    def invoke_launcher(expected=0, delay=9):
                        registers=bytearray(caller_registers)
                        struct.pack_into('<I',registers,16*4,symbols['guest_launch_v15'])
                        struct.pack_into('<I',registers,18*4,0x0273f000)
                        for _ in range(80):
                            if word(0x022a9610)==caller:
                                break
                            remote.resume()
                            time.sleep(.025)
                            remote.stop()
                        else:
                            raise AssertionError('Original launcher task did not resume')
                        remote.write(symbols['guest_hook_status'],bytes(4))
                        assert remote.command('G'+registers.hex())=='OK'
                        for remaining in (delay, 5, 5, 5, 5):
                            remote.resume()
                            time.sleep(remaining)
                            remote.stop()
                            result=remote.words(symbols['guest_hook_status'],2)
                            if result[0]==2:
                                break
                        assert result==[2,expected & 0xffffffff],result

                    if args.relocate_ros_table or moved:
                        original_entry=remote.read(table+4*24,4)
                        remote.write(table+4*24,bytes(4))
                        invoke_launcher(-1,1)
                        assert word(tcb+8*72+24)==0
                        assert word(0x020000a8)==snapshot['timer3b_vector']
                        remote.write(table+4*24,original_entry)
                        snapshot['incompatible_table_rejected']=True
                    if moved:
                        candidate=moved[-1][1]
                        original_byte=remote.read(candidate,1)
                        remote.write(candidate,bytes([original_byte[0]^1]))
                        invoke_launcher(-1,1)
                        assert word(tcb+8*72+24)==0
                        assert word(0x020000a8)==snapshot['timer3b_vector']
                        remote.write(candidate,original_byte)
                        snapshot['incompatible_code_rejected']=True

                    snapshot['resident_cycles']=[]
                    for cycle in range(1,3):
                        invoke_launcher()
                        worker=word(tcb+8*72+24)
                        assert worker
                        base=worker-exports['resident_worker']
                        control=base+exports['resident_control']
                        attached=remote.words(control,17)
                        assert attached[9]==2 and attached[12]==cycle and attached[13]>0,attached
                        assert remote.read(tcb+8*72+60,2)==bytes([4,4])
                        assert word(0x020000a8)==base+exports['usb_platform_timer_guard']
                        assert remote.words(base+exports['guest_model_counts'],8)[7]==cycle
                        if args.test_keys and cycle==1:
                            mailbox=base+exports['guest_v15_key']
                            def framebuffer():
                                return b''.join(remote.read(0x003c0000+offset,640)
                                                for offset in range(0,19200,640))
                            images=[framebuffer()]
                            qmp('screendump',{'filename':str((args.output/'key-before.ppm').resolve())})
                            for seq,key in enumerate((106,105),1):
                                remote.write(mailbox+8,struct.pack('<I',key))
                                remote.write(mailbox,struct.pack('<I',seq))
                                remote.resume()
                                time.sleep(1.5)
                                remote.stop()
                                reply=remote.words(mailbox,6)
                                assert reply[1:]==[seq,key,0,0,0],reply
                                assert remote.words(control,17)[9]==2
                                images.append(framebuffer())
                                qmp('screendump',{'filename':str((args.output/f'key-{seq}.ppm').resolve())})
                            print('V1.5 changed framebuffer bytes',
                                  [sum(a!=b for a,b in zip(x,y)) for x,y in zip(images,images[1:])],flush=True)
                            assert images[0]!=images[1] and images[1]!=images[2], 'GUI ignored direction keys'
                            snapshot['native_gui_keys']=['RIGHT','LEFT']
                            print('V1.5 native GUI LEFT/RIGHT changed the screen while USB stayed running',flush=True)
                        # The worker wakes frequently at priority five. Re-enter
                        # the launcher only from the original foreground task.
                        invoke_launcher()
                        stopped=remote.words(control,17)
                        assert stopped[9]==5 and stopped[10]==0,stopped
                        assert word(0x020000a8)==snapshot['timer3b_vector']
                        assert remote.read(tcb+3*72+63,1)==task3[63:64]
                        assert remote.read(0x00300f20,1)==b'\x27'
                        assert [remote.read(address,1)[0]&0x40 for address in (0x00040273,0x000402a6)]==saved_irqs
                        assert remote.words(base+exports['guest_misc_clock_signals'],6)[4]==0
                        print(f'V1.5 lifecycle {cycle}: attach, launcher stop, native restore passed',flush=True)
                        snapshot['resident_cycles'].append({'running':attached,'stopped':stopped})
                (args.output / 'v15-live.json').write_text(json.dumps(snapshot, indent=2) + '\n')
                print(json.dumps(snapshot, indent=2))
            finally:
                remote.close()
        finally:
            if process.poll() is None:
                process.terminate()


if __name__ == '__main__':
    main()
