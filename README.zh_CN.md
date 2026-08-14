# AudioPlaybackConnector Windows 11 优化版

[English](README.md) | **简体中文**

[![最新版本](https://img.shields.io/github/v/release/summerwind6/AudioPlaybackConnector-Win11?label=release)](https://github.com/summerwind6/AudioPlaybackConnector-Win11/releases/latest)
[![构建状态](https://img.shields.io/github/actions/workflow/status/summerwind6/AudioPlaybackConnector-Win11/build.yaml?branch=win11-compatibility&label=build)](https://github.com/summerwind6/AudioPlaybackConnector-Win11/actions/workflows/build.yaml)
[![开源协议](https://img.shields.io/github/license/summerwind6/AudioPlaybackConnector-Win11)](LICENSE)
[![Windows](https://img.shields.io/badge/Windows-10%202004%2B%20%7C%2011-0078D4?logo=windows11)](https://github.com/summerwind6/AudioPlaybackConnector-Win11/releases/latest)

让 Windows 10/11 电脑变成蓝牙音箱。本程序启用 Windows 内置的 **蓝牙 A2DP Sink（音频接收）**功能，使安卓手机或其他已配对蓝牙音频源的声音通过电脑播放。

> 本项目是 [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) 的 Windows 11 兼容性优化分支，原项目及基础功能版权归原作者所有。

## 功能特点

- 符合 Windows 11 风格的蓝牙设备选择窗口
- 通过通知区域快速连接或断开已配对手机
- 支持下次启动时自动重新连接
- 优化 Windows 11 下的 A2DP 连接生命周期
- 单文件绿色程序，无需安装
- 提供 x86、x64 和 ARM64 正式构建
- 支持简体中文、繁体中文和英文界面

## 系统要求

- Windows 10 2004（内部版本 19041）或更高版本，或者 Windows 11
- 可正常工作的蓝牙适配器及较新的蓝牙驱动
- 已经在 Windows 设置中完成配对的手机或其他蓝牙音频源

## 下载

请从 [GitHub Releases](https://github.com/summerwind6/AudioPlaybackConnector-Win11/releases/latest) 下载最新版本。

| 文件 | 系统架构 | 适用设备 |
| --- | --- | --- |
| `AudioPlaybackConnector64.exe` | x64 / AMD64 | 绝大多数 Intel 和 AMD 处理器电脑 |
| `AudioPlaybackConnector32.exe` | x86 | 32 位 Windows 系统 |
| `AudioPlaybackConnectorARM64.exe` | ARM64 | 骁龙等 Windows on ARM 设备 |

Release 页面同时提供 `SHA256SUMS.txt` 文件校验值。

## 使用方法

1. 在 **设置 → 蓝牙和设备** 中将手机与电脑完成配对。
2. 运行与电脑架构匹配的程序，程序图标会出现在系统通知区域。
3. 点击 AudioPlaybackConnector 图标。
4. 在设备列表中选择手机，然后点击“连接”。
5. 在手机上播放音频，声音将通过 Windows 当前的输出设备播放。

右键通知区域图标可以打开蓝牙设置或退出程序。退出时可以勾选“下次启动时自动连接”，程序会记住当前设备并在下次启动时尝试重新连接。

## 常见问题

### 显示已连接，但没有声音

先在 AudioPlaybackConnector 中断开手机，然后重新连接一次。

### 出现连接报错，或者第二次连接后仍然没有声音

1. 完全退出 AudioPlaybackConnector。
2. 关闭并重新开启电脑蓝牙。
3. 重新启动 AudioPlaybackConnector 并连接手机。

通常不需要删除设备或重新配对。

### 没有显示兼容设备

确认手机已经在 Windows 蓝牙设置中完成配对，然后关闭并重新打开设备选择窗口。

## 从源码构建

安装 Visual Studio 2022 或更高版本，并选择以下组件：

- 使用 C++ 的桌面开发
- MSVC v143 生成工具
- 对应架构的 C++ ATL
- Windows 11 SDK
- NuGet

恢复 NuGet 包后，使用 `Release` 配置为 `Win32`、`x64` 或 `ARM64` 构建 `AudioPlaybackConnector.sln`。

## Windows 11 优化内容

- 使用自定义 Windows 11 风格窗口替换旧版系统设备选择器
- 优化设备发现和连接状态刷新
- 改进 A2DP 连接清理及程序退出流程
- 添加 x86、x64 和 ARM64 自动化 Release 构建
- 增加更清晰的连接错误及恢复说明

## 技术说明

本程序使用 C++/WinRT 和 Windows `AudioPlaybackConnection` 接口。蓝牙编码协商及音频解码由 Windows 蓝牙协议栈处理，程序本身不能直接指定 SBC、AAC、aptX 或 LDAC。

## 已知限制

如果未勾选“下次启动时自动连接”，程序重新启动后的第一次手动连接偶尔可能显示已连接但没有声音。断开后重新连接一次通常可以恢复；如果仍未恢复，请按照上面的步骤重新启动电脑蓝牙。

## 开源协议与致谢

本项目采用 [MIT License](LICENSE) 开源。

- 原始项目：[ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector)
- Windows 11 优化分支：[summerwind6/AudioPlaybackConnector-Win11](https://github.com/summerwind6/AudioPlaybackConnector-Win11)
