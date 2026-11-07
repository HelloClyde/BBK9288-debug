# 9288 Debug（实验驻留版）

当前 `f9dc2295` 已在真机通过连续 5 次普通串口重连（没有额外 DTR 等待）、20KiB 上传/下载/替换/取消、同名文件保护及原生 GUI 左右键测试。按键后截图逐像素恢复，最终 1294 次请求、0 错误。当前文件身份、安装和验证范围以 `dist/service-build.json`、`dist/validation.json` 为准；目录列表暂不支持。重连修订说明见 [serial-reconnect.md](serial-reconnect.md)。

历史构建 `d7af4f73` 已在真机通过自动关机保护验证：原设置为 5 分钟，关闭电脑客户端、无指令静置 331.078 秒后，COM3 仍可严格握手和截图，后台空闲计数为 0，原设置不变。这是该次历史运行的结果，不代表新构建已安装或服务仍在运行；该次未验证 STOP 后的自动关机恢复。

参照 H1Debug，为**原版 9288**实现电脑端截图、GUI 按键和 RAM 检查。当前已实现系统堆驻留、原生 ROS 后台任务、USB CDC 接管与退出恢复；目标固件是已采集真机内核 `CAPTURED-0215EBC4`，内部版本字符串为 V1.3。它与此前本地 V1.5 安装包内核不同，也不适用于 9288S 或 H1。

**构建 `f156bf41` 已在真机枚举为 USB CDC，身份握手、320×240 截图、原生 GUI 左右键、RAM 读取和专属 scratch 读写恢复均通过。** 旧构建 `3f7a2a62` 的 Code43 已有诊断报告，且停止后磁盘重连、报告读回成功；`f156bf41` 的 STOP 与磁盘恢复尚未在真机测试。驻留服务另有两轮原生内核模型回归。

首次真机 USB 验收未发现 9288 调试设备，随后机端显示 `State=00000006 / Error=FFFFFFF4`（驱动平台状态检查失败）。电脑上的 COM4 实例为 `H1DBG-V141-01`，属于 H1，不能当作 9288 端口。后续修订补上每次 Misc USB 时钟写入前的单次解锁、短 CPU 锁和写后校验，并保留分项失败诊断。测试模型也开始强制执行该写保护；USB snooze 不再被当作控制器复位。

补上写保护后，第二轮真机在原端口出现“设备描述符请求失败”（Code43），仍无 9288 调试串口。该轮修订将 USBCLKEN 开启后的稳定等待放在 USBSNZ 清除之前，另保留 D+ 软件断开等待；设备类改为 `02/02/01` 以匹配 Windows 自带串口驱动，并加入停止时的持久诊断报告。这些是已核对的实现修正，不能仅据此确认当时 Code43 的物理根因。

2026-11-04 的构建 `3f7a2a62` 已留下 CRC 正确的 `D88U01.BIN`：后台处于 RUNNING，完成 22976 次 poll，收到 4 次 SETUP，最后的 18 字节 GET_DESCRIPTOR 回复没有完成。旧代码在启动描述符引擎后写 `EP0_IN=08`，会清掉硬件自动设置的 EnShortPkt。随后修订先设置 IN 方向和 DATA1/NAK，再启动描述符回复，最后只清 ForceNAK；模型已能重现旧写法阻塞短回复。它修正了明确的代码问题，**尚不能证明短包是 Code43 的唯一物理根因**。同次修订也修正了停止请求后误显示旧 READY 的界面竞态。

## 文件用途

|文件|在哪里运行|用途|
|---|---|---|
|`dist/9288Debug-PC.exe`|Windows 电脑|图形客户端，内置 Python、pyserial 和 Pillow，无需另装依赖|
|`启动9288Debug.cmd`|Windows 电脑|优先打开上述客户端，也支持 Python 源码环境|
|`dist/9288Debug.exe`|9288，菜单名 **D88DEBUG**|安装、启动或停止堆驻留调试服务|
|`dist/9288DebugProbe-v3.exe`|9288，菜单名 **D88PRB3**|采集 `DBG88P01..05.BIN` 状态报告，用于识别固件和排查问题|
|`dist/9288MemoryProbe.exe`|9288，菜单名 **D88MEM**|独立机端内存申请探测和读取/写入/复制测速，由 D88DEBUG 读回报告|
|`dist/9288LayoutProbe.exe`|9288，菜单名 **D88MAP**|只读系统/IRAM 布局探测，以及自有静态区和 malloc 同时使用 2.25MiB 的验证|
|`dist/service-build.json`|电脑查看|当前驻留版的 image_id、SHA256、绑定固件和验证边界|

