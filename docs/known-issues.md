# 已知问题与踩坑记录

按“后果严重程度”排序，每条都附现象与判据。

---

## 系统级限制

### 1. Quest 3 不允许第三方 App 读取 UVC 采集卡

**现象**：采集卡插头显后，系统弹「选择用哪个 App 打开视频设备」，选自己的 App 也拿不到画面。

**日志证据**（`app.log`）：

```
[UsbProbe] 设备 vid=0x2b89 pid=0x5348 class=239 ifCount=8   ← USB 层认到了
[UsbProbe] 权限: 无，申请中
[UsbProbe] 权限回调: false                                   ← 系统不给权限
[App] Camera 0 1920x1080@30 -> 无帧                          ← 相机栈也零帧
[App] Camera 0 0x0@0 -> 无帧                                 ← 六种模式全失败
```

同一块卡插 Windows 立刻变成 `Camera: UGREEN 95348`，说明**卡没问题，是 Horizon OS 不允许**。

**试过无效的手段**：

- `device_filter` + `USB_DEVICE_ATTACHED`（AAR 形式）
- `UsbManager.requestPermission()`（无弹窗，直接回调 false）
- `WebCamTexture` 全模式枚举

**结论**：只能走 PC 中转。

---

## 嵌入式

### 2. SPI 同步字节写错导致永久收 0 帧

magic `0xA55A` 小端首字节是 **`0x5A`**。曾“修正”成 `0xA5`，结果 F411 一直收 0 帧且无任何报错。

判据：`bridge: F411 ... 收 0 帧`。

### 3. SPI 从机不能用 HAL DMA

F411 作 SPI 从机时 HAL DMA 时序不可靠，必须裸寄存器 + 逐字节中断；ISR 里**先补 DR 再处理缓冲**，否则会卡死。

### 4. 传输长度必须多 1 字节

C5 侧 `.length = 8 * 31`（31 字节），因为从机帧首会多一个残留字节。解析按偏移找 magic，不能假设偏移 0。

### 5. C5 堆查询会 panic

`heap_caps_get_largest_free_block()` 会遍历 tlsf 空闲链表，实测触发 `Load access fault`。改用 `heap_caps_get_free_size()`。

### 6. C5 WiFi 频段顺序

必须 `start() → apply_band_mode() → set_config()`，否则报 `Channel 6 (2G) is not supported (5G_only)`。

### 7. 限位校准“瞬间完成”

堵转判定阈值设成 5 个采样（100 ms）时，电机还没转起来就被判为“到限位”，表现为**校准一下都没动**。

修法：

- 阈值放宽到 25 个采样（500 ms）
- 必须**先下发过位移**才统计堵转
- 校准结束再退到 18° 正角，否则停在限位旁边

### 8. 云台跨 ±180° 绕整圈

直接用绝对 yaw 当目标角，头转到 179°→-179° 时目标突变 360°，电机就转一整圈。

修法：固件里维护**连续累计角**

```c
s_yaw_cont += wrap180(raw - s_yaw_prev);
s_yaw_prev = raw;
y = s_yaw_cont;
```

### 9. 读 C5 串口会复位它

打开 COM 口触发 DTR/RTS 跳变 → ESP32 复位 → BLE 断开。测试时不要同时读串口。

---

## Unity / Quest App

### 10. OpenXR 默认特性把应用卡在加载

Meta 包默认勾上 `MockRuntime` + 一堆 AR 模块（Plane/Mesh/Anchor/Raycast/Occlusion），运行时报 `XR_ERROR_FUNCTION_UNSUPPORTED`，表现为**一直转加载圈**。

只保留：

```
✅ Meta Quest Support
✅ Meta Quest: Camera (Passthrough)
✅ Meta Quest: Session
✅ XR Performance Settings
```

### 11. XR loader 不自动启动

`XRGeneralSettingsPerBuildTarget.asset` 里 `m_AutomaticLoading / m_AutomaticRunning` 为 0 时，loader 不会加载，表现为**没有 XR 会话、头姿恒为单位值**（`pose camera`、`yaw -180` 恒定）。

### 12. Android 蓝牙权限必须运行时申请

自定义清单里没有 `BLUETOOTH_SCAN / BLUETOOTH_CONNECT` 时，`requestPermissions()` **静默失败**，连弹窗都不会出现。

### 13. Unity 6 不再支持 `Assets/Plugins/Android/res`

直接报：

```
OBSOLETE - Providing Android resources in Assets/Plugins/Android/res was removed
```

必须打成 AAR。仓库里的 `tools/make_usbfilter_aar.py` 就是干这个的（注意 `classes.jar` 不能是 0 字节，否则 Gradle 报 `Could not find EOCD`）。

### 14. 面板文字不显示 / 镜像

- `TextMesh` 运行时创建没有字体材质 → 只有方块没有字
- UGUI `Text` 受 Canvas / RectTransform（默认 100×100 裁切）影响

最终方案：**自己把字画进 Texture2D**（点阵字，见 `BitmapFont.cs`），并加 `flipy` 开关修正上下翻转。

### 15. 图传每帧拷贝把主线程拖死

每帧 `GetPixels32()`（720p 3.6 MB）+ `SetPixels32()`（2 MB）+ 背板镜像（2 MB），60 fps 就是 340 MB/s 的主线程搬运，表现为**整体卡顿、帧率上不去**。

修法：图传单独用一块 Quad，`material.mainTexture = texture` 零拷贝显示。

### 16. UnityWebRequest 在 Quest 上只回调一次

`DownloadHandlerScript` 收 MJPEG 时只回调一两次就停（`bytes` 卡住不动），但同一台机器用 curl 拉是连续的。

修法：自建 `TcpClient` 后台线程读流、解析 JPEG 边界，主线程 `LoadImage`（`MjpegTcp.cs`）。

### 17. `Pump()` 忘了在主循环调用

后台线程解析出的帧进了队列，但主线程从没取——`bytes` 一直涨，`frames` 恒 0。

---

## 图传（PC 侧）

### 18. 采集卡只认 YUYV

这块 UGREEN 95348 用 `-vcodec mjpeg` 抓会报：

```
No JPEG data found in image
```

必须：

```powershell
ffmpeg -f dshow -vcodec rawvideo -pix_fmt yuyv422 -video_size 1920x1080 -framerate 60 ...
```

### 19. 无信号时 ffmpeg 只吐一帧

HDMI 没信号时 ffmpeg 会输出一帧然后卡住，客户端看到的是“画面冻住”。`devserver.py` 已加 `NO SIGNAL` 占位图（超过 2 秒无新帧就发）。

### 20. 端口和设备被占用

- 采集卡同时只能被一个进程打开（OBS / ffmpeg 互斥）
- `adb reverse` 在重装 APK、拔插 USB 后经常失效，需要重跑
