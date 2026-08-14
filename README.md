# AudioPlaybackConnector
**English** | [简体中文](README.zh_CN.md)

Bluetooth audio playback (A2DP Sink) connector for Windows 10 2004+.

Microsoft added Bluetooth A2DP Sink to Windows 10 2004. However, a third-party app is required to manage connection.\
There is already an app can do this job. However it can't hide to notification area and it's not open-source.\
So I write this app, provide a simple, modern and open-source alternative.

# Preview
![Preview](https://cdn.jsdelivr.net/gh/ysc3839/AudioPlaybackConnector@master/AudioPlaybackConnector.gif)

# Usage
* Download the build matching your system architecture from [Releases](https://github.com/summerwind6/AudioPlaybackConnector-Win11/releases), then run AudioPlaybackConnector.
* Pair your phone in Windows Bluetooth settings. You can right-click the AudioPlaybackConnector notification-area icon and select "Bluetooth Settings".
* Click the AudioPlaybackConnector notification-area icon, select your phone in the device picker, and click "Connect".
* Enjoy!

# Troubleshooting
* If the device shows as connected but there is no audio, disconnect it and connect it again.
* If a connection error appears, or there is still no audio after the second connection, fully exit AudioPlaybackConnector, turn the PC's Bluetooth off and back on, then restart the app and connect the phone again. Removing and pairing the device again is usually unnecessary.
