# VideoBridge —— 图传上屏一键开关

采集卡 → 硬件编码 → RTSP/WebRTC → Quest 浏览器，附带 `adb reverse`。
**只在 start 期间占用资源，stop 之后没有任何常驻进程。**

---

## 1. 目录结构

```
tools/videobridge/
├─ videobridge.ps1   主控（start / stop / status / toggle / restart / doctor / logs）
├─ config.psd1       全部可调参数（改这里，不改脚本）
├─ bridge.cmd        双击入口（无参 = 交互菜单，带参 = 直接执行）
└─ logs/             运行日志（%LOCALAPPDATA%\videobridge\logs）
```

## 2. 用法

```bat
bridge.cmd                 :: 交互菜单
bridge.cmd start           :: 启动
bridge.cmd stop            :: 停止
bridge.cmd toggle          :: 一键开关（同一个快捷键绑两次也行）
bridge.cmd status
bridge.cmd doctor          :: 自检：ffmpeg / NVENC / 采集卡 / MediaMTX / 端口 / adb / C5
bridge.cmd logs
```

PowerShell 直接调：`.\videobridge.ps1 start`。

## 3. 依赖

| 组件 | 放哪 | 说明 |
|---|---|---|
| ffmpeg | PATH 里，或 `Video.FfmpegPath` | 本机已装（`C:\Users\...\ffmpeg.exe`），NVENC 可用 |
| MediaMTX | `Relay.ExePath` | 单文件程序，官网下 zip 解压即可 |
| adb | PATH 里，或 `Link.AdbPath` | Android Platform-Tools |
| node | `Node.ExePath` | 只有要跑 ControlAnything 时才需要 |

## 4. 配置文件要点（config.psd1）

```powershell
Video.DeviceName = 'UGREEN 95348'   # 本机实测的采集卡名，doctor 会列出所有候选
Video.Size       = '1920x1080'      # 吃力就改 '1280x720'
Video.Bitrate    = '8M'
Video.Encoder    = 'auto'           # auto 会优先 NVENC，没有就退回 x264
Relay.RtspPort   = 8554
Relay.HttpPort   = 8889             # WHEP: http://127.0.0.1:8889/cam
Link.ReversePorts= @(8080, 8889)    # 映射进头显的 localhost
Link.C5Ip        = '192.168.4.1'    # 只用于 doctor 连通性检查
Node.Enabled     = $false           # 需要 ControlAnything 时改 $true
```

## 5. 每次 start 都做了什么

```
1. adb reverse tcp:8080 / tcp:8889   ← 头显的 localhost 指回 PC
2. 启 MediaMTX（RTSP:8554 / WHEP:8889）
3. 启 ffmpeg（采集卡 → NVENC → rtsp://127.0.0.1:8554/cam）
4. （可选）启 Node
5. 把 PID 写入 %LOCALAPPDATA%\videobridge\state.json
```

`stop` 按 state.json 逐个 `taskkill /T /F`，再撤销 `adb reverse`，清状态文件。
如果 state.json 丢了，`stop` 还会兜底清理残留的 `ffmpeg` / `mediamtx` 进程。

## 6. 头显侧

```
浏览器打开  http://127.0.0.1:8080         ← ControlAnything 页面（WebXR 可用，localhost 是安全上下文）
视频地址    http://127.0.0.1:8889/cam     ← MediaMTX 自带播放页，先验证再嵌进页面
```

## 7. 为什么这样封装

| 设计 | 理由 |
|---|---|
| 脚本不常驻、执行完就退出 | 不占 CPU/内存；没有后台服务要维护 |
| 参数全在 config.psd1 | 改分辨率/码率/端口不用碰代码，升级脚本不会覆盖配置 |
| 全部走 PID + taskkill /T | 能连带杀掉 ffmpeg 的子进程，不留孤儿 |
| doctor 独立命令 | 出问题先跑它，一次看全所有前置条件 |
| 失败自动回滚 | start 中途失败会 stop 已起的部件 + 撤销 adb reverse |

## 8. 常见故障

| 现象 | 原因 | 处理 |
|---|---|---|
| `采集设备` FAIL | 设备名不对/没插 | 跑 `doctor` 看列出的名字，抄进 config |
| ffmpeg 起来又立刻退出 | 分辨率/帧率设备不支持 | 降到 720p60；看 `logs\ffmpeg.log` |
| WHEP 页面转圈 | MediaMTX 没起或端口被占 | `status` 看端口；`logs` 看 mediamtx.log |
| 头显打不开页面 | adb reverse 没建立 | `status`；手动 `adb reverse tcp:8080 tcp:8080` |
| 画面卡顿 | NVENC 未启用 / WiFi 差 | `doctor` 看 NVENC；降码率到 4M |

## 9. 固化：编成 exe

```powershell
Install-Module ps2exe -Scope CurrentUser
Invoke-PS2EXE .\videobridge.ps1 .\VideoBridge.exe -noConsole:$false
```

exe 与 `config.psd1` 放同一目录即可（脚本按 `$PSScriptRoot` 找配置）。
省事的话直接用 `bridge.cmd`，效果一样。
