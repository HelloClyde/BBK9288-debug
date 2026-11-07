"""Run the real launcher/resident on captured ROS with guest USB registers.

Only an independent NAND copy and a loopback QEMU GDB endpoint are touched.
No USB host/device transport or physical disk is used by this regression.
"""
from __future__ import annotations
import argparse,json,os,queue,shutil,socket,struct,subprocess,threading,time,sys,zlib
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/native-service-model'
FILE_ORIGINAL=bytes(range(251))*83
FILE_REPLACEMENT=b'native replacement\0'+bytes(range(239))*89
sys.path.insert(0,str(ROOT))
from inspect_usb_log import decode as decode_usb_log

class Remote:
    def __init__(self,port,pause):
        self.s=socket.create_connection(('127.0.0.1',port),timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        self.running=True
        self.pause=pause
        self.command('qSupported:multiprocess+;xmlRegisters=i386')
    def response(self,started=False):
        if not started:
            while True:
                byte=self.s.recv(1)
                if byte==b'$':break
                if not byte:raise RuntimeError('GDB closed')
        data=b''
        while True:
            byte=self.s.recv(1)
            if byte==b'#':break
            if not byte:raise RuntimeError('GDB closed')
            data+=byte
        checksum=self.s.recv(2)
        assert int(checksum,16)==sum(data)%256
        self.s.sendall(b'+')
        return data.decode()
    def command(self,value,response=True):
        raw=value.encode();self.s.sendall(b'$'+raw+b'#'+f'{sum(raw)%256:02x}'.encode())
        ack=self.s.recv(1)
        while ack==b'$':
            self.response(True);ack=self.s.recv(1)
        assert ack==b'+',ack
        return self.response() if response else None
    def stop(self):
        if self.running:
            self.pause();self.command('?');self.running=False
    def resume(self):
        self.command('c',False);self.running=True
    def read(self,address,size):
        value=self.command(f'm{address:x},{size:x}')
        if value.startswith('E'):raise RuntimeError(value)
        return bytes.fromhex(value)
    def write(self,address,data):
        for off in range(0,len(data),1024):
            part=data[off:off+1024]
            assert self.command(f'M{address+off:x},{len(part):x}:'+part.hex())=='OK'
    def words(self,address,count):
        return list(struct.unpack('<'+'I'*count,self.read(address,count*4)))
    def close(self):self.s.close()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',type=int,default=12389)
    parser.add_argument('--break-at',type=lambda x:int(x,0))
    parser.add_argument('--arm-after',type=lambda x:int(x,0))
    parser.add_argument('--diagnostic-restore-tcb',action='store_true',
        help='diagnostic only: repair native STA scratch/current pointer via GDB before dispatcher')
    parser.add_argument('--watch-timer-enable',action='store_true')
    parser.add_argument('--files',action='store_true',
        help='exercise actual FS through the GUI bridge on a fresh independent NAND copy')
    args=parser.parse_args()
    OUT.mkdir(parents=True,exist_ok=True)
    nand=OUT/('independent-files-native-no-update.raw' if args.files else 'independent-native-no-update.raw')
    if args.files or not nand.exists():shutil.copyfile(ROOT/'build/hardware-profile-emulator/native-no-update.raw',nand)
    symbols=json.loads((OUT/'launcher-symbols.json').read_text())
    manifest=json.loads((OUT/'resident/resident.json').read_text())
    env=dict(os.environ);env['PATH']=r'C:\msys64\ucrt64\bin;'+env.get('PATH','')
    log=(OUT/'qemu.stderr.log').open('w',encoding='utf8')
    p=subprocess.Popen([str(ROOT.parent/'bbk9288-qemu/_build/qemu-system-s1c33.exe'),
        '-M',f'bbk9288,usb-connected=off,nand-image={nand.as_posix()},nand-id=0xADDA8015',
        '-cpu','c33l05,exit-on-halt=off','-bios',str(ROOT.parent/'9288-boot-capture/captures/2026-10-01-v3/BOOT0.BIN'),
        '-display','none','-serial','none','-monitor','none','-qmp','stdio','-no-shutdown',
        '-gdb',f'tcp:127.0.0.1:{args.port}'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
        stderr=log,text=True,encoding='utf8',env=env)
    q=queue.Queue();seq=0;observations={};remote=None
    if (OUT/'model-build.json').exists():
        observations['build']=json.loads((OUT/'model-build.json').read_text())
    def reader():
        for line in p.stdout:
            try:q.put(json.loads(line))
            except ValueError:pass
    threading.Thread(target=reader,daemon=True).start()
    def call(command,arguments=None):
        nonlocal seq
        seq+=1;item={'execute':command,'id':seq}
        if arguments is not None:item['arguments']=arguments
        p.stdin.write(json.dumps(item)+'\n');p.stdin.flush()
        while True:
            response=q.get(timeout=15)
            if response.get('id')==seq:return response
    def native_snapshot():
        tcb=remote.words(0x022a9820,1)[0]
        top=remote.words(tcb+8*72+0x14,1)[0]
        saved_sp=remote.words(tcb+8*72+0x10,1)[0]
        return {'tcb_base':tcb,'task3':remote.read(tcb+3*72,72).hex(),
                'ros_globals_02068660':remote.words(0x02068660,8),
                'all_tcbs':remote.read(tcb,9*72).hex(),
                'ready_queues_02068684':remote.words(0x02068684,18),
                'timer_vector':remote.words(0x020000a8,1)[0],
                'task8':remote.read(tcb+8*72,72).hex(),
                'task8_stack_top':top,'task8_saved_sp':saved_sp,
                'task8_stack':remote.read(top-256,256).hex() if 0x02500100<=top<=0x02700000 else None,
                'task8_saved_context':remote.read(saved_sp,128).hex() if 0x02000000<=saved_sp<0x02800000 else None,
                'misc':remote.read(0x00300f20,1)[0],
                'timer_enable':remote.read(0x00040273,1)[0],
                'usb_enable':remote.read(0x000402a6,1)[0],
                'power_idle_counter':remote.words(0x024eef54,1)[0],
                'power_idle_minutes':remote.words(0x020496b8,1)[0],
                'power_idle_msc_flag':remote.read(0x024ef018,1)[0],
                'gui_timers_paused':remote.words(0x02401b4c,1)[0],
                'hook':remote.words(symbols['guest_hook_status'],2),
                'callback':remote.words(0x0203857c,1)[0]}
    def stop_using_launcher(cycle,control,base,exports):
        remote.write(0x02700000,(OUT/'native-service-model.bin').read_bytes())
        bridge=base+exports['gui_hook']
        assert remote.words(0x0203857c,1)[0]==bridge,'resident GUI hook lost before STOP'
        handoff=base+exports['guest_file_handoff']
        before_handoff=remote.words(handoff,8)
        remote.write(handoff+16,struct.pack('<I',symbols['guest_hook_status']))
        remote.write(handoff,struct.pack('<I',1))
        remote.resume()
        time.sleep(5);remote.stop()
        handed=remote.words(handoff,8)
        observations[f'gui_stop_handoff_{cycle}']=handed
        assert handed[1]==before_handoff[1]+1,'owned hook did not hand off after its GUI step'
        assert handed[5]==0x02068b7c and handed[6]&0x710==0x10 and handed[7]==bridge,'handoff bypassed ordinary owned GUI1 context'
        state=remote.words(control,17)
        observations[f'control_at_logged_stop_{cycle}']=state
        result=remote.words(symbols['usb_log_last_result'],1)[0]
        dialog_state=remote.words(symbols['usb_last_dialog_state'],1)[0]
        observations[f'log_dialog_state_{cycle}']=dialog_state
        assert dialog_state==5,'stop dialog displayed a stale RUNNING/READY state'
        path=remote.read(symbols['usb_log_last_path'],24).split(b'\0',1)[0].decode('ascii')
        observations[f'log_result_{cycle}']=result;observations[f'log_path_{cycle}']=path
        assert result==0 and path.startswith('A:\\D88U') and path.endswith('.BIN'),'native USB log write/readback failed'
        raw=remote.read(symbols['saved_report'],1024)
        report=decode_usb_log(raw)
        (OUT/f'native-usb-log-{cycle}.bin').write_bytes(raw)
        observations[f'usb_log_{cycle}']=report
        assert report['header']['flags']==7,'log lacked owned worker/USB snapshot guards'
        assert report['control']['state']==2 and report['header']['capture_kind']==1,'log did not capture the running service'
        tcb=observations['before']['tcb_base']
        assert int(report['register_map'][f'0x{tcb+8*72+63:08x}'],16)&8,'worker was not suspended for the log'
        address=base+exports['device_descriptor']
        actual=bytes(int(report['register_map'][f'0x{address+i:08x}'],16) for i in range(18))
        assert actual==bytes([18,1,0,2,2,2,1,64,9,18,136,146,0,1,1,2,3,1]),'resident descriptor relocation/data mismatch'
        if remote.words(symbols['guest_hook_status'],1)[0]!=2:
            remote.resume();call('human-monitor-command',{'command-line':'sendkey ret 300'})
            time.sleep(0.5);remote.stop()
        assert remote.words(symbols['guest_hook_status'],1)[0]==2,'logged stop launcher did not return'
        handed=remote.words(handoff,8)
        assert handed[2]==before_handoff[2]+1,'GUI launcher return chain did not finish'
        observations[f'gui_stop_handoff_{cycle}']=handed
        assert remote.words(0x0203857c,1)[0]==0,'STOP did not release its own GUI hook'
    def advance(seconds=.035):
        remote.resume();time.sleep(seconds);remote.stop()
    def file_rpc(opcode,payload):
        mailbox=base+exports['guest_file_mailbox']
        seq=remote.words(mailbox,1)[0]+1
        assert len(payload)<=512
        remote.write(base+exports['guest_file_input'],payload)
        remote.write(mailbox+8,struct.pack('<II',opcode,len(payload)))
        remote.write(mailbox,struct.pack('<I',seq))
        until=time.monotonic()+10
        while time.monotonic()<until:
            advance()
            state=remote.words(mailbox,8)
            if state[1]==seq:break
            assert remote.words(control,17)[9]==2,'resident stopped during file RPC'
        else:raise AssertionError(f'native file mailbox timed out opcode={opcode}, state={state}')
        assert state[4]==0 and state[5]<=512,(opcode,state)
        assert state[6]==0x02068d74 and state[7]&0x710==0x10,'file RPC did not run in ordinary worker8'
        return remote.read(base+exports['guest_file_output'],state[5])
    def file_submit(opcode,payload):
        data=file_rpc(opcode,payload);assert len(data)==8
        ident,result=struct.unpack('<Ii',data);assert ident and result==0,(opcode,ident,result)
        return ident
    def file_job(opcode,payload,expected=0):
        ident=file_submit(opcode,payload);until=time.monotonic()+45
        while time.monotonic()<until:
            raw=file_rpc(19,struct.pack('<I',ident));assert len(raw)>=16
            got,state,result,size=struct.unpack('<IIiI',raw[:16])
            assert got==ident and len(raw)==16+size
            if state==3:
                assert result==expected,(opcode,ident,result,expected)
                observations.setdefault('file_jobs',[]).append({'op':opcode,'job':ident,'result':result,'bytes':size})
                return raw[16:]
        raise AssertionError(f'native GUI file job timed out opcode={opcode}, job={ident}, state={state}')
    def file_put_open(path,data,replace=False):
        reply=file_job(15,struct.pack('<IIII',len(data),zlib.crc32(data),int(replace),len(path))+path)
        token,offset=struct.unpack('<II',reply);assert token and offset==0;return token
    def file_put_data(token,data,offset=0):
        reply=file_job(16,struct.pack('<III',token,offset,zlib.crc32(data))+data)
        got,next_offset,_=struct.unpack('<III',reply);assert got==token and next_offset==offset+len(data)
    def file_put(path,data,replace=False):
        token=file_put_open(path,data,replace)
        for off in range(0,len(data),480):file_put_data(token,data[off:off+480],off)
        reply=file_job(17,struct.pack('<I',token))
        assert struct.unpack('<III',reply)==(token,len(data),zlib.crc32(data))
    def file_get(path,expected):
        reply=file_job(13,struct.pack('<I',len(path))+path)
        token,size=struct.unpack('<II',reply);assert size==len(expected)
        actual=bytearray()
        while len(actual)<size:
            at=len(actual);reply=file_job(14,struct.pack('<III',token,at,480))
            got,off,crc=struct.unpack('<III',reply[:12]);chunk=reply[12:]
            assert got==token and off==at and crc==zlib.crc32(chunk) and chunk
            actual.extend(chunk)
        assert bytes(actual)==expected
        assert struct.unpack('<III',file_job(17,struct.pack('<I',token)))==(token,size,zlib.crc32(expected))
    def files_functional():
        path=b'A:\\D88NAT.BIN';original=FILE_ORIGINAL;replacement=FILE_REPLACEMENT
        assert len(original)>20*1024 and len(replacement)>20*1024
        file_put(path,original)
        directory='A:\\系统\\程序'.encode('gbk')
        file_job(15,struct.pack('<IIII',1,zlib.crc32(b'x'),1,len(directory))+directory,-703)
        file_get(path,original) # The rejected directory replacement did not affect ordinary reads.
        file_put(path,replacement,True);file_get(path,replacement)
        file_job(15,struct.pack('<IIII',1,zlib.crc32(b'x'),0,len(path))+path,-705)
        cancel_path=b'A:\\D88NCAN.BIN';token=file_put_open(cancel_path,b'cancel'*100)
        file_put_data(token,b'cancel'*40)
        file_job(18,struct.pack('<I',token))
        file_job(13,struct.pack('<I',len(cancel_path))+cancel_path,-704)
        stop_path=b'A:\\D88NSTP.BIN';token=file_put_open(stop_path,b'stop'*250)
        file_put_data(token,b'stop'*60)
        layout=remote.words(base+exports['guest_file_layout'],4)
        assert layout[0]%4==0 and layout[2]%4==0 and layout[0]<layout[3]<=4096
        state=remote.words(base+exports['tx'],layout[0]//4+1)
        assert state[layout[2]//4]==1 and state[layout[0]//4]!=0,'STOP fixture has no owned live upload'
        temp=remote.read(base+exports['tx']+layout[1],241).split(b'\0',1)[0]
        assert temp.startswith(b'A:\\~D') and temp.endswith(b'.TMP'),temp
        observations['files_functional']={'put_get_crc':True,'replace_crc':True,'abort_cleanup':True,
            'original_bytes':len(original),'original_crc32':f'{zlib.crc32(original):08x}',
            'replacement_bytes':len(replacement),'replacement_crc32':f'{zlib.crc32(replacement):08x}',
            'directory_replace_rejected':True,'directory_guard_path_gbk_hex':directory.hex(),
            'no_overwrite':True,'stop_token':token,'stop_target':stop_path.decode(),'stop_temp':temp.decode(),
            'tx_before_stop':state,'tx_layout':layout,'physical_usb_tested':False}
        print('files: native PUT/GET/replace/abort PASS; active upload ready for STOP',flush=True)
        return stop_path,temp
    try:
        q.get(timeout=10);call('qmp_capabilities');time.sleep(8)
        remote=Remote(args.port,lambda:call('stop'));remote.stop()
        observations['before']=native_snapshot()
        print('boot', {key:observations['before'][key] for key in
              ['tcb_base','misc','timer_enable','usb_enable','callback']},flush=True)
        remote.write(0x02700000,(OUT/'native-service-model.bin').read_bytes())
        remote.write(0x0203857c,struct.pack('<I',symbols['guest_launcher_hook']))
        first_break=0x021cfa52 if (args.diagnostic_restore_tcb or args.watch_timer_enable) else (args.arm_after or args.break_at)
        if first_break:assert remote.command(f'Z0,{first_break:x},2')=='OK'
        remote.resume()
        call('human-monitor-command',{'command-line':'sendkey ret 300'})
        time.sleep(5)
        if args.watch_timer_enable:
            remote.stop();observations['watch_armed_sta']=native_snapshot()
            entry=remote.words(observations['watch_armed_sta']['tcb_base']+8*72+24,1)[0]
            check=entry-manifest['exports']['resident_worker']+manifest['exports']['usb_platform_check']
            assert remote.command(f'Z0,{check:x},2')=='OK'
            remote.resume();time.sleep(0.3);remote.stop()
            observations['watch_before_first_check']=native_snapshot()
            observations['watch_first_check_registers']=call('human-monitor-command',{'command-line':'info registers'})
            assert remote.command('Z2,40273,1')=='OK'
            remote.resume();time.sleep(0.3);remote.stop()
            observations['watch_hit']=native_snapshot()
            observations['watch_registers']=call('human-monitor-command',{'command-line':'info registers'})
            print('watch_registers',observations['watch_registers'],flush=True)
            observations['watch_disassembly']=call('human-monitor-command',{'command-line':'x /120i $pc-80'})
            return
        if args.diagnostic_restore_tcb:
            remote.stop();observations['diagnostic_before_sta_write']=native_snapshot()
            previous=remote.words(0x02068664,1)[0]
            assert remote.command('Z0,21cfa6c,2')=='OK'
            remote.resume();time.sleep(0.3);remote.stop()
            observations['diagnostic_after_sta_write']=native_snapshot()
            remote.write(0x02068664,struct.pack('<I',previous))
            observations['diagnostic_gdb_write']={'address':0x02068664,'value':previous}
            remote.resume();time.sleep(2)
        if args.arm_after:
            remote.stop()
            observations['armed']=native_snapshot()
            observations['registers_at_arming']=call('human-monitor-command',{'command-line':'info registers'})
            # QEMU's initial-halt '?' query clears the breakpoint list.
            assert remote.command(f'Z0,{args.break_at:x},2')=='OK'
            remote.resume();time.sleep(0.3)
        remote.stop();observations['launched']=native_snapshot()
        observations['registers_after_launch']=call('human-monitor-command',{'command-line':'info registers'})
        print('registers',observations['registers_after_launch'],flush=True)
        print('task8', {key:observations['launched'][key] for key in
              ['task8','task8_stack_top','task8_saved_sp','misc','timer_enable','usb_enable','hook']},flush=True)
        if args.break_at:
            observations['native_ros_table']=remote.words(0x021e81e8,90)
            observations['native_stack']=remote.words(0x02018000,256)
            observations['disassembly']=[]
            for address,count in [(0x021ced06,180),(observations['native_ros_table'][0],180),(0x021ce8d8,60),(remote.words(0x02000088,1)[0],180)]:
                observations['disassembly'].append(call('human-monitor-command',{'command-line':f'x /{count}i 0x{address:x}'}))
            return
        tcb=observations['launched']['tcb_base']
        worker=remote.words(tcb+8*72+0x18,1)[0]
        exports=manifest['exports']
        base=worker-exports['resident_worker']
        control=base+exports['resident_control']
        observations['resident_base']=base;observations['control_address']=control
        observations['control_before_overlay']=remote.words(control,17)
        observations['model_counts']=remote.words(base+exports['guest_model_counts'],8)
        observations['model_fault']=remote.words(base+exports['guest_model_fault'],4)
        observations['usb_power']={'guest_vbus':remote.words(base+exports['guest_model_vbus'],1)[0],
            'native_qemu_usb_connected':False,
            'boundary':'VBUS high applies to the guest transport/ownership window; no physical host or native MSC session is emulated'}
        observations['signals_running']=remote.words(base+exports['guest_model_signals'],4)
        observations['cdc_diagnostic_running']=remote.words(base+exports['usb_diagnostic'],27)
        diagnostic=observations['cdc_diagnostic_running']
        assert diagnostic[0]==1 and diagnostic[2:4]==[0,0],'CDC diagnostic recorded a platform failure'
        assert diagnostic[10]==1 and diagnostic[11]==0x12,'CDC diagnostic register snapshot is invalid'
        assert diagnostic[13]&0x80 and diagnostic[14]==0x80,'powered CDC attachment is missing'
        assert observations['usb_power']['guest_vbus']==1,'USB power fixture is not connected'
        assert observations['model_counts'][7]==1,'CDC did not attach with VBUS already high'
        assert (observations['signals_running'][1]-observations['signals_running'][0])&0xffffffff>=64,'attach bypassed native detach delay'
        print('control',observations['control_before_overlay'],flush=True)
        print('ownership_first_fault',observations['model_fault'],flush=True)
        assert observations['model_fault']==[0,0,0,0],'native ownership invariant violation was recorded'
        assert observations['control_before_overlay'][0]==0x33443838,'launcher did not create resident'
        assert observations['control_before_overlay'][9]==2,'resident not RUNNING'
        guard=base+exports['usb_platform_timer_guard']
        assert observations['launched']['timer_vector']==guard,'Timer3B vector is not the heap ISR'
        observations['guard_disassembly']=call('human-monitor-command',{'command-line':f'x /9i 0x{guard:x}'})
        assert remote.command(f'Z0,{guard:x},2')=='OK'
        # Native cold-idle may leave timer3 stopped. Stimulate its real QEMU
        # peripheral once to execute the production ISR, then restore PRUN.
        # This is explicitly recorded test stimulus, not production behavior.
        timer_ctrl=remote.read(0x0004819e,1)[0]
        observations['irq_stimulation']={'address':0x0004819e,'before':timer_ctrl,
                                         'test_value':timer_ctrl|1}
        remote.write(0x0004819e,bytes([timer_ctrl|1]))
        remote.resume();time.sleep(1);remote.stop()
        observations['guard_execution']=call('human-monitor-command',{'command-line':'info registers'})
        assert f'pc=0x{guard:08x}' in observations['guard_execution'].get('return',''),'guard ISR was not executed'
        remote.write(0x0004819e,bytes([timer_ctrl]))
        remote.resume()
        if observations['launched']['hook'][0]!=2:
            call('human-monitor-command',{'command-line':'sendkey ret 300'})
        time.sleep(1)
        remote.stop()
        observations['before_overlay']=native_snapshot()
        assert observations['before_overlay']['hook'][0]==2,'launcher still on overlay stack'
        remote.write(0x02700000,b'\xa5'*0x100000)
        assert remote.read(0x02700000,32)==b'\xa5'*32
        observations['control_at_overlay']=remote.words(control,17)
        remote.resume();time.sleep(2);remote.stop()
        observations['control_after_overlay']=remote.words(control,17)
        assert observations['control_after_overlay'][13]>observations['control_at_overlay'][13], 'polls stopped after download overlay overwrite'
        observations['power_idle_running']=remote.words(base+exports['power_idle_diagnostic'],4)
        assert observations['power_idle_running'][0:2]==[1,1] and observations['power_idle_running'][2]>1 and observations['power_idle_running'][3]==0,'native idle timer keep-awake is not active'
        # A small, below-threshold counter seed proves the actual worker calls
        # the captured reset helper. No power-off setting or MSC flag is set.
        observations['power_idle_counter_seed']=5
        remote.write(0x024eef54,struct.pack('<I',5))
        remote.resume();time.sleep(.5);remote.stop()
        observations['power_idle_after_seed']=native_snapshot()
        assert observations['power_idle_after_seed']['power_idle_counter']<=1,'worker did not refresh the native idle counter'
        for field in ['power_idle_minutes','power_idle_msc_flag','gui_timers_paused']:
            assert observations['power_idle_after_seed'][field]==observations['before'][field],f'keep-awake changed {field}'
        stop_paths=files_functional() if args.files else None
        stop_using_launcher(1,control,base,exports)
        if args.files:
            layout=observations['files_functional']['tx_layout']
            state=remote.words(base+exports['tx'],layout[0]//4+1)
            at=layout[2]//4
            assert state[at:at+3]==[0,0,0] and state[layout[0]//4]==0,'STOP retained an owned file handle or name'
            observations['files_functional']['tx_after_stop']=state
        observations['stopped']=native_snapshot()
        observations['control_stopped']=remote.words(control,17)
        observations['model_counts_stopped']=remote.words(base+exports['guest_model_counts'],8)
        observations['signals_stopped']=remote.words(base+exports['guest_model_signals'],4)
        assert (observations['signals_stopped'][3]-observations['signals_stopped'][2])&0xffffffff>=64,'resume bypassed native detach delay'
        assert remote.words(base+exports['guest_model_vbus'],1)[0]==1,'stop depended on unplugging USB'
        stopped=observations['stopped']
        assert observations['control_stopped'][9]==5,'resident not STOPPED'
        observations['power_idle_stopped']=remote.words(base+exports['power_idle_diagnostic'],4)
        assert observations['power_idle_stopped'][1]==0,'stop left idle keep-awake active'
        remote.write(0x024eef54,struct.pack('<I',5))
        remote.resume();time.sleep(.5);remote.stop()
        observations['power_idle_after_stop_seed']=remote.words(0x024eef54,1)[0]
        assert observations['power_idle_after_stop_seed']>=5,'stopped debugger still reset native idle time'
        assert remote.words(base+exports['power_idle_diagnostic'],4)==observations['power_idle_stopped'],'stopped debugger refreshed idle diagnostics'
        # Captured ext_tsk sets bit4 and leaves low ready/running bits behind.
        # It removes the TCB from its ready queue; the canonical dormant test
        # is bit4, not equality with 0x10 (observed raw value is 0x13).
        assert bytes.fromhex(stopped['task8'])[0x3f]&16,'task8 not dormant'
        assert bytes.fromhex(stopped['task3'])[0x3f]==7,'native task3 not restored'
        assert stopped['misc']==0x27 and stopped['timer_enable']==0x44 and stopped['usb_enable']==0x40,'native register state not restored'
        assert stopped['timer_vector']==0x0219c9ac,'native Timer3B vector not restored'
        assert observations['model_counts_stopped'][4:6]==[1,1],'actual native suspend/resume callbacks not exercised'
        # Re-open the exact same launcher after destroying its previous copy.
        # Its existing-resident path must reuse this allocation and dormant TCB.
        remote.write(0x02700000,(OUT/'native-service-model.bin').read_bytes())
        remote.write(0x0203857c,struct.pack('<I',symbols['guest_launcher_hook']))
        remote.resume();call('human-monitor-command',{'command-line':'sendkey ret 300'})
        time.sleep(5);remote.stop()
        observations['second_running']=native_snapshot()
        observations['control_second_running']=remote.words(control,17)
        observations['signals_second_running']=remote.words(base+exports['guest_model_signals'],4)
        assert (observations['signals_second_running'][1]-observations['signals_second_running'][0])&0xffffffff>=64,'second attach bypassed native delay'
        second=observations['control_second_running']
        assert second[9]==2 and second[12]==2,'same-build second start did not run'
        observations['power_idle_second_running']=remote.words(base+exports['power_idle_diagnostic'],4)
        assert observations['power_idle_second_running'][1]==1,'second start did not resume idle keep-awake'
        assert second[4:7]==observations['control_before_overlay'][4:7],'second start changed resident allocation'
        assert remote.words(tcb+8*72+24,1)[0]==worker,'second start changed worker address'
        assert observations['second_running']['timer_vector']==guard,'second start lost IRQ guard'
        remote.resume()
        if observations['second_running']['hook'][0]!=2:
            call('human-monitor-command',{'command-line':'sendkey ret 300'})
        time.sleep(1);remote.stop()
        assert remote.words(symbols['guest_hook_status'],1)[0]==2,'second launcher still on overlay stack'
        if args.files:
            for path in stop_paths:file_job(13,struct.pack('<I',len(path))+path,-704)
            file_get(b'A:\\D88NAT.BIN',FILE_REPLACEMENT)
            observations['files_functional']['stop_cleanup_native_absence']=True
            observations['files_functional']['restart_file_readback']=True
        remote.write(0x02700000,b'\x5a'*0x100000)
        second_polls=remote.words(control,17)[13]
        remote.resume();time.sleep(1);remote.stop()
        observations['control_second_overlay']=remote.words(control,17)
        assert observations['control_second_overlay'][13]>second_polls,'second worker stopped after overlay overwrite'
        stop_using_launcher(2,control,base,exports)
        observations['second_stopped']=native_snapshot()
        observations['control_second_stopped']=remote.words(control,17)
        observations['model_counts_second_stopped']=remote.words(base+exports['guest_model_counts'],8)
        observations['signals_second_stopped']=remote.words(base+exports['guest_model_signals'],4)
        observations['misc_write_protection']=remote.words(base+exports['guest_misc_counts'],4)
        observations['misc_failure']=remote.words(base+exports['usb_platform_misc_failure'],1)[0]
        observations['misc_clock_powerup']=remote.words(base+exports['guest_misc_clock_signals'],6)
        powerup=observations['misc_clock_powerup']
        assert powerup[2]==2 and powerup[5]==2 and powerup[3]>=64 and powerup[4]==0,'USB clock stabilization was bypassed'
        assert observations['log_path_1']!=observations['log_path_2'],'second USB log overwrote the first report'
        observations['model_fault_final']=remote.words(base+exports['guest_model_fault'],4)
        assert observations['model_fault_final']==[0,0,0,0],'ownership invariant failed during a later cycle'
        misc=observations['misc_write_protection']
        assert misc[0]==misc[1] and misc[1]>0,'Misc writes lack one unlock key each'
        assert misc[2:]==[0,0],'a protected Misc write was rejected or lacked the CPU lock'
        assert observations['misc_failure']==0,'Misc readback failure was recorded'
        assert (observations['signals_second_stopped'][3]-observations['signals_second_stopped'][2])&0xffffffff>=64,'second resume bypassed native delay'
        assert remote.words(base+exports['guest_model_vbus'],1)[0]==1,'second stop depended on unplugging USB'
        assert observations['model_counts_second_stopped'][7]==2,'unexpected number of CDC software attaches'
        final=observations['second_stopped']
        assert observations['control_second_stopped'][9]==5 and observations['control_second_stopped'][12]==2,'second stop did not finish'
        observations['power_idle_second_stopped']=remote.words(base+exports['power_idle_diagnostic'],4)
        assert observations['power_idle_second_stopped'][1]==0,'second stop left idle keep-awake active'
        for field in ['power_idle_minutes','power_idle_msc_flag','gui_timers_paused']:
            assert final[field]==observations['before'][field],f'start/stop changed {field}'
        assert bytes.fromhex(final['task8'])[0x3f]&16,'second ext_tsk did not mark task8 dormant'
        assert bytes.fromhex(final['task3'])[0x3f]==7,'second stop did not restore native USB task'
        assert (final['misc'],final['timer_enable'],final['usb_enable'],final['timer_vector'])==(0x27,0x44,0x40,0x0219c9ac),'second native restore failed'
        assert observations['model_counts_second_stopped'][4:6]==[2,2],'second suspend/resume not exercised'
        observations['result']=('DIAGNOSTIC WITH GDB OS POINTER REPAIR: ' if args.diagnostic_restore_tcb else 'PASS: ')+\
            'native ROS task/heap/two overlays/two powered starts and logged stops; Misc protection and clock powerup enforced; native FS report readback; simulated USB only'
        if args.files:
            observations['result']+='; actual captured FS PUT/GET/replace >20KiB CRC/abort/active-upload STOP cleanup and restart readback passed'
        print(observations['result'],flush=True)
    finally:
        record=OUT/('observations-watch-timer-enable.json' if args.watch_timer_enable else
                    'observations-diagnostic-restore-tcb.json' if args.diagnostic_restore_tcb else
                    f'observations-break-{args.break_at:08x}.json' if args.break_at else
                    'observations-files.json' if args.files else 'observations.json')
        record.write_text(json.dumps(observations,indent=2)+'\n')
        if remote is not None:remote.close()
        if p.poll() is None:p.terminate()
        p.wait(timeout=10);log.close()

if __name__=='__main__':main()
