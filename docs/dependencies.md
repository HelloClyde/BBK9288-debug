# 构建依赖与来源

9288 Debug 面向原版 9288 的 S1C33 下载程序，产物是 **KF2 格式的设备 EXE**。
它不是 9588/H1 的 BDA，也不使用这些机型的 SDK。当前服务只接受 README 中列出的
`CAPTURED-0215EBC4` 固件指纹；SDK 中存在声明不代表其他固件支持相同接口。

## 固定构建输入

公开构建从已发布的 GAMForge Full 包提取工具链和原版 9288 SDK 头文件，
先核验整个 ZIP 的 SHA-256，再使用仓库中的发布构建入口。该包只是构建输入，
本项目的源码包和安装包不携带它，也不分发其中的 SDK、固件、工具链或原厂打包器。

| 项目 | 固定值 |
| --- | --- |
| 来源版本 | [HelloClyde/gamforge v1.5.4](https://github.com/HelloClyde/gamforge/releases/tag/v1.5.4) |
| 完整文件 | `gamforge-1.5.4-full-windows-x64.zip` |
| 下载地址 | [固定 Windows Full ZIP](https://github.com/HelloClyde/gamforge/releases/download/v1.5.4/gamforge-1.5.4-full-windows-x64.zip) |
| ZIP SHA-256 | `1c51ae2c416bfef265c64a4d0e86f29dfa0dfe5a985a9fe05719787ed730206d` |
| 该发布源码提交 | `60cd999bc5f40d7bfea1d7a24667b3c524dc505d` |
| 头文件目录 | ZIP 内 `GAMForge/dependencies/sdk9288/Down_Include/` |
| 工具链目录 | ZIP 内 `GAMForge/dependencies/toolchain/` |

SDK 来自钳工提供的原版 9288 开发环境。其原始材料没有给出可用于本项目的开源
许可证或独立 Git 提交，因此这里不为它虚构开源许可或子模块提交。
[GAMForge 的来源说明](https://github.com/HelloClyde/gamforge/blob/v1.5.4/THIRD_PARTY_NOTICES.md)
记录维护者已确认该依赖包中 SDK 和固件的分发授权；这不是将它们重新许可为 GPL。
本项目仅把其中的必要头文件作为外部构建输入。构建器不执行原厂打包器，
不提取 ROM，并使用本项目生成的图标。

也可以按发布构建入口的参数，使用自己已有的相同 SDK 和工具链；输入不同可能产生
不同结果，不能据此沿用某次发布二进制的验证标记。旧实验脚本保留原有参数以保持
已验证服务映像的源码身份，正常构建应使用 README 的仓库相对路径入口。

## S1C33 工具链

| 项目 | 固定值 |
| --- | --- |
| 上游 | [autch/llvm-s1c33](https://github.com/autch/llvm-s1c33) |
| 上游提交 | `f6e7f2d31f56b80e74f116f96f6fac5ffb126cf7` |
| 目标 | `s1c33-none-elf` |
| ABI | `9288-gnu33-r6-r9-return-r4-reserved-r15` |
| 补丁文件 | 工具链目录中的 `9288-gnu33-abi.patch` |
| 补丁 SHA-256 | `b00c2fedcb3c28c56db239db957422d4facdbe7ae121455abee2667c39d2ab40` |
| 必需工具 | `clang`、`ld.lld`、`llvm-objcopy`、`llvm-readelf` |

LLVM 保持 **Apache-2.0 WITH LLVM-exception**。来源包中的
`LLVM-LICENSE.TXT`、`manifest.json` 和 ABI 补丁属于工具链的来源记录，不能把 LLVM
改写成项目的 GPL 许可。构建入口还会实际编译 ABI 探针，检查第 1/4 个参数使用
R6/R9、返回值使用 R4，而不是只凭文件名接受任意 S1C33 编译器。

Windows 预编译工具链的 Microsoft VC++ 运行库仅随该外部构建依赖使用，
不被本项目重新许可或复制进设备安装包。工具链来源包保留其独立分发记录。

## 电脑客户端与打包

源码客户端需要 Python、pyserial、Pillow 及该 Python 提供的 Tcl/Tk。
独立 Windows 客户端已冻结这些运行依赖，使用者无需另装 Python。
构建依赖范围见根目录的 `requirements.txt` 与 `requirements-build.txt`；
下表是本机本轮核验的实际版本，CI/发布包中的组件记录以其实际构建清单为准。

| 组件 | 本地核验版本 | 来源及许可 |
| --- | --- | --- |
| Python | 3.11.5 | [CPython](https://www.python.org/)，PSF 许可及其随附声明 |
| pyserial | 3.5 | [pyserial](https://github.com/pyserial/pyserial/tree/v3.5)，BSD-3-Clause，Chris Liechti 等贡献者 |
| Pillow | 12.3.0 | [Pillow](https://github.com/python-pillow/Pillow)，MIT-CMU，含各捆绑组件的单独声明 |
| Tcl/Tk | 8.6.12 | [Tcl/Tk](https://www.tcl-lang.org/)，保留各自的 `license.terms` |
| pyelftools | 0.33 | [pyelftools](https://github.com/eliben/pyelftools)，公共领域/Unlicense；内含 construct 保持自身许可 |
| PyInstaller | 6.16.0 | [PyInstaller](https://github.com/pyinstaller/pyinstaller)，GPL-2.0-or-later 加 bootloader 分发例外 |

pyelftools 只用于解析编译结果；PyInstaller 只用于电脑端打包。
发布包应保留实际冻结组件的许可证，不把第三方运行库一并称作本项目的原创代码。

## 未随仓库分发的验证输入

原生内核模型与部分历史探测脚本需要用户自备的固件、启动镜像、专用 NAND 副本和
匹配 QEMU。这些输入不属于公开构建所必需的依赖，也不进入仓库或 Release。
公开 CI 的主机测试、控制器模型和编译检查不应被写成完整固件模拟器或真机验收。
历史真机证据按服务映像摘要绑定，详见项目验证说明；截图只展示实际观察到的界面。
