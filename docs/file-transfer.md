# 文件传输与原生 GUI 按键

文件扩展沿用 `88DB` 版本 1 帧和严格 9288 身份检查，不提供任意执行或原始闪存操作。机端文件系统调用由原生 GUI 任务执行；USB worker 只提交任务、查询结果和标记取消。新主机代码已通过离线协议回归，物理文件传输是否通过须查看对应 `image_id`、SHA256 和验证记录，不能沿用旧构建的 USB/截图验收结论。

当前支持上传和下载。目录列表的协议及主机 API/CLI 入口已预留，但当前机端 `LIST_OPEN/LIST_NEXT` 返回 `UNSUPPORTED (-715)`；图形界面不提供目录浏览，CLI `file-list` 也不能作为已可用的目录浏览功能。传输应使用已知完整路径，目标目录必须存在。

## 使用

先安装具有 `CAP_FILES` 能力的新版机端服务。旧驻留映像必须按 README 的更新流程重启并重新激活，覆盖磁盘中的程序不会替换已经运行的旧代码。仅升级电脑 EXE 也不会给旧服务增加命令。图形界面的文件按钮会在能力位缺失时禁用。

电脑端源码运行依赖 `requirements.txt`。从项目目录执行，替换示例中的 COM3：

```powershell
python -m host.client list
python -m host.client --port COM3 status
python -m host.client --port COM3 --report .\captures\upload.json file-put .\dist\9288MemoryProbe.exe 'A:\系统\程序\9288MemoryProbe.exe'
python -m host.client --port COM3 --report .\captures\download.json file-get 'A:\系统\程序\9288MemoryProbe.exe' .\captures\9288MemoryProbe-readback.exe
```

默认不覆盖机端已有文件，返回 `EXISTS (-705)` 时原文件保留。显式允许覆盖：

```powershell
python -m host.client --port COM3 --report .\captures\replace.json file-put .\dist\9288MemoryProbe.exe 'A:\系统\程序\9288MemoryProbe.exe' --replace
```

图形界面进入“文件传输”页，填写完整机端文件路径；若路径以反斜杠结尾，上传时会追加所选电脑文件的名称。上传和下载共享唯一串口线程，不能同时操作同一客户端。下载先写电脑目标目录的 `.part-*` 文件，完成大小和 CRC32 校验后才替换本地目标。上传只有在临时文件校验、提交及最终目标读回成功后才报告 `committed=true`；它不会自动运行上传的程序。

路径必须在 `A:\` 下，以 GBK 编码，编码后最多 240 字节，不带 NUL。拒绝其他盘、相对路径、空中间分量、`.`、`..`、非法名称及不可编码字符。主机先按 Unicode 分解和规范化路径，再严格编码；机端按双字节字符边界验证。GBK 字符的第二字节可能是 `0x5C`，不能将其错误地拆成路径分隔符。协议长度是字节数，不是汉字数。

单文件上限为 16MiB（16,777,216 字节），每个数据块最多 480 字节。空文件也需执行 OPEN/FINISH 并校验 CRC32。主机不把整个文件加载进 RAM，而是逐块传输。

## 取消、断线和恢复

图形界面的“取消传输”只设置取消事件，串口线程在安全边界发取消命令。已经提交的任务仍使用原 `job_id`，持续查询，直到 GUI 任务结束文件调用及清理。不能为了取消而用新的 ABORT 任务覆盖仍在执行的任务。若最后的提交已经完成，取消请求可能得到成功结果；客户端会按实际结果报告成功，不会假称回滚。

数据进度达到 100% 后，还需等待机端完整 CRC 校验和提交，较大文件可能需要数分钟；界面会显示此阶段，取消按钮仍可用。FINISH 任务预算至少 900 秒（15 分钟），其他文件任务预算保持至少 60 秒；单次串口 RPC 仍使用客户端 `--timeout`，没有被延长为 15 分钟。进度达到 100% 本身不是上传成功回执。

CLI 遇到 Ctrl-C 或传输异常会关闭会话；协议 CRC、序号、长度或超时使客户端会话失效，随后不自动重放写入、提交或清理命令。机端会在会话断开后安排 GUI 任务清理。正常文件错误有明确完成回执时，客户端可保持连接，并在同步状态下清理其活动事务。不能因为 USB 回复丢失而假定上传没有提交；应重新连接并下载目标核对。

文件系统调用可能等待原生任务。断线或 STOP 的取消标记会保留到该调用返回，再让同一任务继续执行清理；一次取消信号足以关闭句柄，不需要电脑再补一次命令。只有实际完成的上传提交可以胜出最后到达的取消；下载最后关闭期间收到取消时仍返回取消结果。

上传临时文件与目标在同一目录。替换时旧目标先保存到本事务备份，直到新目标完整读回校验成功再移除备份；失败时尝试恢复原文件。该流程不等于断电可恢复的文件系统日志，也不应在校验提交期间直接关闭设备电源。

`RECOVERY (-714)` 表示需要保留现场恢复，可能存在唯一的原文件备份或不确定的重命名结果。客户端保留错误负载，不自动 ABORT、重新上传或删除备份；GUI 暂停进一步文件操作，仍可查询其他调试能力。CLI JSON 含 `recovery_required=true`、`outcome_unknown=true`、`file_job_id`、`recovery_data_hex`。先保存报告及目标、临时文件、备份，不要按名字批量删除 `~D*.TMP/.BAK`。

RECOVERY 也可能在提交请求时直接返回，不一定附带路径数据；空负载不表示现场已经恢复。文件系统关闭、清理或恢复重命名失败会保留故障诊断并拒绝后续文件提交，既有任务结果仍可查询。STOP 不会在这类清理故障下假报正常完成；应保存诊断和备份，可能需要设备正常重启才能恢复服务。

恢复负载是小端 `{token, temp_path_bytes, temp_path[GBK], backup_path_bytes, backup_path[GBK]}`，路径不带 NUL。可在电脑只读解析 JSON 查看路径：

```python
import json, struct
from pathlib import Path

