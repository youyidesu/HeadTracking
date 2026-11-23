# Quest3 头追数据提取 —— Meta/OpenXR 工程侧分析

> 目标：从运行在 Quest3 上的 Meta XR（Unity）工程里，稳定取出**头部姿态**并送到
> F411（经 ESP32-C5）。本文分析"数据从哪来、怎么取才准、哪些坑会毁掉闭环"。

---

## 1. 姿态数据在 Meta 工程里的三个来源

| # | 来源 | 拿到什么 | 精度/特性 | 适用 |
|---|---|---|---|---|
| A | `Camera.main.transform` / `OVRCameraRig.centerEyeAnchor` | Unity 世界位姿 | 简单，但**已被预测到显示时刻**；`localRotation` 受父级影响 | 快速验证 |
| B | `UnityEngine.XR.InputDevices` / `XRNodeState` | tracking space 位姿 + `tracked` 标志 | 跨 SDK，OpenXR 通用；无时间戳、无角速度 | 通用实现 |
| C | **`OVRPlugin.GetNodePose(node, Step.Render)` + `GetNodeVelocity`** | 位姿 + 角速度 + 可切 `Step` | **最精确**，能拿速度做前馈 | **推荐** |

`Step` 的三档：`Render`（渲染帧用，默认）、`Physics`、`LateLatch`（最贴近显示）。
取控制用的姿态用 `Render`；要"最不滞后"可用 `LateLatch`。

## 2. 推荐实现（Meta XR SDK / OVRPlugin）

```csharp
using OVRPlugin;

// 在 XR 提交渲染前采样，拿到的就是这一帧的预测显示姿态
void OnEnable()  => Application.onBeforeRender += SampleHead;
void OnDisable() => Application.onBeforeRender -= SampleHead;

void SampleHead()
{
    if (!OVRPlugin.GetNodePose(OVRPlugin.Node.EyeCenter, OVRPlugin.Step.Render,
                               out var pose))
    {
        flags &= unchecked((byte)~F_VALID);      // 追踪丢失：明确告诉下游
        return;
    }

    // OVRPlugin 是右手系（Y up / -Z forward），Unity 是左手系 —— 必须转
    Quaternion q = pose.Orientation.ToQuat();
    Vector3    p = pose.Position.FromFlippedZVector3f();

    // 角速度（rad/s），比差分稳得多
    Vector3 angVel = Vector3.zero;
    if (OVRPlugin.GetNodeVelocity(OVRPlugin.Node.EyeCenter, OVRPlugin.Step.Render, out var v))
        angVel = v.AngularVelocity.FromFlippedZVector3f();   // rad/s

    // 相对零点（Recenter）
    Quaternion rel = Quaternion.Inverse(_zeroRot) * q;       // _zeroRot 用**世界**旋转
    Vector3 fwd = rel * Vector3.forward;

    float yaw   = Mathf.Atan2(fwd.x, fwd.z) * Mathf.Rad2Deg; // 右正
    float pitch = -Mathf.Asin(Mathf.Clamp(fwd.y, -1f, 1f)) * Mathf.Rad2Deg;

    // 时间戳：用引擎单调时钟，别用 DateTime.UtcNow（会跳变）
    double t = OVRPlugin.GetTimeInSeconds();                 // 预测显示时刻（秒）
    uint tUs = (uint)((t - _t0) * 1e6);
    ...
}
```

## 3. 十个会毁掉闭环的坑

