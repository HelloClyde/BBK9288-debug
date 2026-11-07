"""Read D88MEM's fixed RAM report through the existing, identity-checked debugger."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import hashlib
import json
from pathlib import Path
import statistics
import struct
import time
import zlib

from .client import Client

ROOT=Path(__file__).resolve().parents[1]
MAGIC=0x4d383844
REPORT_BYTES=8192
HEAP_FIELDS=('version','bytes','result','start','end','data_top','directory_bottom','record_count',
             'allocated_records','free_records','allocated_payload','free_holes','largest_hole',
             'tail_gap','tail_capacity','max_request','free_capacity','metadata_bytes','directory_crc32')
MAP_FIELDS=('version','structure_bytes','result','flags','register_count','partrev','addrc','configured_bytes',
            'bank_count','bank_bytes','row_bytes','area_enable_mask','window_base','window_end','organization','reserved')
PROBE_FIELDS=('version','bytes','result','candidate','pointer','boundary_request','boundary_pointer',
              'allocated_ok','boundary_failed','state_restored','directory_restored','errno_before','errno_after')
SPEED_FIELDS=('version','operation','source','destination','bytes_per_pass','windows','passes','bytes_processed',
              'rtc_ticks','rtc_nominal_hz','flags','checksum','error','ros_ticks','t2_first','t2_last',
              't2_compare_b','t2_ctrl','t2_clock','psc_select','psc_power','clock_option','rtc_first','rtc_last')
CASES=('IRAM read 16KiB','IVRAM read 16KiB (LCD active)','SDRAM bank0 read 16KiB',
       'SDRAM bank1 read 16KiB','SDRAM bank2 read 16KiB','SDRAM bank3 read 16KiB',
       'owned buffer A write 64KiB','owned buffer B write 64KiB',
       'owned A to B copy 64KiB','owned B to A copy 64KiB')

def signed(x):return x if x<0x80000000 else x-0x100000000
def words(data,offset,count):return struct.unpack_from('<'+'I'*count,data,offset)
def fields(data,names,offset=0):return dict(zip(names,words(data,offset,len(names))))

def decode(data:bytes,expected_image:int|None=None)->dict:
    if len(data)!=REPORT_BYTES:raise ValueError('D88MEM report must be 8192 bytes')
    h=words(data,0,16)
    if h[:3]!=(MAGIC,1,REPORT_BYTES):raise ValueError('D88MEM header identity/length mismatch')
    if expected_image is not None and h[3]!=expected_image:raise ValueError('D88MEM image_id mismatch')
    if h[4] not in (3,4):raise ValueError('D88MEM is not complete')
    if zlib.crc32(data[:-4])&0xffffffff!=words(data,REPORT_BYTES-4,1)[0]:raise ValueError('D88MEM CRC32 mismatch')
    if not 0<h[8]<=8 or not 256<=h[10]<=REPORT_BYTES-4:raise ValueError('Invalid section count/end')
    sections={};ranges=[]
    for i in range(h[8]):
        kind,offset,length,count=words(data,64+i*16,4)
        if kind in sections or kind not in range(1,7) or offset<256 or offset&3 or length&3 or offset+length>h[10]:
            raise ValueError('Invalid/duplicate D88MEM section')
        if any(offset<end and start<offset+length for start,end in ranges):raise ValueError('Overlapping D88MEM sections')
        ranges.append((offset,offset+length));sections[kind]=(data[offset:offset+length],count)
    if set(sections)!=set(range(1,7)):raise ValueError('D88MEM required sections missing')
    for kind,expected in ((1,288),(2,76),(3,204),(4,24),(6,76)):
        if len(sections[kind][0])!=expected or sections[kind][1]!=1:raise ValueError('D88MEM structure size mismatch')
    mapping=fields(sections[1][0],MAP_FIELDS)
    if mapping['register_count']>28:raise ValueError('Invalid register count')
    mapping['registers']=[{'address':a,'value':v} for a,v in
        (words(sections[1][0],64+i*8,2) for i in range(mapping['register_count']))]
    mapping['register_map']={f'0x{x["address"]:08x}':f'0x{x["value"]:08x}' for x in mapping['registers']}
    probe=fields(sections[3][0],PROBE_FIELDS)
    probe['before']=fields(sections[3][0],HEAP_FIELDS,52)
    probe['after']=fields(sections[3][0],HEAP_FIELDS,128)
    allocation=fields(sections[4][0],('version','bytes','result','address','allocation_bytes','active'))
    rows=[];raw,count=sections[5]
    if count>30 or len(raw)!=count*104 or count!=h[7]:raise ValueError('Invalid speed-result count/size')
    seen=set()
    for i in range(count):
        case,trial=words(raw,i*104,2)
        if case>=len(CASES) or trial>=3 or (case,trial) in seen:raise ValueError('Invalid/duplicate speed case')
        seen.add((case,trial));row=fields(raw,SPEED_FIELDS,i*104+8)
        row.update(case_id=case,trial=trial,label=CASES[case],error_signed=signed(row['error']))
        if row['version']!=1 or row['operation'] not in (1,2,3):raise ValueError('Invalid speed-result identity')
        if not row['error']:
            if row['windows']!=4 or row['rtc_ticks']<128 or row['rtc_nominal_hz']!=256 or row['bytes_per_pass'] not in (16384,65536):
                raise ValueError('Invalid speed timing/size')
            if row['bytes_processed']!=row['passes']*row['bytes_per_pass']:raise ValueError('Speed byte accounting mismatch')
            row['nominal_seconds']=row['rtc_ticks']/row['rtc_nominal_hz']
            row['nominal_payload_mib_s']=row['bytes_processed']/row['nominal_seconds']/1048576
        rows.append(row)
    result={'sha256':hashlib.sha256(data).hexdigest(),'crc32':'passed','image_id':f'{h[3]:08x}',
            'state':'DONE' if h[4]==3 else 'ERROR','result':signed(h[6]),'flags':h[9],
            'error_phase':h[9]>>16,'report_address':h[11],'used_bytes':h[10],
            'ros_start':h[12],'ros_end':h[13],'rtc_start':h[14],'rtc_end':h[15],
            'map':mapping,'heap_before':fields(sections[2][0],HEAP_FIELDS),'probe':probe,
            'allocation':allocation,'speed_results':rows,'heap_after':fields(sections[6][0],HEAP_FIELDS),
            'timing_boundary':'Rates use device RTC ticks at nominal OSC1/128=256Hz; explicit yields excluded, interrupts included. Copy counts payload once, not read+write bus traffic.'}
    summaries=[]
    for case,label in enumerate(CASES):
        samples=[x['nominal_payload_mib_s'] for x in rows if x['case_id']==case and not x['error']]
        if samples:summaries.append({'case_id':case,'label':label,'samples':len(samples),
            'median_nominal_mib_s':statistics.median(samples),'minimum_nominal_mib_s':min(samples),
            'maximum_nominal_mib_s':max(samples)})
    result['speed_summary']=summaries
    result['whole_run_nominal_rtc_seconds']=((h[15]-h[14])%(3600*256))/256 if h[15] or h[14] else None
    if h[4]==3 and (h[6] or count!=30 or allocation['active'] or not probe['state_restored'] or not probe['directory_restored'] or not probe['allocated_ok'] or not probe['boundary_failed']):
        raise ValueError('DONE report contradicts capacity/cleanup result')
    return result

def collect(port:str|None,output:Path,build:Path,timeout:float=300,poll_interval:float=1,*,client=None)->dict:
    meta=json.loads(build.read_text(encoding='utf-8'))
    address,size=meta['report_address'],meta['report_bytes']
    if size!=REPORT_BYTES or not 0x02700000<=address<=0x02800000-size:raise ValueError('Invalid report RAM bounds')
    output.mkdir(parents=True,exist_ok=True)
    record={'memory_build':meta,'agent_keys_sent':False,'agent_memory_writes_sent':False,'stop_sent':False,'progress':[]}
    started=time.monotonic();first_running=None;last_progress=None
    try:
        with (nullcontext(client) if client is not None else Client(port)) as c:
            record['identity']=c.identity;record['status_before']=c.status()
            while time.monotonic()-started<timeout:
                sent=time.monotonic();header=c.read_memory(address,64);received=time.monotonic()
                h=words(header,0,16)
                if h[:4]==(MAGIC,1,REPORT_BYTES,int(meta['image_id'],16)):
                    if first_running is None and h[4]==2:first_running=received
                    progress=(h[4],h[5],h[7])
                    if progress!=last_progress:
                        record['progress'].append({'elapsed_seconds':received-started,'state':h[4],'phase':h[5],'samples':h[7],
                            'request_sent_monotonic':sent,'reply_received_monotonic':received})
                        print(json.dumps({'state':h[4],'phase':h[5],'samples':h[7]}),flush=True);last_progress=progress
                    if h[4] in (3,4):
                        record['host_observed_running_to_done_seconds']=received-first_running if first_running is not None else None
                        raw=c.read_memory(address,size);(output/'D88MEM.BIN').write_bytes(raw)
                        record['report']=decode(raw,int(meta['image_id'],16))
                        record['status_after']=c.status();record['service_left_running']=True
                        break
                time.sleep(poll_interval)
            else:raise TimeoutError('D88MEM report did not complete; no replay, write or STOP sent')
    except Exception as error:
        record['failure']={'type':type(error).__name__,'message':str(error)}
        raise
    finally:
        (output/'acceptance.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return record

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port');p.add_argument('--build',type=Path,default=ROOT/'dist/memory-build.json')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--input',type=Path)
    p.add_argument('--timeout',type=float,default=300)
    a=p.parse_args()
    if a.input:
        result=decode(a.input.read_bytes());a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    else:result=collect(a.port,a.output,a.build,a.timeout)
    print(json.dumps(result,ensure_ascii=False,indent=2))

if __name__=='__main__':main()
