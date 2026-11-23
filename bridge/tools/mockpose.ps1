#Requires -Version 5.1
<#
    mockpose.ps1 —— 假头姿源：不接头显、不接 ControlAnything，直接把 16 字节 pose 帧
                    发到指定地址，用来单独调试 C5 / F411 这一段。

    帧格式（与 quest/bridge.js、F411 的 esp_link 一致，小端）：
        0  u16 magic    0xA55A
        2  u16 seq
        4  i16 yaw_cdeg      (deg*100)
        6  i16 pitch_cdeg
        8  i16 yaw_rate      (deg/s*10)
       10  i16 pitch_rate
       12 u16 flags         0x01 VALID / 0x04 RECENTER / 0x08 ESTOP
       14 u16 crc16         前 14 字节，CCITT 0x1021 init 0xFFFF

    用法：
        .\mockpose.ps1                      # 默认发 127.0.0.1:5005，正弦摆动
        .\mockpose.ps1 -Target 192.168.4.1  # 发到 C5
        .\mockpose.ps1 -Still               # 静止 0°（验零漂）
        .\mockpose.ps1 -Step 30             # 每 2 秒 ±30° 阶跃（验响应/方向）
        .\mockpose.ps1 -Rate 90 -Seconds 30
        .\mockpose.ps1 -EStop               # 带急停标志（验保护）
#>
[CmdletBinding()]
param(
    [string]$Target = '127.0.0.1',
    [int]$Port = 5005,
    [int]$Rate = 90,               # 每秒帧数
    [int]$Seconds = 0,             # 0 = 一直跑，Ctrl+C 结束
    [double]$Amp = 60.0,           # 正弦幅度（度）
    [double]$Freq = 0.25,          # 正弦频率（Hz）
    [switch]$Still,                # 完全静止
    [double]$Step = 0,             # >0 = 阶跃模式，每 2 秒切换 ±Step
    [switch]$EStop,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

# ── CRC16/CCITT ────────────────────────────────────────────────────────────
function Get-Crc16([byte[]]$buf) {
    $crc = 0xFFFF
    foreach ($b in $buf) {
        $crc = $crc -bxor ($b -shl 8)
        for ($i = 0; $i -lt 8; $i++) {
            if ($crc -band 0x8000) { $crc = (($crc -shl 1) -bxor 0x1021) -band 0xFFFF }
            else { $crc = ($crc -shl 1) -band 0xFFFF }
        }
    }
    return [uint16]$crc
}

# ── UDP 端点 ───────────────────────────────────────────────────────────────
$udp = New-Object System.Net.Sockets.UdpClient
$ep  = New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Parse($Target)), $Port
$udp.Client.SendTimeout = 200

Write-Host ''
Write-Host "── mockpose ─────────────────────────────" -ForegroundColor Cyan
Write-Host "  目标      $Target`:$Port"
Write-Host "  速率      $Rate Hz"
if ($Still)  { Write-Host '  模式      静止 0°' }
elseif ($Step -gt 0) { Write-Host "  模式      阶跃 ±$Step°（每 2 s 切换）" }
else { Write-Host ("  模式      正弦 ±{0}° @ {1} Hz" -f $Amp, $Freq) }
if ($EStop) { Write-Host '  标志      含 ESTOP' -ForegroundColor Yellow }
Write-Host '─────────────────────────────────────────' -ForegroundColor Cyan
Write-Host ''

$seq = 0
$t0 = [DateTime]::UtcNow
$period = 1.0 / $Rate
$sent = 0
$lastReport = [DateTime]::UtcNow
$prevYaw = 0.0

try {
    while ($true) {
        $elapsed = ([DateTime]::UtcNow - $t0).TotalSeconds
        if ($Seconds -gt 0 -and $elapsed -ge $Seconds) { break }

        # 目标角
        if ($Still) {
            $yaw = 0.0; $pitch = 0.0
        } elseif ($Step -gt 0) {
            $phase = [math]::Floor($elapsed / 2.0) % 2
            $yaw = if ($phase -eq 0) { $Step } else { -$Step }
            $pitch = -$yaw / 3.0
        } else {
            $yaw = $Amp * [math]::Sin(2 * [math]::PI * $Freq * $elapsed)
            $pitch = ($Amp / 3.0) * [math]::Cos(2 * [math]::PI * $Freq * $elapsed)
        }

        # 角速度（差分）
        $dt = if ($elapsed -gt 0) { $period } else { $period }
        $yawRate = if ($elapsed -gt 0) { ($yaw - $prevYaw) / $dt } else { 0 }
        $prevYaw = $yaw

        # 组帧
        $b = New-Object byte[] 16
        [BitConverter]::GetBytes([uint16]0xA55A).CopyTo($b, 0)
        [BitConverter]::GetBytes([uint16]($seq -band 0xFFFF)).CopyTo($b, 2)
        [BitConverter]::GetBytes([int16][math]::Round($yaw * 100)).CopyTo($b, 4)
        [BitConverter]::GetBytes([int16][math]::Round($pitch * 100)).CopyTo($b, 6)
        [BitConverter]::GetBytes([int16][math]::Max(-32767, [math]::Min(32767, [math]::Round($yawRate * 10)))).CopyTo($b, 8)
        [BitConverter]::GetBytes([int16]0).CopyTo($b, 10)

        $flags = 0x01
        if ($EStop) { $flags = $flags -bor 0x08 }
        [BitConverter]::GetBytes([uint16]$flags).CopyTo($b, 12)
        $crc = Get-Crc16 $b[0..13]
        [BitConverter]::GetBytes($crc).CopyTo($b, 14)

        try {
            [void]$udp.Send($b, $b.Length, $ep)
            $sent++
        } catch {
            Write-Host "  发送失败: $($_.Exception.Message)" -ForegroundColor Red
        }

        $seq++

        # 每秒报一次
        $now = [DateTime]::UtcNow
        if (($now - $lastReport).TotalSeconds -ge 1) {
            if (-not $Quiet) {
                Write-Host ("  seq {0,6}  yaw {1,7:F1}°  pitch {2,7:F1}°  rate {3,7:F0}°/s  sent {4}" -f `
                    $seq, $yaw, $pitch, $yawRate, $sent) -ForegroundColor DarkGray
            }
            $sent = 0
            $lastReport = $now
        }

        Start-Sleep -Milliseconds ([int]($period * 1000))
    }
} finally {
    $udp.Close()
    Write-Host "`n已停止（共发 $seq 帧）" -ForegroundColor Cyan
}