| # | 坑 | 后果 | 处理 |
|---|---|---|---|
| 1 | **姿态是"预测到显示时刻"的** | 云台按它追会**超前 20~40 ms**，甩头过冲/回摆 | 用 `yawRate × 预测提前量` 反推回"当前"；或云台侧按收到的 tUs 做延迟对齐 |
| 2 | 用 `localRotation` 做 Recenter | 相机有父级旋转时零点歪 | 一律用 `worldRotation` |
| 3 | 频率定 **90 Hz 稳定全帧** | Quest 显示率 90 Hz，帧间隔 11.1 ms，带宽 2.7 kB/s 只占 SPI 0.04% | 每帧都发，不做抽帧；下游按 `tUs` 算 age + `yaw_rate` 外推 |
| 4 | 时间戳用 `DateTime.UtcNow` | 系统时间可跳、跨线程不一致 | `OVRPlugin.GetTimeInSeconds()` 或 `Time.realtimeSinceStartupAsDouble` |
| 5 | 角速度用相邻帧差分 | 重复帧时 dt→0，速率爆跳 | 直接读 `GetNodeVelocity` |
| 6 | 不检查 `tracked` | 遮挡/出界时仍发 `F_VALID`，云台追幽灵 | 位姿取不到就清 `F_VALID`，下游进降级 |
| 7 | `StructToBytes` 每帧分配 | GC 尖峰造成掉帧、发包抖动 | 预分配 `byte[]` + `Marshal.StructureToPtr` 复用 |
| 8 | WiFi 省电策略 | 延迟从 3 ms 抖到 50 ms | `WifiLock(WIFI_MODE_FULL_HIGH_PERF)` + 关省电（已有） |
| 9 | 坐标系数值约定不一致 | yaw 反向/偏 90°，云台朝反方向跑 | Unity 左手 Y-up、Z 前；协议 `yaw=atan2(x,z)`、`pitch=-asin(y)`；**上机先验一次符号** |
| 10 | 系统级 Recenter（长按 Oculus 键） | 零点被系统改掉，云台突然跳 | 监听 `OVRManager.recenterPose` 或对比 `XRNodeState`，必要时重新校准并置 `F_RECENTER` |

## 4. 端到端延迟预算（90 Hz 环）

| 环节 | 典型值 | 备注 |
|---|---|---|
| Quest 追踪→姿态可用 | 15~25 ms | 含预测 |
| 发包 + WiFi（5G，同 AP） | 2~8 ms | 抖动主要来源 |
| **帧间隔（90 Hz）** | **11.1 ms** | 平均滞后 5.6 ms，外推后残余 0.12~0.31° |
| ESP32-C5 → F411（SPI @5 MHz） | **0.01 ms 量级** | 30 B 一次搬完 |
| F411 解析 + 控制环 | 1 ms | 1 kHz |
| ZDT 驱动器电流环 | 1~2 ms | 闭环 FOC |
| **合计** | **20~36 ms** | 人眼对 20 ms 内不敏感 |

> 用 `yawRate` 做前馈（`target += yawRate × T_delay`）能把等效延迟压到 10 ms 以内，
> 这就是协议里保留 `yaw_rate/pitch_rate` 两个字段的原因。

## 4.5 自适应帧率：慢 36 Hz / 快 72 Hz

固定砍到 36 Hz 在慢速时够用，但快速甩头时 27.8 ms 的帧间隔会让目标呈台阶状。
所以加一层判定，只在**需要的时候**才用满 72 Hz：

```
进入快模式： max(|yaw_rate|, |pitch_rate|) > 120 °/s          （立即）
退出快模式： 该值 < 80 °/s 且持续 400 ms                      （迟滞）
最短快模式驻留：200 ms                                        （防止抖）
快速时：每帧发（72 Hz）    慢速时：隔帧发（36 Hz）
```

**为什么必须用迟滞而不是单阈值**：36 Hz 下角速度是 28 ms 差分出来的，
在 120 °/s 附近本身就有 ±30 °/s 的量化抖动；单阈值会让模式在每帧之间来回切，
发包率忽 36 忽 72，反而比固定频率更难预测。进入 120 / 退出 80 的 40 °/s 迟滞带
刚好覆盖这个抖动幅度。

**代价**：快模式 30 B × 72 Hz = 2.2 kB/s，SPI @5 MHz 占 0.04%——可以忽略。

**F411 侧不需要任何改动**：它按 `tUs` 算 age、按 `yaw_rate` 外推，
帧率变化会自动适应；`Esp_AgeMs()` 的 150 ms 超时对两种模式都有 5 倍余量。

```csharp
// HeadPoseSender.cs 内
void UpdateFastMode(float dps) {
    float now = Time.realtimeSinceStartup;
    if (!_fast) {
        if (dps > enterFastDps) { _fast = true; _fastUntil = now + minFastSec; _slowSince = now; }
    } else {
        if (dps > exitFastDps) _slowSince = now;
        else if (now > _fastUntil && (now - _slowSince) >= exitHoldSec) _fast = false;
    }
}
// 发送前
_frameNo++;
if (adaptiveDecimate && !_fast && (_frameNo & 1u) != 0u) return;
```

### 4.6 帧率对性能的影响（量化）

帧率真正影响的**不是带宽**（30 B/帧的流量可以忽略），而是"帧间隔造成的量化误差"：

