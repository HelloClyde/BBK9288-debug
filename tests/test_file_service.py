"""Fault tests of the production GUI file transaction FSM; no physical ports."""
import ctypes as C
import _ctypes
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
import zlib

from compiler_paths import native_gcc, target_compiler

ROOT=Path(__file__).resolve().parents[1]
U32=struct.Struct('<I')
ARG,BUSY,IO,NOTFOUND,EXISTS,CRC,STATE,PATH,PROFILE,CANCELLED,SIZE,LIMIT,COMMIT,RECOVERY,UNSUPPORTED=range(-701,-716,-1)
OPEN,READ,WRITE,CLOSE,EXIST,RENAME,REMOVE,SEEK,TELL=range(1,10)
DEFAULT='A:\\系统\\程序\\DEMO.EXE'.encode('gbk')

class Reply(C.Structure):
    _fields_=[('job_id',C.c_uint),('state',C.c_uint),('result',C.c_int),('length',C.c_uint),('data',C.c_ubyte*496)]

def words(*v):return struct.pack('<'+'I'*len(v),*v)
def crc(data):return zlib.crc32(data)&0xffffffff

class FileServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc=native_gcc()
        if not Path(gcc).is_file():raise unittest.SkipTest('GCC required')
        cls.tmp=tempfile.TemporaryDirectory();lib=Path(cls.tmp.name)/'files.dll'
        env=dict(os.environ);env['PATH']=str(Path(gcc).parent)+os.pathsep+env.get('PATH','')
        subprocess.run([gcc,'-std=c99','-O2','-Wall','-Wextra','-Werror','-shared',str(ROOT/'tests/file_service_model.c'),'-o',str(lib)],check=True,capture_output=True,text=True,env=env)
        cls.lib=C.CDLL(str(lib));l=cls.lib
        l.d88_file_submit.argtypes=(C.c_uint,C.c_void_p,C.c_uint,C.POINTER(C.c_uint))
        l.d88_file_poll.argtypes=(C.c_uint,C.POINTER(Reply))
        l.model_seed.argtypes=(C.c_char_p,C.c_void_p,C.c_uint)
        l.model_size.argtypes=(C.c_char_p,);l.model_contents.argtypes=(C.c_char_p,C.c_void_p)
        l.model_directory.argtypes=(C.c_char_p,)
        l.model_name.argtypes=(C.c_uint,);l.model_name.restype=C.c_char_p
        l.model_fail.argtypes=(C.c_uint,C.c_uint,C.c_uint)
        l.model_fail_also.argtypes=(C.c_uint,C.c_uint)
        l.model_profile.argtypes=(C.c_uint,);l.model_calls.argtypes=(C.c_uint,)
        l.model_report_size.argtypes=(C.c_char_p,C.c_uint)
        l.model_cancel_in_call.argtypes=(C.c_uint,C.c_uint)
    @classmethod
    def tearDownClass(cls):
        if hasattr(cls,'lib'):
            if os.name=='nt':_ctypes.FreeLibrary(cls.lib._handle)
            else:_ctypes.dlclose(cls.lib._handle)
            del cls.lib
        cls.tmp.cleanup()
    def setUp(self):self.lib.model_reset()
    def tearDown(self):self.assertEqual(self.lib.model_bad_calls(),0)
    def seed(self,p,data):self.lib.model_seed(p,data,len(data))
    def content(self,p):
        n=self.lib.model_size(p)
        if n<0:return None
        out=C.create_string_buffer(32768);self.assertEqual(self.lib.model_contents(p,out),n);return out.raw[:n]
    def names(self):return [self.lib.model_name(i) for i in range(self.lib.model_files())]
    def submit(self,op,p):
        i=C.c_uint();self.assertEqual(self.lib.d88_file_submit(op,p,len(p),C.byref(i)),0);self.assertGreater(i.value,0);return i.value
    def poll(self,i):
        r=Reply();self.assertEqual(self.lib.d88_file_poll(i,C.byref(r)),0);return r
    def drain(self,i,maximum=1000):
        for _ in range(maximum):
            r=self.poll(i)
            if r.state==3:return r
            self.lib.model_step()
        self.fail(f'job did not finish phase={self.lib.model_phase()} flags={self.lib.model_flags()}')
    def job(self,op,p):return self.drain(self.submit(op,p))
    def put_open(self,data,path=DEFAULT,replace=0,expected=None):
        r=self.job(15,words(len(data),crc(data) if expected is None else expected,replace,len(path))+path)
        self.assertEqual(r.result,0);self.assertEqual(r.length,8)
        token,offset=struct.unpack('<II',bytes(r.data[:8]));self.assertEqual(offset,0);return token
    def put_data(self,token,data):
        for at in range(0,len(data),480):
            chunk=data[at:at+480];r=self.job(16,words(token,at,crc(chunk))+chunk)
            self.assertEqual(r.result,0);self.assertEqual(struct.unpack('<III',bytes(r.data[:12])),(token,at+len(chunk),crc(data[:at+len(chunk)])))
    def put(self,data,path=DEFAULT,replace=0):
        t=self.put_open(data,path,replace);self.put_data(t,data);return self.job(17,words(t))
    def assert_closed(self):self.assertEqual(self.lib.model_handles(),0);self.assertFalse(self.lib.d88_file_pending())

    def test_asynchronous_metadata_never_touches_fs(self):
        i=self.submit(13,words(len(DEFAULT))+DEFAULT);self.assertEqual(self.lib.model_fs_calls(),0)
        self.assertEqual(self.poll(i).state,1);self.assertEqual(self.lib.model_fs_calls(),0)
        j=C.c_uint();self.assertEqual(self.lib.d88_file_submit(13,b'x',1,C.byref(j)),BUSY)
        self.lib.d88_file_cancel();r=self.drain(i);self.assertEqual(r.result,CANCELLED);self.assertEqual(self.lib.model_fs_calls(),0);self.assert_closed()
    def test_upload_multichunk_and_final_readback(self):
        data=bytes(range(251))*7;r=self.put(data)
        self.assertEqual(r.result,0);self.assertEqual(self.content(DEFAULT),data);self.assertEqual(self.names(),[DEFAULT])
        self.assertEqual(self.lib.model_calls(RENAME),1);self.assertEqual(self.lib.model_calls(READ),4);self.assert_closed()
    def test_finish_uses_bounded_16k_native_reads_and_480_byte_transport(self):
        data=bytes(range(250))*80;r=self.put(data)
        self.assertEqual(r.result,0);self.assertEqual(self.content(DEFAULT),data)
        self.assertEqual(self.lib.model_max_read(),16384);self.assertEqual(self.lib.model_calls(READ),6)
        self.assertEqual(self.lib.model_calls(WRITE),42);self.assert_closed()
    def test_empty_upload(self):
        r=self.put(b'');self.assertEqual(r.result,0);self.assertEqual(self.content(DEFAULT),b'');self.assert_closed()
    def test_no_replace_leaves_existing_original(self):
        self.seed(DEFAULT,b'old');r=self.job(15,words(3,crc(b'new'),0,len(DEFAULT))+DEFAULT)
        self.assertEqual(r.result,EXISTS);self.assertEqual(self.content(DEFAULT),b'old');self.assertEqual(self.names(),[DEFAULT]);self.assert_closed()
    def test_directory_target_is_never_moved_or_replaced(self):
        self.lib.model_directory(DEFAULT)
        r=self.job(15,words(3,crc(b'new'),1,len(DEFAULT))+DEFAULT)
        self.assertEqual(r.result,IO);self.assertEqual(self.names(),[DEFAULT]);self.assertEqual(self.lib.model_calls(RENAME),0)
        self.assertEqual(self.lib.model_calls(REMOVE),0);self.assertEqual(self.lib.model_calls(WRITE),0);self.assert_closed()
    def test_replace_preserves_old_until_verified_commit(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new version',replace=1);self.put_data(t,b'new version')
        self.assertEqual(self.content(DEFAULT),b'old');r=self.job(17,words(t))
        self.assertEqual(r.result,0);self.assertEqual(self.content(DEFAULT),b'new version');self.assertEqual(self.names(),[DEFAULT])
        self.assertEqual(self.lib.model_calls(RENAME),2);self.assert_closed()
    def test_replace_rename_failure_before_change_restores_backup(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1);self.put_data(t,b'new');self.lib.model_fail(RENAME,2,1)
        r=self.job(17,words(t));self.assertEqual(r.result,COMMIT);self.assertEqual(self.names(),[DEFAULT]);self.assertEqual(self.content(DEFAULT),b'old');self.assertEqual(self.lib.model_calls(RENAME),3);self.assert_closed()
    def test_replace_flush_error_after_promote_rolls_back(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1);self.put_data(t,b'new');self.lib.model_fail(RENAME,2,2)
        r=self.job(17,words(t));self.assertEqual(r.result,COMMIT);self.assertEqual(self.content(DEFAULT),b'old');self.assertEqual(self.names(),[DEFAULT]);self.assertEqual(self.lib.model_calls(RENAME),4);self.assert_closed()
    def test_ambiguous_move_never_deletes_possible_original(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1);self.put_data(t,b'new');self.lib.model_fail(RENAME,1,3)
        r=self.job(17,words(t));self.assertEqual(r.result,RECOVERY);self.assertEqual(self.content(DEFAULT),b'old')
        self.assertTrue(any(p.endswith(b'.BAK') and self.content(p)==b'old' for p in self.names()));self.assertEqual(self.lib.model_calls(REMOVE),0);self.assert_closed()
        self.assertEqual(self.lib.d88_file_fatal_error(),0);self.assertEqual(self.lib.d88_file_cleanup_error(),RECOVERY)
        original=bytes(r.data[:r.length]);self.lib.d88_file_init();again=self.poll(r.job_id)
        self.assertEqual(again.result,RECOVERY);self.assertEqual(bytes(again.data[:again.length]),original)
        i=C.c_uint();self.assertEqual(self.lib.d88_file_submit(18,words(0),4,C.byref(i)),RECOVERY);self.assertEqual(i.value,0)
    def test_failed_rollback_keeps_only_original_backup(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1);self.put_data(t,b'new')
        self.lib.model_fail(RENAME,2,2);self.lib.model_fail_also(3,1)
        r=self.job(17,words(t));self.assertEqual(r.result,RECOVERY)
        self.assertTrue(any(p.endswith(b'.BAK') and self.content(p)==b'old' for p in self.names()))
        self.assertEqual(self.content(DEFAULT),b'new');self.assertEqual(self.lib.model_calls(REMOVE),0);self.assert_closed()
    def test_final_readback_crc_failure_restores_original(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1);self.put_data(t,b'new')
        self.lib.model_fail(READ,3,2)  # temporary data, temporary EOF, final data.
        r=self.job(17,words(t));self.assertEqual(r.result,CRC);self.assertEqual(self.content(DEFAULT),b'old')
        self.assertEqual(self.names(),[DEFAULT]);self.assert_closed()
    def test_cancel_after_old_move_restores_original(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1);self.put_data(t,b'new');i=self.submit(17,words(t))
        for _ in range(100):
            self.lib.model_step()
            if self.lib.model_flags()&1:break
        self.assertTrue(self.lib.model_flags()&1);self.lib.d88_file_cancel();r=self.drain(i)
        self.assertEqual(r.result,CANCELLED);self.assertEqual(self.content(DEFAULT),b'old');self.assertEqual(self.names(),[DEFAULT]);self.assert_closed()
    def test_cancel_after_promote_restores_original(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1);self.put_data(t,b'new');i=self.submit(17,words(t))
        for _ in range(100):
            self.lib.model_step()
            if self.lib.model_flags()&2:break
        self.assertEqual(self.lib.model_flags()&3,3);self.lib.d88_file_cancel();r=self.drain(i)
        self.assertEqual(r.result,CANCELLED);self.assertEqual(self.content(DEFAULT),b'old');self.assertEqual(self.names(),[DEFAULT]);self.assert_closed()
    def test_commit_wins_cancel_after_last_remove_started(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1);self.put_data(t,b'new');self.lib.model_cancel_in_remove()
        r=self.job(17,words(t));self.assertEqual(r.result,0);self.assertEqual(self.content(DEFAULT),b'new');self.assert_closed()
    def test_invalid_token_offset_crc_and_size_do_not_write(self):
        t=self.put_open(b'abc');writes=self.lib.model_calls(WRITE)
        for body,expected in [(words(t+1,0,crc(b'abc'))+b'abc',STATE),(words(t,1,crc(b'abc'))+b'abc',ARG),(words(t,0,0)+b'abc',CRC),(words(t,0,crc(b'abcd'))+b'abcd',ARG)]:
            self.assertEqual(self.job(16,body).result,expected)
        self.assertEqual(self.lib.model_calls(WRITE),writes);self.put_data(t,b'abc');self.assertEqual(self.job(17,words(t)).result,0);self.assert_closed()
    def test_full_crc_mismatch_removes_only_owned_temp(self):
        t=self.put_open(b'abc',expected=123);self.put_data(t,b'abc');r=self.job(17,words(t));self.assertEqual(r.result,CRC);self.assertEqual(self.names(),[]);self.assert_closed()
    def test_close_flush_error_does_not_close_freed_handle_twice(self):
        t=self.put_open(b'abc');self.put_data(t,b'abc');self.lib.model_fail(CLOSE,1,1)
        r=self.job(17,words(t));self.assertEqual(r.result,IO);self.assertEqual(self.lib.model_calls(CLOSE),1);self.assertEqual(self.names(),[]);self.assert_closed()
    def test_cancel_propagates_close_failure(self):
        self.put_open(b'abc');self.lib.model_fail(CLOSE,1,1);self.lib.d88_file_cancel()
        r=self.drain(1);self.assertEqual(r.result,IO);self.assertEqual(self.names(),[]);self.assert_closed()
        self.assertEqual(self.lib.d88_file_cleanup_error(),IO);self.assertEqual(self.lib.d88_file_fatal_error(),0)
        self.lib.d88_file_init();self.assertEqual(self.lib.d88_file_cleanup_error(),IO)
    def test_failed_open_after_creating_partial_is_cleaned(self):
        self.lib.model_fail(OPEN,1,2);r=self.job(15,words(3,crc(b'abc'),0,len(DEFAULT))+DEFAULT)
        self.assertEqual(r.result,IO);self.assertEqual(self.names(),[]);self.assert_closed()
    def test_short_write_closes_and_cleans(self):
        t=self.put_open(b'abc');self.lib.model_fail(WRITE,1,2);r=self.job(16,words(t,0,crc(b'abc'))+b'abc')
        self.assertEqual(r.result,IO);self.assertEqual(self.names(),[]);self.assert_closed()
    def test_temp_collision_is_never_truncated(self):
        collision=DEFAULT[:DEFAULT.rfind(b'\\')+1]+b'~D000001.TMP';self.seed(collision,b'preserve')
        r=self.put(b'abc');self.assertEqual(r.result,0);self.assertEqual(self.content(collision),b'preserve');self.assertEqual(self.content(DEFAULT),b'abc');self.assert_closed()
    def test_abort_zero_idempotent_and_token_checked(self):
        self.assertEqual(self.job(18,words(0)).result,0);t=self.put_open(b'abc')
        self.assertEqual(self.job(18,words(t+1)).result,STATE);self.assertEqual(self.job(18,words(0)).result,0);self.assertEqual(self.names(),[]);self.assertEqual(self.job(18,words(0)).result,0);self.assert_closed()
    def test_cancel_open_transaction_keeps_same_last_job(self):
        t=self.put_open(b'abc');r=self.poll(1);self.assertEqual(r.state,3);self.lib.d88_file_cancel()
        r=self.drain(1);self.assertEqual(r.result,CANCELLED);self.assertEqual(self.names(),[]);self.assert_closed()
    def test_single_cancel_inside_temp_open_cleans_returned_handle(self):
        self.seed(DEFAULT,b'old');self.lib.model_cancel_in_call(OPEN,1)
        r=self.job(15,words(3,crc(b'new'),1,len(DEFAULT))+DEFAULT)
        self.assertEqual(r.result,CANCELLED);self.assertEqual(r.length,0)
        self.assertEqual(self.lib.model_cancel_injections(),1)
        self.assertEqual(self.content(DEFAULT),b'old');self.assertEqual(self.names(),[DEFAULT]);self.assert_closed()
    def test_single_reset_or_stop_cancel_inside_write_remains_schedulable_until_drained(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1)
        self.lib.model_cancel_in_call(WRITE,1);i=self.submit(16,words(t,0,crc(b'new'))+b'new')
        for _ in range(20):
            self.assertTrue(self.lib.d88_file_needs_step())
            self.lib.model_step()
            if self.lib.model_cancel_injections():break
        self.assertEqual(self.lib.model_cancel_injections(),1)
        r=self.poll(i);self.assertEqual(r.job_id,i);self.assertEqual(r.state,2);self.assertEqual(r.length,0)
        other=C.c_uint();self.assertEqual(self.lib.d88_file_submit(13,words(len(DEFAULT))+DEFAULT,len(DEFAULT)+4,C.byref(other)),BUSY)
        self.assertEqual(other.value,0)
        for _ in range(100):
            if not self.lib.d88_file_pending():break
            self.assertTrue(self.lib.d88_file_needs_step(),'cancelled active handle stopped scheduling GUI cleanup')
            self.lib.model_step()
        r=self.poll(i);self.assertEqual(r.state,3);self.assertEqual(r.result,CANCELLED)
        self.assertEqual(self.lib.model_cancel_injections(),1)
        self.assertEqual(self.content(DEFAULT),b'old');self.assertEqual(self.names(),[DEFAULT]);self.assert_closed()
    def test_single_cancel_inside_download_read_does_not_publish_partial_success(self):
        self.seed(DEFAULT,b'abc');r=self.job(13,words(len(DEFAULT))+DEFAULT)
        t=U32.unpack(bytes(r.data[:4]))[0];self.lib.model_cancel_in_call(READ,1)
        r=self.job(14,words(t,0,3));self.assertEqual(r.result,CANCELLED);self.assertEqual(r.length,0)
        self.assertEqual(self.lib.model_cancel_injections(),1)
        self.assertEqual(self.content(DEFAULT),b'abc');self.assertEqual(self.names(),[DEFAULT]);self.assert_closed()
    def test_cancel_during_last_get_close_is_not_put_commit_success(self):
        self.seed(DEFAULT,b'abc');r=self.job(13,words(len(DEFAULT))+DEFAULT);t=U32.unpack(bytes(r.data[:4]))[0]
        self.assertEqual(self.job(14,words(t,0,3)).result,0)
        self.lib.model_cancel_in_call(CLOSE,2)  # Initial read handle, then reopened verification handle.
        r=self.job(17,words(t));self.assertEqual(r.result,CANCELLED);self.assertEqual(r.length,0)
        self.assertEqual(self.lib.model_cancel_injections(),1);self.assertEqual(self.content(DEFAULT),b'abc');self.assert_closed()
    def test_inflight_write_cancel_preserves_cleanup_failure_diagnostic(self):
        self.seed(DEFAULT,b'old');t=self.put_open(b'new',replace=1)
        self.lib.model_cancel_in_call(WRITE,1);self.lib.model_fail(CLOSE,1,1)
        r=self.job(16,words(t,0,crc(b'new'))+b'new')
        self.assertEqual(r.result,IO);self.assertEqual(self.lib.d88_file_cleanup_error(),IO)
        self.assertEqual(self.content(DEFAULT),b'old');self.assertEqual(self.names(),[DEFAULT]);self.assert_closed()
    def test_profile_loss_never_touches_fs_and_preserves_pending(self):
        t=self.put_open(b'abc');i=self.submit(16,words(t,0,crc(b'abc'))+b'abc');n=self.lib.model_fs_calls();self.lib.model_profile(0)
        self.lib.model_step();self.assertEqual(self.lib.model_fs_calls(),n);self.assertTrue(self.lib.d88_file_pending())
        self.assertEqual(self.poll(i).result,PROFILE);self.assertEqual(self.lib.d88_file_fatal_error(),PROFILE)
        self.lib.d88_file_cancel();self.lib.model_profile(1);self.lib.d88_file_init();self.lib.model_step()
        self.assertEqual(self.lib.model_fs_calls(),n);self.assertEqual(self.lib.d88_file_fatal_error(),PROFILE)
        self.assertEqual(self.lib.model_handles(),1);self.assertTrue(self.lib.d88_file_pending())
    def test_ordinary_crc_and_notfound_errors_are_not_latched(self):
        self.assertEqual(self.job(13,words(len(DEFAULT))+DEFAULT).result,NOTFOUND)
        self.assertEqual(self.lib.d88_file_fatal_error(),0);self.assertEqual(self.lib.d88_file_cleanup_error(),0)
        t=self.put_open(b'abc',expected=123);self.put_data(t,b'abc');self.assertEqual(self.job(17,words(t)).result,CRC)
        self.assertEqual(self.lib.d88_file_fatal_error(),0);self.assertEqual(self.lib.d88_file_cleanup_error(),0)
        self.assertEqual(self.put(b'valid').result,0);self.assert_closed()
    def test_download_chunks_and_second_crc_pass(self):
        data=bytes(range(211))*5;self.seed(DEFAULT,data);r=self.job(13,words(len(DEFAULT))+DEFAULT);self.assertEqual(r.result,0)
        t,n=struct.unpack('<II',bytes(r.data[:8]));self.assertEqual(n,len(data));out=b''
        while len(out)<n:
            r=self.job(14,words(t,len(out),480));self.assertEqual(r.result,0)
            token,at,c=struct.unpack('<III',bytes(r.data[:12]));part=bytes(r.data[12:r.length]);self.assertEqual((token,at,c),(t,len(out),crc(part)));out+=part
        self.assertEqual(out,data);r=self.job(17,words(t));self.assertEqual(r.result,0);self.assertEqual(struct.unpack('<III',bytes(r.data[:12])),(t,n,crc(data)));self.assert_closed()
    def test_download_change_is_rejected_at_finish(self):
        self.seed(DEFAULT,b'old');r=self.job(13,words(len(DEFAULT))+DEFAULT);t=U32.unpack(bytes(r.data[:4]))[0]
        self.assertEqual(self.job(14,words(t,0,480)).result,0);self.seed(DEFAULT,b'new');r=self.job(17,words(t));self.assertEqual(r.result,CRC);self.assert_closed()
    def test_download_size_limit_and_seek_failures(self):
        self.seed(DEFAULT,b'a');self.lib.model_report_size(DEFAULT,16*1024*1024+1)
        self.assertEqual(self.job(13,words(len(DEFAULT))+DEFAULT).result,SIZE);self.assert_closed()
        self.lib.model_reset();self.seed(DEFAULT,b'a');self.lib.model_fail(SEEK,1,1)
        self.assertEqual(self.job(13,words(len(DEFAULT))+DEFAULT).result,IO);self.assert_closed()
    def test_paths_dbcs_and_rejections(self):
        valid=b'A:\\folder\\'+bytes([0x81,0x5c])+b'.EXE';self.assertEqual(self.put(b'x',valid).result,0);self.assertEqual(self.content(valid),b'x')
        for p in [b'relative',b'B:\\x',b'A:\\..\\x',b'A:\\.\\x',b'A:\\a\\\\b',b'A:\\x.',b'A:\\x ',b'A:\\x\0',b'A:\\x*',b'A:\\'+bytes([0x81]),b'A:\\'+bytes([0x81,0x7f]),b'A:\\'+b'a'*238]:
            with self.subTest(p=p):self.assertEqual(self.job(13,words(len(p))+p).result,PATH)
        self.assert_closed()
    def test_directory_capability_is_explicitly_unsupported(self):
        self.assertEqual(self.job(11,words(3)+b'A:\\').result,UNSUPPORTED);self.assertEqual(self.lib.model_fs_calls(),0)
    def test_bad_profile_rejects_before_submission(self):
        self.lib.model_profile(0);i=C.c_uint(5);self.assertEqual(self.lib.d88_file_submit(13,words(len(DEFAULT))+DEFAULT,len(DEFAULT)+4,C.byref(i)),PROFILE);self.assertEqual(i.value,0);self.assertEqual(self.lib.model_fs_calls(),0)

class CapturedAdapterTests(unittest.TestCase):
    def test_actual_table_and_signatures(self):
        path=ROOT/'reference/hardware-kernel-body.bin'
        if not path.is_file():self.skipTest('private captured firmware unavailable')
        body=path.read_bytes();source=(ROOT/'device/file_native.c').read_text(encoding='utf8')
        for a,n,raw in re.findall(r'\{0x([0-9a-f]{8})u,(\d+)u,\{([^}]*)\}\}',source):
            a,n=int(a,16),int(n);code=bytes(int(x,16) for x in raw.split(','));self.assertEqual(code,body[a-0x02000000:a-0x02000000+n])
        raw=source.split('static const unsigned entries[29]={',1)[1].split('};',1)[0]
        entries=[int(x,16) for x in re.findall(r'0x([0-9a-f]+)u',raw)]
        self.assertEqual(len(entries),29);self.assertEqual(struct.pack('<29I',*entries),body[0x1e7ae0:0x1e7b54])
        self.assertNotIn('0x0217ea76u',source);self.assertIn('d88_gui_jobs_context()',source)
    def test_target_strict_compile(self):
        cc=target_compiler()
        if not cc.exists():self.skipTest('Target compiler not installed')
        with tempfile.TemporaryDirectory() as t:
            for name in ['file_service','file_native']:
                subprocess.run([str(cc),'--target=s1c33-none-elf','-Os','-ffreestanding','-fno-builtin','-Wall','-Wextra','-Werror','-c',str(ROOT/f'device/{name}.c'),'-o',str(Path(t)/f'{name}.o')],check=True,capture_output=True,text=True)

class AdapterGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc=native_gcc()
        if not Path(gcc).is_file():raise unittest.SkipTest('GCC required')
        cls.tmp=tempfile.TemporaryDirectory();lib=Path(cls.tmp.name)/'adapter.dll'
        env=dict(os.environ);env['PATH']=str(Path(gcc).parent)+os.pathsep+env.get('PATH','')
        subprocess.run([gcc,'-DD88_FILE_ADAPTER_MODEL','-std=c99','-O2','-Wall','-Wextra','-Werror','-shared',str(ROOT/'tests/file_service_model.c'),'-o',str(lib)],check=True,capture_output=True,text=True,env=env)
        cls.lib=C.CDLL(str(lib));l=cls.lib
        l.d88_file_native_exists.argtypes=(C.c_char_p,);l.adapter_corrupt.argtypes=(C.c_uint,)
        l.adapter_context.argtypes=(C.c_uint,);l.adapter_stat.argtypes=(C.c_int,C.c_uint)
        l.adapter_execution.argtypes=(C.c_uint,C.c_uint,C.c_uint,C.c_uint)
        l.adapter_race.argtypes=(C.c_uint,)
        l.adapter_dir.argtypes=(C.c_uint,)
    @classmethod
    def tearDownClass(cls):
        if hasattr(cls,'lib'):
            if os.name=='nt':_ctypes.FreeLibrary(cls.lib._handle)
            else:_ctypes.dlclose(cls.lib._handle)
            del cls.lib
        cls.tmp.cleanup()
    def setUp(self):self.lib.adapter_reset()
    def test_profile_context_and_initialized_gates_before_stat(self):
        l=self.lib;self.assertEqual(l.d88_file_native_available(),1)
        for address in (0x02000208,0x021e7ae0,0x021ab7ac,0x0205ac84):
            l.adapter_reset();l.adapter_corrupt(address)
            self.assertEqual(l.d88_file_native_exists(DEFAULT),-1);self.assertEqual(l.adapter_calls(),0)
        l.adapter_reset();l.adapter_context(0)
        self.assertEqual(l.d88_file_native_available(),1);self.assertEqual(l.d88_file_native_exists(DEFAULT),-1);self.assertEqual(l.adapter_calls(),0)
    def test_real_stat_absence_code_is_12_and_other_failures_are_errors(self):
        l=self.lib;self.assertEqual(l.d88_file_native_exists(DEFAULT),1)
        l.adapter_stat(-1,0x12);self.assertEqual(l.d88_file_native_exists(DEFAULT),0)
        for error in (0x30,0x01,0x14,0xc6,0xd0):
            l.adapter_stat(-1,error);self.assertEqual(l.d88_file_native_exists(DEFAULT),-1)
    def test_captured_directory_bit_rejects_replacement(self):
        l=self.lib;l.adapter_dir(1)
        self.assertEqual(l.d88_file_native_exists(DEFAULT),-1);self.assertEqual(l.adapter_calls(),1)
        l.adapter_dir(0);self.assertEqual(l.d88_file_native_exists(DEFAULT),1)
        l.adapter_corrupt(0x021b0420)
        self.assertEqual(l.d88_file_native_exists(DEFAULT),-1);self.assertEqual(l.adapter_calls(),2)
    def test_metadata_locks_require_full_signatures_and_ordinary_context(self):
        l=self.lib
        for context in ((0,0,0,0),(0x10,1,0,0),(0x10,0,1,0),(0x10,0,0,1),(0x10,0,0,4)):
            l.adapter_reset();l.adapter_execution(*context);self.assertEqual(l.d88_file_native_lock(),-1);self.assertEqual(l.adapter_lock_depth(),0)
        for address in (0x021ce8ae+40,0x021ce8d8+56,0x021e8244,0x02068664,0x022a9850):
            l.adapter_reset();l.adapter_corrupt(address);self.assertEqual(l.d88_file_native_lock(),-1);self.assertEqual(l.adapter_lock_depth(),0)
        l.adapter_reset();self.assertEqual(l.d88_file_native_lock(),0);self.assertEqual(l.adapter_lock_depth(),1)
        self.assertEqual(l.d88_file_native_unlock(),0);self.assertEqual(l.adapter_lock_depth(),0)
    def test_metadata_rechecks_after_lock_and_rolls_back(self):
        l=self.lib;l.adapter_race(0x021e8244)
        self.assertEqual(l.d88_file_native_lock(),-1);self.assertEqual(l.adapter_lock_depth(),0)

if __name__=='__main__':unittest.main(verbosity=2)
