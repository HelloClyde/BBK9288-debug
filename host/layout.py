"""Decode/collect D88MAP. System-page census is observation, not ownership."""
from pathlib import Path
import argparse,hashlib,json,struct,time,zlib
from contextlib import nullcontext
from .client import Client
from .memory import fields,HEAP_FIELDS,signed
ROOT=Path(__file__).resolve().parents[1]
MAGIC=0x504d3844
ARENA_FIELDS=('version','structure_bytes','result','flags','base','arena_bytes','end','rounds_completed',
              'fill_words','verified_words','yield_calls','active_round','expected_crc0','expected_crc1',
              'actual_crc0','actual_crc1','first_bad_address','first_bad_expected','first_bad_actual','error_phase')
def decode(raw,expected_image=None):
    if len(raw)<256 or len(raw)>131072 or len(raw)&3:raise ValueError('invalid report length')
    h=struct.unpack_from('<64I',raw)
    if h[:3]!=(MAGIC,1,len(raw)):raise ValueError('report identity/size mismatch')
    if expected_image is not None and h[3]!=expected_image:raise ValueError('report build mismatch')
    if h[4] not in (3,4) or h[18]!=9:raise ValueError('incomplete report/sections')
    if zlib.crc32(raw[:-4])&0xffffffff!=struct.unpack_from('<I',raw,len(raw)-4)[0]:raise ValueError('report CRC mismatch')
    parts={};ranges=[]
    for n in range(9):
        k,off,size,count=h[24+n*4:28+n*4]
        if k not in range(1,10) or k in parts or off<256 or off&3 or size&3 or off+size>len(raw)-4:raise ValueError('invalid section')
        if any(off<b and a<off+size for a,b in ranges):raise ValueError('overlap')
        ranges.append((off,off+size));parts[k]=(raw[off:off+size],count,off)
    for k,size,count in [(1,16384,1),(2,16384,1),(3,4096,128),(4,76,1),(5,76,1),(6,24,1),(7,80,1),(8,648,9)]:
        if len(parts[k][0])!=size or parts[k][1]!=count:raise ValueError('section size/count mismatch')
    before=fields(parts[4][0],HEAP_FIELDS);after=fields(parts[5][0],HEAP_FIELDS)
    work=fields(parts[6][0],('version','bytes','result','address','allocation_bytes','active'))
    arena=fields(parts[7][0],ARENA_FIELDS)
    pages=[]
    for i in range(128):
        p=fields(parts[3][0],('address','bytes','crc32','zero_bytes','ff_bytes','first_word','last_word','reserved'),i*32)
        if p['address']!=0x02000000+i*65536 or p['bytes']!=65536 or p['zero_bytes']+p['ff_bytes']>65536:raise ValueError('invalid page')
        pages.append(p)
    if len(parts[9][0])!=6144 or parts[9][1]>256 or parts[9][1]!=before['record_count']:raise ValueError('invalid heap records')
    records=[]
    for i in range(parts[9][1]):
        x=struct.unpack_from('<6I',parts[9][0],i*24)
        records.append({'address':x[0],'bytes':x[1]&~1,'allocated':bool(x[1]&1),'sample_words':list(x[2:])})
    result={'image_id':f'{h[3]:08x}','state':'DONE' if h[4]==3 else 'ERROR','result':signed(h[6]),
        'flags':h[8],'completed_pages':h[7],'psr':h[9],'sp':h[10],'gp':h[11],'current_TCB':h[12],
        'native_ticks':{'start':h[13],'iram0':h[14],'iram1':h[15],'end':h[16]},
        'last_phase':h[17],'confirmed_simultaneously_held_bytes':h[19],'heap_request_bytes':h[20],
        'heap_work_bytes':h[21],'static_arena_bytes':h[22],
        'heap_before':before,'heap_after':after,'heap_work':work,'arena':arena,
        'heap_records':records,'pages':pages,'tcbs':[list(struct.unpack_from('<18I',parts[8][0],i*72)) for i in range(9)],
        'iram_sha256':[hashlib.sha256(parts[k][0]).hexdigest() for k in (1,2)],
        'iram_changed_bytes':sum(a!=b for a,b in zip(parts[1][0],parts[2][0])),
        'report_crc32':f'{zlib.crc32(raw[:-4])&0xffffffff:08x}','report_sha256':hashlib.sha256(raw).hexdigest(),
        'census_boundary':'sequential read-only observation; zero/FF does not grant application ownership'}
    if h[4]==3:
        if h[6] or h[8]!=63 or h[7]!=128 or before!=after or work['active'] or arena['result'] or arena['flags']!=7 or arena['rounds_completed']!=2:raise ValueError('DONE contradicts verification')
        if h[19]!=work['allocation_bytes']+arena['arena_bytes'] or h[19]<=before['max_request']:raise ValueError('additional-capacity demonstration failed')
        if arena['expected_crc0']!=arena['actual_crc0'] or arena['expected_crc1']!=arena['actual_crc1']:raise ValueError('arena CRC mismatch')
    return result

def collect(output,build,*,client=None,port=None,timeout=300):
    meta=json.loads(build.read_text(encoding='utf-8'));a,n=meta['report_address'],meta['report_bytes']
    if not 0x02700000<=a<a+n<=0x02740000 or n>131072:raise ValueError('report RAM bounds')
    output.mkdir(parents=True,exist_ok=True);record={'build':meta,'progress':[]};start=time.monotonic();last=None
    try:
        with (nullcontext(client) if client is not None else Client(port)) as c:
            record['identity']=c.identity;record['status_before']=c.status()
            while time.monotonic()-start<timeout:
                h=struct.unpack('<24I',c.read_memory(a,96))
                if h[:4]==(MAGIC,1,n,int(meta['image_id'],16)):
                    progress=(h[4],h[5],h[7])
                    if progress!=last:
                        last=progress;record['progress'].append({'seconds':time.monotonic()-start,'state':h[4],'phase':h[5],'pages':h[7]})
                        print(json.dumps(record['progress'][-1]),flush=True)
                    if h[4] in (3,4):
                        raw=c.read_memory(a,n);(output/'D88MAP.BIN').write_bytes(raw);record['report']=decode(raw,int(meta['image_id'],16))
                        words=struct.unpack_from('<64I',raw)
                        for i in (0,1):
                            k,off,size,count=words[24+i*4:28+i*4];(output/f'iram-{i}.bin').write_bytes(raw[off:off+size])
                        record['status_after']=c.status();return record
                time.sleep(.5)
            raise TimeoutError('D88MAP did not complete')
    except BaseException as e:
        record['failure']={'type':type(e).__name__,'message':str(e)};raise
    finally:(output/'acceptance.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--build',type=Path,default=ROOT/'dist/layout-build.json');p.add_argument('--port');a=p.parse_args()
    if a.input:
        r=decode(a.input.read_bytes());a.output.write_text(json.dumps(r,indent=2),encoding='utf-8')
    else:r=collect(a.output,a.build,port=a.port)
    print(json.dumps(r,ensure_ascii=True))
if __name__=='__main__':main()