| 速率 | 帧间隔 | 平均滞后* | 外推残余 @2000 °/s² | @5000 °/s² | 带宽 | SPI 占用 |
|---|---|---|---|---|---|---|
| 36 Hz | 27.8 ms | 13.9 ms | 0.77° | 1.93° | 1.1 kB/s | 0.02% |
| 60 Hz | 16.7 ms | 8.3 ms | 0.28° | 0.69° | 1.8 kB/s | 0.02% |
| 72 Hz | 13.9 ms | 6.9 ms | 0.19° | 0.48° | 2.2 kB/s | 0.04% |
| 90 Hz | 11.1 ms | 5.6 ms | 0.12° | 0.31° | 2.7 kB/s | 0.04% |
| **120 Hz** | **8.3 ms** | **4.2 ms** | **0.07°** | **0.17°** | **3.6 kB/s** | **0.07%** |

\* 平均滞后 = 帧间隔/2，一阶外推（`+yaw_rate·dt`）能把这一项抵掉大半。
**残余误差 ∝ α·τ²/2**，所以帧率翻倍、误差降到 1/4。

**结论：链路完全不是瓶颈**，120 Hz 只要 3.6 kB/s、SPI 占 0.07%。
真正的约束在 Quest 端——120 Hz 的渲染预算只有 **8.3 ms/帧**（72 Hz 是 13.9 ms）。
如果工程里还叠了 UVC 采集卡画面（`UvcCameraFeed.cs`），8.3 ms 可能撑不住，
掉帧造成的抖动比降帧率更糟。

**推荐**：先量 Quest 端的实际帧时间（`OVRPlugin.GetAppFrametime` 或 `adb shell dumpsys gfxinfo`）
- 能稳定 <8 ms → **锁 120 Hz**，隔帧 60 Hz、快速满 120 Hz
- 只能到 11 ms → 锁 90 Hz（误差 0.12~0.31°，仍然够用）
- 只能到 13 ms 以上 → 说明场景/采集卡是瓶颈，先解决它，别指望链路省出来

## 5. 与现有 F411 侧的对接

- 帧：`qp_head_t` 30 B（magic 0xA55A / ver / flags / seq / tUs / 四元数 / 角度 / 角速度 / CRC16）
- 传输：C5 → F411 走 **SPI1 从机**（`BSP/Src/esp_link.c` 已实现 CRC + 滑窗同步）
- 标志位：`F_VALID`（姿态可信）、`F_CALIB`（校准中，云台冻结）、
  `F_RECENTER`（回中并重设零点）、`F_ESTOP`（立即停）
- 下游：`Esp_GetHead()` 取出最新帧；`Esp_AgeMs() > 150` 判链路超时 → 进入降级

## 6. 验证清单

1. **静态**：头不动 10 s，`yaw/pitch` 抖动应 <0.05°（>0.2° 说明噪声/重复帧问题）
2. **频率**：`adb logcat` 打点，实际发包率应 ≈ 显示刷新率（72/90/120），**不是 200**
3. **连续性**：抓 UDP（Wireshark）看 `seq` 连续、无重复姿态
4. **符号**：头向右转（+yaw）→ 云台应同向；反了先改协议符号，别在驱动器里改
5. **延迟**：匀速甩头（2 Hz），对比"画面里云的移动"与"炮塔实际转动"，应同相
6. **丢追踪**：手挡住头显镜头 2 s，云台应冻结在最后位置并置 `F_VALID=0`，恢复后无跳变

## 7. 现有 `HeadPoseSender.cs` 的差距（逐条）

| 现状 | 问题 | 改法 |
|---|---|---|
| `Camera.main.transform.localRotation` | 已被预测 + 父级污染 | 改 `OVRPlugin.GetNodePose` + `worldRotation` |
| `rateHz=200` 累积器 | 实际 ≤120 Hz 且发重复帧 | 按帧发，`rateHz` 改成"上限"语义 |
| `tUs = DateTime.UtcNow` | 跳变/非单调 | `OVRPlugin.GetTimeInSeconds()` |
| 差分算 `yawRate` | 重复帧时爆跳 | `GetNodeVelocity` |
| 无 tracked 检查 | 追幽灵 | 位姿失败清 `F_VALID` |
| `StructToBytes` 每帧 new | GC 掉帧 | 复用缓冲 |
| `Crc16` 里又 StructToBytes 一次 | 每帧两次分配 | 先序列化到复用缓冲，再算 CRC |
