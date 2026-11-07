#ifndef D88_FILE_PROTOCOL_H
#define D88_FILE_PROTOCOL_H

#define D88_FILE_VERSION 1u
#define D88_FILE_PATH_MAX 240u
#define D88_FILE_CHUNK 480u
#define D88_FILE_MAX_REPLY 496u
#define D88_FILE_MAX_BYTES (16u*1024u*1024u)
#define D88_FILE_PUT_REPLACE 1u

enum {
    D88_FILE_LIST_OPEN=11u,D88_FILE_LIST_NEXT=12u,
    D88_FILE_GET_OPEN=13u,D88_FILE_GET_READ=14u,
    D88_FILE_PUT_OPEN=15u,D88_FILE_PUT_DATA=16u,
    D88_FILE_FINISH=17u,D88_FILE_ABORT=18u,D88_FILE_POLL=19u,
    D88_FILE_CANCEL_JOB=20u,D88_APP_REFRESH=21u
};
enum { D88_FILE_IDLE=0u,D88_FILE_QUEUED=1u,D88_FILE_RUNNING=2u,D88_FILE_DONE=3u };
enum {
    D88_FILE_OK=0,D88_FILE_E_ARGUMENT=-701,D88_FILE_E_BUSY=-702,
    D88_FILE_E_IO=-703,D88_FILE_E_NOT_FOUND=-704,D88_FILE_E_EXISTS=-705,
    D88_FILE_E_CRC=-706,D88_FILE_E_STATE=-707,D88_FILE_E_PATH=-708,
    D88_FILE_E_PROFILE=-709,D88_FILE_E_CANCELLED=-710,D88_FILE_E_SIZE=-711,
    D88_FILE_E_LIMIT=-712,D88_FILE_E_COMMIT=-713,D88_FILE_E_RECOVERY=-714,
    D88_FILE_E_UNSUPPORTED=-715
};
/* Existing v1 frames and op1..10 retain their layout. All integers here LEu32.
 * Submit op11..18 reply: {job_id,result_signed},8 bytes (id0 on rejection).
 * Poll19 request{job_id}, reply{job_id,state,result_signed,length,data[length]}.
 * GUI FS steps publish DONE only when that command is complete. At most one
 * outstanding command and one directory/file transaction per USB session.
 * Cancel20 request{job_id}, empty reply; marks cancel and retains the same job
 * until its GUI cleanup/DONE result. It never interrupts an in-flight FS call.
 *
 * LIST_OPEN/GET_OPEN request{path_bytes,path[GBK bytes without NUL]}.
 * LIST_OPEN result{token}; LIST_NEXT request{token,index}; its result is
 * {token,index,eof,attributes,size,name_bytes,name[name_bytes]}.
 * GET_OPEN result{token,size}; GET_READ request{token,offset,wanted<=480};
 * result{token,offset,chunk_crc32,data[<=480]}.
 * PUT_OPEN request{size,full_crc32,flags,path_bytes,path[GBK]},flags bit0 replace;
 * result{token,next_offset=0}. PUT_DATA request{token,offset,chunk_crc32,data};
 * result{token,next_offset,running_crc32}. Data chunks contain1..480 bytes.
 * FINISH request{token}; GET/PUT result{token,size,full_crc32},LIST result empty.
 * PUT finish verifies closed temporary file in chunks, then guarded rename and
 * final readback; replacement must preserve/recover original via owned backup.
 * ABORT request{token}, token0 means this session's active transaction; empty
 * result once its handles/owned temporary files have been cleaned by GUI1.
 * APP_REFRESH21 submits an empty request on this same exclusive GUI job lane.
 * It requires no active file transaction and the native desktop to be active.
 * Success data is six LEu32: {version1,before_total,after_total,
 * before_games,after_games,post_result0}; games is native category8.
 * Cancel before execution prevents the scan; once scanning starts the complete
 * scan/post result wins cancellation and is never automatically replayed.
 * Paths are absolute A:\,DBCS-aware GBK; reject NUL,invalid bytes, . / .. path
 * components and overlength. No raw NAND, formatting, arbitrary EXEC or delete.
 */
#endif