report = json.loads(Path("captures/replace.json").read_text(encoding="utf-8"))
raw = bytes.fromhex(report["recovery_data_hex"])
token, n = struct.unpack_from("<II", raw)
temporary = raw[8:8+n].decode("gbk", errors="strict")
offset = 8 + n
m, = struct.unpack_from("<I", raw, offset)
backup = raw[offset+4:offset+4+m].decode("gbk", errors="strict")
assert len(raw) == offset + 4 + m
print(token, temporary, backup)
```

## 协议布局

权威常量及结构位于 `device/file_protocol.h`。现有 op1..10 保持不变；`STATUS.caps` 的 bit6 为 `CAP_FILES`。帧负载仍最多 512 字节，外层 IEEE CRC32 覆盖头与负载。下面的整数均为小端 u32，`result` 按有符号 32 位解释；字符串为显式长度 GBK 字节。

op11..18 是提交命令，同步回 `{job_id,result}` 共 8 字节。接受时 job_id 非零、result=0；提交拒绝时 job_id=0、result 为负错误。接受不代表文件操作已完成。op19 查询同一 job，回 `{job_id,state,result,length,data[length]}`；固定头 16 字节，data 最多 496 字节。state 为 1 QUEUED、2 RUNNING、3 DONE；只有 DONE 含命令结果，排队或运行状态的 result/length 均为 0。

| op | 命令 | 请求负载 | DONE 成功数据 |
|---|---|---|---|
| 11 | LIST_OPEN，当前不支持 | `{path_bytes,path}` | 预留 `{token}` |
| 12 | LIST_NEXT，当前不支持 | `{token,index}` | 预留 `{token,index,eof,attributes,size,name_bytes,name}` |
| 13 | GET_OPEN | `{path_bytes,path}` | `{token,size}` |
| 14 | GET_READ | `{token,offset,wanted}`，wanted=1..480 | `{token,offset,chunk_crc32,data}` |
| 15 | PUT_OPEN | `{size,full_crc32,flags,path_bytes,path}`，flags.bit0=replace，其余为0 | `{token,next_offset=0}` |
| 16 | PUT_DATA | `{token,offset,chunk_crc32,data}`，data=1..480 字节 | `{token,next_offset,running_crc32}` |
| 17 | FINISH | `{token}` | GET/PUT 为 `{token,size,full_crc32}`；LIST 预留空数据 |
| 18 | ABORT | `{token}`，token=0 表示本会话活动事务 | 清理完毕后空数据 |
| 19 | POLL | `{job_id}` | 同步回任务状态及其数据，见上文 |
| 20 | CANCEL_JOB | `{job_id}` | 同步空回执；随后继续 POLL 原 job |

同一会话最多一个活动文件/目录事务及一个未完成命令，文件 token 与截图 token 独立。偏移必须连续精确匹配，不能凭超时猜测下一位置或自动重试 PUT_DATA。每块都有 CRC32；FINISH 还核对整个文件大小、完整 CRC32。上传会关闭并重开临时文件逐块校验，提交后还校验最终目标；下载核对接收到的全流 CRC。CRC 用于传输完整性，不是密码学身份认证。

错误为 `ARGUMENT=-701`、`BUSY=-702`、`IO=-703`、`NOT_FOUND=-704`、`EXISTS=-705`、`CRC=-706`、`STATE=-707`、`PATH=-708`、`PROFILE=-709`、`CANCELLED=-710`、`SIZE=-711`、`LIMIT=-712`、`COMMIT=-713`、`RECOVERY=-714`、`UNSUPPORTED=-715`。未知错误编码、job 不匹配、异常状态或长度使主机会话失效。

## 按键范围和计时边界

`press LEFT`、`press ENTER` 等命令向原生 GUI 当前活动窗口或焦点控件发送一次完整 DOWN/UP，扫描码范围为 1..127。两条消息保持相同目标，目标销毁时不将 UP 重定向给新窗口。回执表示接受排队，不保证程序已处理，应刷新截图核对。

图形界面仅在正在发送按键时保留最多 16 个后续按键，保持顺序；队列满时忽略新增项并提示。文件传输或其他长操作期间不积压按键；断线、新操作及退出时清空旧队列。该输入不模拟按住/多键和物理键盘矩阵，直接轮询硬件按键的游戏不在支持范围。

文件和截图的 USB 传输耗时只能描述连接吞吐量，不能用于推算 SDRAM、IRAM 或 IVRAM 的内存带宽。D88MEM 的计时和验证记录独立保存，文件上传仅用于部署程序。