这些机端 `.exe` 文件不能在 Windows 上运行。**D88PRB3 负责生成报告，D88DEBUG 负责启动调试服务，D88MEM/D88MAP 是独立内存诊断程序；普通 USB 磁盘模式本身不提供调试串口。** 当前交付包不再分发旧前台 dev 程序，驻留版身份以 `service-build.json` 为准。

## 安装和首次激活

机端服务安装到 USB 卷的 `系统\程序\9288Debug.exe`。当前构建摘要见 `dist/service-build.json`；已通过的真机项目和剩余边界见下文。**更新构建后必须通过设备正常重启清除旧驻留，再运行新版；USB 线可以一直插着供电。** 覆盖文件或关闭提示框不会替换已经驻留的代码。

1. 用原生 USB 磁盘模式，把 `dist/9288Debug.exe` 复制到 9288 的 `系统/程序` 目录。需要再次采报告时，也可复制 `9288DebugProbe-v3.exe`。
2. 等电脑文件传输结束并安全弹出磁盘，再在 **9288 上退出 USB 磁盘界面**，回到下载程序菜单。**USB 线可以一直插着供电。** 电脑弹出与真机退出是两个步骤。
3. 看到 **D88DEBUG READY** 后确认并返回菜单。服务的代码、数据、栈和截图缓冲已复制到系统堆，由独立任务继续运行。
4. 无需拔插线，在 Windows 打开 **`9288Debug-PC.exe`**，刷新并选择出现的调试 COM 端口。服务会关闭 USB 数据连接，经过有时间保证的等待后重新枚举为 CDC；原生磁盘服务此时暂停。
5. 先查询身份和状态，再尝试截图或单次按键。客户端支持原尺寸及 2 倍截图显示、方向/确认/返回/字母数字键和 RAM 保存。

真机验收中 `f156bf41` 出现在 COM3，VID:PID 为 `1209:9288`，PING 身份为 `BBK9288DEBUG/1 CAPTURED-0215EBC4 EXPERIMENTAL`；COM 号可能随电脑变化。没有出现 COM 口时，应记录 Windows 设备识别结果及机端提示。`PROFILE` 表示固件身份不匹配，`BUSY` 表示任务槽被其他构建或程序占用，`EXIT USB` 表示原生 USB 尚未达到可接管的停止状态。服务仍会拒绝活动磁盘会话及尚在协商的状态，回到菜单本身不能绕过底层检查。`STATUS/ERROR` 中的状态和错误值应保留用于排查。

`READY` 现在同时显示 `USB/CTRL/XCVR/SETUP` 的最近快照；失败界面还会保留 `PHASE/MASK/F20/F35/USBIRQ/TIMER/BCU/MISCFAIL`。这些是失败前记录的值，停止清理不会将其替换为恢复后的正常值。`MISCFAIL` 非零时，高字节是预期 F20，低字节是实际读回值。`READY` 只说明机端启动检查已通过，电脑是否完成枚举仍须单独确认。

## 停止、软件恢复与再次启动

在电脑客户端发送 **STOP**，或者在 9288 再运行一次 **D88DEBUG**，都会请求停止正在运行的服务。STOP 回复完成后，服务断开 CDC，保留 USB 供电；经过受保护的断开等待，再恢复原生 USB 状态、Timer3B 中断向量和 USB 任务，最后结束后台任务。**不再要求拔线。** 回复只是停止请求的回执，恢复随后完成。

