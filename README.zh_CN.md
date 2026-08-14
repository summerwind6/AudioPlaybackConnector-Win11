# AudioPlaybackConnector
[English](README.md) | **简体中文**

Windows 10 2004+ 蓝牙音频接收 (A2DP Sink) 连接工具。

微软在 Windows 10 2004 加入了蓝牙 A2DP Sink 支持。但是需要第三方软件来管理连接。\
已经有了一个可以实现此功能的 app。但是它不可以隐藏到通知区域，而且不开源。\
所以我写了这个 app，提供一个简单的，现代且开源的替代品。

# 预览
![预览](https://cdn.jsdelivr.net/gh/ysc3839/AudioPlaybackConnector@master/AudioPlaybackConnector.gif)

# 使用方法
* 从 [Releases](https://github.com/summerwind6/AudioPlaybackConnector-Win11/releases) 下载与系统架构匹配的版本并运行 AudioPlaybackConnector。
* 在 Windows 蓝牙设置中与手机完成配对。你可以右键点击通知区域的 AudioPlaybackConnector 图标，然后选择“蓝牙设置”。
* 点击通知区域的 AudioPlaybackConnector 图标，在设备选择窗口中选择手机并点击“连接”。
* 尽情享受吧！

# 常见问题
* 如果设备显示已连接但没有声音，请先断开连接，然后重新连接一次。
* 如果出现连接报错，或者第二次连接后仍然没有声音，请完全退出 AudioPlaybackConnector，关闭并重新开启电脑蓝牙，然后重新启动程序并连接手机。通常不需要删除设备或重新配对。
