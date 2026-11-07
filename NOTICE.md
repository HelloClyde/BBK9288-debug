# 来源与许可

本项目自行编写的 9288 调试服务、USB CDC 实现、电脑客户端、协议、测试和构建包装脚本，按 **GPL-3.0-only** 发布，完整许可见 `LICENSE`。维护仓库为 HelloClyde/BBK9288-debug。

- 功能设计参照 [H1Debug](https://github.com/HelloClyde/BBKH1-debug)，其独立开源版本也采用 GPL-3.0-only。9288 的固件接口、处理器、USB 控制器与 H1 不同，不能互换二进制程序。
- 固件函数表地址和少量函数指纹用于匹配受支持设备及拒绝不匹配的固件。仓库不包含原厂固件、NAND、用户存档或完整内核转储。
- Epson S1C33L05 的寄存器说明依据厂商手册，相关链接保留在技术文档中；不重新分发手册。
- 原版 BBK 9288 SDK 头文件只作为外部构建依赖。SDK 没有在本仓库中附带开放源码许可，本项目的 GPL 不适用于 SDK 或固件。构建固定到现有公开 GAMForge 依赖包，只提取所需头文件与工具链；不把 SDK、原生打包器、ROM 或原始图标放入本仓库或调试器 Release。
- Release 使用可由构建脚本再生的本项目图标，按本项目 GPL-3.0-only 发布。
- S1C33 编译器来自 autch/llvm-s1c33，使用 GAMForge 已验证的 GNU33 ABI 补丁；其 LLVM 等上游许可单独适用。具体版本、来源与 SHA-256 见 `docs/dependencies.md`。
- Windows 客户端包含 CPython、Tcl/Tk、pyserial、Pillow 和 PyInstaller 运行时。它们各自的许可随安装包 `licenses/` 分发；PyInstaller 的 bootloader 例外不把其许可强加到应用代码上。

`docs/validation.md` 区分历史真机验证、主机模型测试与本次构建验证。除明确记录的部分外，不将编译通过描述为真机通过。