从真机再次运行 D88DEBUG 停止时，新版会先自动保存 `A:\D88U01.BIN` 至 `A:\D88U99.BIN` 中首个空名。文件在设备根目录，已有报告保留；STOPPED 提示会显示所选路径或保存错误。保存期间暂停自有调试 worker，保持原生 USB 路径暂停，完成原生文件写入、关闭和读回校验后才继续停止。电脑端 STOP 不触发这份前台文件报告。

最新修订在 `stop_requested` 已置位时会继续等待停止完成；成功显示 **D88DEBUG STOPPED**，仍在恢复则显示 **D88DEBUG USB WAIT**。旧构建可能在已经请求停止并保存报告后，抢先显示“Background debug started / READY”；这个提示不能用来判断后台未停止。

重新进入磁盘模式后，电脑可复制根目录中的新报告并解析：

```powershell
python inspect_usb_log.py H:\D88U01.BIN --output usb-log-01.json
```

1024 字节报告带 CRC32，保存运行时控制块、驱动诊断、经过访问门控的 USB/时钟/中断/SETUP/描述符游标/DMA 状态，以及驻留映像中实际的设备描述符字节。寄存器是连续读取的快照，硬件仍可能在读取期间响应主机；不能把它们当作同一时刻的总线抓包。

原生服务重新获得控制器后，可重新进入磁盘模式。构建 `3f7a2a62` 已在真机停止后重新连接磁盘并读回 `D88U01.BIN`；这证明了该次停止与报告回收路径，最新修订仍需重复验证。若机端报告恢复错误，应保留错误值并正常重启设备。

停止后驻留内存保留到设备重启。原生磁盘模式退出后再次运行**完全相同构建**的 D88DEBUG，会验证 image_id 和任务身份，复用原来的堆内存及任务。正在运行时再次打开则请求停止。换成不同构建不能直接覆盖旧驻留服务；出现 `D88DEBUG BUSY` 时，先重启 9288，再运行新版本。

## 当前能力和边界

独立的 [D88MEM 内存基准](memory-benchmark.md) 已完成 30 项真机测试。本轮调试器驻留时最大连续申请为 1,733,036 字节，加 1 字节失败；测试块已释放，堆和目录恢复一致。片内 IRAM 读取约 9.91MiB/s，SDRAM 四个 bank 约 5.31–5.34MiB/s，未发现明显的 SDRAM 快慢分区。速率按 RTC 标称频率计算，完整数据和解释边界见该说明。D88MEM 与驻留调试器分开，不向 USB 协议增加任意 EXEC。

后续 D88MAP 已在真机同时使用 1.5MiB malloc 与程序窗口内的 768KiB 静态区，共 **2.25MiB**，校验后堆恢复。8MiB SDRAM 按固件划为前 4MiB 内核/静态/保留、1MiB 系统运行数据、2MiB 全局 malloc 和 1MiB 下载程序窗口；字库的两份副本约 1.81MiB。IRAM 为 16KiB，但当前未证明普通应用可独占任何范围，不应把旧 SDK 的 `0x0800` 链接声明当可分配空间。具体布局、扩展用法和实测证据见同一内存说明及 `dist/layout-validation.json`。

调试服务运行期间会定期调用已核对的原生自动关机计数复位函数；即使电脑暂未打开串口，也持续刷新。它保留系统设置中的关机分钟数，停止服务后从最近一次刷新开始正常倒计时。其他 GUI 计时器继续运行。已有原生关机请求时会拒绝启动，避免覆盖正在进行的关机操作。实现与固件依据见 [自动关机说明](auto-power-off.md)。

- 截图：读取当前受支持的 320×240 LCD 模式，输出 8 位灰度快照；不支持的显示配置会被拒绝。
- 输入：向经过指纹和队列检查的**原生 GUI 活动窗口或焦点控件**排队发送一组 DOWN/UP。排队成功不等于目标程序已经处理；直接读取键盘矩阵的游戏或自绘程序不在此接口的控制范围内。
- 文件：新增版本提供受限的上传、下载事务，使用原生文件系统和完整 CRC32 校验。旧驻留构建没有此能力，需更新机端服务；当前不提供目录浏览，预留的目录列表协议返回 `UNSUPPORTED`。实现与使用方式见[文件传输协议和使用说明](file-transfer.md)，新增功能的真机验收以当前构建记录为准。
- RAM：只读 SDRAM `02000000..027FFFFF`；写入只开放调试器自己的 512 字节 scratch 测试区。
- 驻留：服务放在系统全局堆，独立于 `02700000..027FFFFF` 下载窗口；模型已验证覆盖下载窗口后仍继续运行。

文件传输不提供自动运行、任意删除或格式化；任意 RAM 写入、任意代码执行及键盘矩阵模拟尚未提供。固件 API、任务槽和 USB 状态均有运行时检查；此构建仅接受已采集的真机配置。详细接管与恢复实现见 [USB ownership 说明](usb-platform-takeover.md)，重定位与寿命见 [resident loader 说明](resident-loader.md)。

## 文件上传、下载与电脑按键

新版电脑界面的“文件传输”页可输入已知机端路径后上传或下载，带进度及取消按钮。路径使用设备原生 `A:\` 和 GBK 编码，长度最多 240 字节；单文件最多 16MiB。目录列表的协议与主机入口已保留，当前机端返回 `UNSUPPORTED`。旧服务没有 `CAP_FILES` 时，文件按钮会禁用并提示更新；仅更新电脑客户端不会给旧驻留映像增加功能。

在 `9288-debug` 目录执行以下命令，COM3 应替换为本机实际 9288 端口。上传目标目录必须已存在，默认不覆盖已有文件：

```powershell
python -m host.client --port COM3 file-put .\dist\9288MemoryProbe.exe 'A:\系统\程序\9288MemoryProbe.exe'
python -m host.client --port COM3 file-get 'A:\系统\程序\9288MemoryProbe.exe' .\captures\9288MemoryProbe-readback.exe
```

确需替换已有文件时，显式增加 `--replace`；图形界面遇到同名目标时会单独询问是否覆盖。机端先写同目录临时文件，关闭并完整读回校验后再提交；覆盖路径保留旧文件备份，直到新目标读回通过。取消需等待机端清理，最后一步已完成提交时仍会返回成功。若报告 `RECOVERY (-714)`，应保存 JSON 中的 `recovery_data_hex`，保留现场的临时文件和备份，按[恢复说明](file-transfer.md)检查，不能自动重新上传或删除。

```powershell
python -m host.client --port COM3 --report .\captures\upload.json file-put .\dist\9288MemoryProbe.exe 'A:\系统\程序\9288MemoryProbe.exe' --replace
python -m host.client --port COM3 press LEFT
python -m host.client --port COM3 press ENTER
```

按键范围为扫描码 `1..127`，每次为一组原生 GUI DOWN/UP；电脑键盘不是任意游戏的键矩阵输入。界面仅在正在发送按键时保留最多 16 个后续按键，传输文件时不积压按键；断线或开始其他操作会清空旧队列。上传成功不代表程序已运行，需要在设备原生菜单中找到并启动它。

## 真机报告与验证范围

2026-11-03 已从真机 USB 卷读回 v3 完整报告 `DBG88P02.BIN`，CRC32 正确，函数表匹配，三段代码指纹全部通过。归档文件为 `captures/2026-11-03-probe-v3/DBG88P02-fb4e4c18c1ea.BIN`，SHA256：

```text
fb4e4c18c1ea71617ba2f4c8508b6e9ea8a82556b945408c9d58d327e525eac1
```

报告确认了实际 ROS/GUI 表、任务 3 的原生 USB 状态、空闲任务槽、系统堆和总线/时钟配置，作为接管时再次检查的依据。它证明了探针在这台真机上的完整采集成功；这份早期报告本身未执行 CDC 调试传输。

2026-11-04 的旧版 USB 诊断报告归档为 `captures/2026-11-04-powerup-usb-logs/D88U01-8cdccbdf2d5b.json`，对应 BIN 的 SHA256 为 `8cdccbdf2d5bf6ba562b206a427ffa855b6f73044b6930e046e008528d0fd6b2`。CRC32 正确，image_id 为 `3f7a2a62`，控制块 RUNNING、无平台错误、poll=22976、SETUP=4；最后请求为 `80 06 00 01 00 00 40 00`，描述符地址/剩余长度停在 `0040/0012`，EP0 IN 为 `10`（EnShortPkt 已清）。报告采集在停止之前；之后重新进入原生磁盘并成功读回。该旧版仍报 Code43。

随后构建 `f156bf41` 在同一真机上成功枚举并通过严格 PING 身份检查，STATUS 能力值为 63、错误数为 0。读取 `0215EBC4` 起的 64 字节与已采集内核完全一致；实际截图为 320×240、8 位灰度、76800 字节，首次传输用时 15.974 秒，随后 STATUS 为 157 次请求、0 错误。512 字节专属 scratch 的测试图案写入、读回和原内容恢复均通过。单次 LEFT 的 DOWN/UP 将菜单高亮从 D88DEBUG 移到 D88PRB3，单次 RIGHT 恢复到 D88DEBUG，已由三张原尺寸截图确认；没有发送 ENTER。这验证了该原生 GUI 菜单的左右键，不覆盖任意游戏的键盘矩阵。最后 STATUS 为 474 次请求、0 错误，截图 token/bytes 均已释放为 0。证据保存在本地 `captures/2026-11-04-ep0-short-ready-check/acceptance.json`、`functional-check.json` 及对应截图，这些私有采集文件不随交付包分发。该版本尚未发送 STOP，停止和磁盘恢复仍需单独真机验收。

`tools/test_service_native_model.py` 使用独立完整 NAND 副本启动原生内核，运行实际 launcher、重定位器和驻留代码；内存分配、任务创建/启动、挂起/恢复、延时、CPU 锁和任务退出均调用原生固件。回归已完成**两轮启动、下载窗口覆盖、停止恢复及同构建内存复用**，并执行驻留 Timer3B ISR；最新两轮也确认停止界面为 STOPPED，避免只检查稍后的后台状态而遗漏旧 READY 竞态。日志在 `build/native-service-model/observations.json`，源码和编译器摘要在 `model-build.json`。

这个测试的 USB 寄存器和共享 SRAM 由 guest adapter 模拟；带电回归让该窗口的 VBUS 持续为高，原生 QEMU USB 窗口仍未连接，不能据此证明原生磁盘退出或物理主机枚举。原生 ROS 定时器的计数也用于核对软件连接前和交还原生服务前的延时。它不验证硅片 FIFO、USB 电气时序或 MSC 重连。自动测试数量和本次固件模型结果见 `dist/validation.json`。上面的 `f156bf41` 实测独立验证了 CDC、截图及受限内存操作；新版 STOP 软件恢复和磁盘重连尚未实际测试。

## 探针使用与此前未生成文件的原因

需要新报告时，安全弹出磁盘并在真机退出磁盘界面，再运行 **D88PRB3**；供电线可以保留。它先显示 **D88PRB3 START**；确认采集后，只有看到 **D88PRB3 SAVED** 才表示保存及读回比较成功。报告在设备 A 盘根目录，按 `DBG88P01.BIN` 至 `DBG88P05.BIN` 选择第一个空名，不覆盖已有文件。当前真机的 01 已被旧报告占用，所以 v3 使用了 **02**；不能只找 01。

再接普通 USB 磁盘，把新文件复制到电脑后解析：

```powershell
python inspect_probe.py DBG88P02.BIN --output probe-02.json
```

v3 生成 1024 字节报告，包含 CRC32；先匹配运行时函数表和三段各 40 字节代码指纹，匹配后读取任务 1～8、系统堆、总线/时钟/中断及原生 USB 状态。未知配置仅保存基础信息。探针通过原生文件系统保存报告，不接管 USB、不挂起任务、不改时钟、不写 NAND 控制器。

最初 D88PROBE 仍在菜单且没有生成文件，是因为旧版在创建文件前要求本地安装包的 ROS/GUI 表 `021806EC/02180058`，而真机实际为 `021E81E8/021E7B54`。v2 基础报告已确认这个差异，v3 随后成功采集完整状态。两种内核地址不能混用。

若没有 START，应排查是否运行了正确文件或程序是否进入入口。ERROR 的十六进制阶段值：`0A` 文件系统接口无效；`14` 创建失败；`15` 写入不足；`16` 关闭失败；`17` 读回打开失败；`18` 读回不完整；`19` 内容不一致。五个报告名全被占用时显示 `FILES EXIST`，需先在电脑备份并移走旧报告。

## 构建与源码运行

在 `9288-debug` 目录执行。机端需要本地原版 SDK，以及已补 BBK R6～R9/R4 ABI 的 S1C33 LLVM：

```powershell
python -m pip install -r requirements-build.txt
python build_service.py --sdk D:\Downloads\app_env_9288 --toolchain ..\_tools\s1c33-9288
```

这会生成当前驻留版 `dist/9288Debug.exe` 和 `dist/service-build.json`；构建器会把堆重定位结果与另外两个地址的独立链接结果逐字节比较。`build.py` 用于重建探针和旧前台开发包，旧 dev 输出不再分发：

```powershell
python build.py --sdk D:\Downloads\app_env_9288 --toolchain ..\_tools\s1c33-9288
python -m unittest discover -s tests -p "test_*.py" -v
python -m host.gui
python -m host.client list
python -m host.client --port COM5 status
```

电脑端独立 EXE 可在 Windows 使用 PyInstaller 重建；它与机端 `.exe` 是两种文件。源码有更新时，旧的 `dist/9288Debug-PC.exe` 不会自动更新，启动脚本会优先运行这个打包文件：

```powershell
python -m PyInstaller --noconfirm --clean --onefile --windowed --name 9288Debug-PC --distpath dist --workpath build/pyinstaller --specpath build --paths . run_gui.py
```

`requirements-build.txt` 已包含 PyInstaller；电脑源码运行依赖 `requirements.txt` 中的 pyserial、Pillow，以及该 Python 安装自带的 Tcl/Tk。打包应在重建机端并更新验证记录后再运行 `python package.py`，以便交付清单包含本次实际文件的摘要。

原生内核模型的构建和回归入口为 `tools/build_service_model.py`、`tools/test_service_native_model.py`，需要工作区的私有固件、独立 NAND 副本和 QEMU。交付包不包含 SDK、原厂固件或 NAND；普通自动测试不会操作物理设备。

## 协议概要

`host/client.py` 与 `device/debug_core.c` 使用 12 字节 `<4sBBHHH>` 头、最多 512 字节负载及 IEEE CRC32。magic=`88DB`，版本 1，应答 opcode=`request|0x80`，序号一致，头最后一项为错误码。

|命令|用途|
|---|---|
|PING|身份 `BBK9288DEBUG/1 CAPTURED-0215EBC4 EXPERIMENTAL`|
|STATUS|image_id、能力、请求/错误数、快照及 flags|
|READ|分块读取允许的 SDRAM|
|KEY|排队一次原生 GUI DOWN/UP|
|SCREEN_BEGIN/READ/END|冻结、分块读取及释放灰度快照|
|SCRATCH/WRITE|查询或写入专属 512 字节测试区|
|STOP|回复完成后断开 CDC，等待软件断开间隔并恢复原生路径，USB 可保持供电|
|FILE submit/POLL/CANCEL|文件上传、下载和取消；能力位 `CAP_FILES=1<<6`，块数据最多 480 字节，完整布局见[文件协议](file-transfer.md)|

CRC、长度、序号或传输出错会使客户端会话失效，不自动重放输入和写入；多块事务共用同一锁，快照 token 在重置或断开时作废。flags bit0 表示旧前台模式，bit1 保留实验 USB 尚未全面验收标记（当前构建不会因本次实测自动清除此位），bit2 表示 uptime 不可用，bit3 表示仅原生 GUI 输入；bit2 置位时 `uptime_ms=0`。

USB 使用 CDC ACM、EP0=64 字节、EP1 IN、EP2 OUT、EP3 interrupt。`1209:9288` 为实验标识，未声称取得公共 VID/PID 分配。寄存器依据 [Epson S1C33L05 官方手册](https://www.epson.jp/prod/semicon/pdf/id000446.pdf) VI-2；静态证据见 [USB platform 说明](usb-platform.md)。
