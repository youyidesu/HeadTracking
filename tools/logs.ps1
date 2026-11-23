# HT Turret 日志抓取（PowerShell 版，避免 cmd 的编码/重定向坑）
#   .\logs.ps1            重启 App → 抓 6 秒 → 过滤 → 打开
#   .\logs.ps1 -Live      实时滚动
#   .\logs.ps1 -Raw       不过滤，直接看原始 logcat
param(
    [switch]$Live,
    [switch]$Raw,
    [int]$WaitSeconds = 12
)

$ErrorActionPreference = 'Continue'
$adb = 'D:\i4\i4Tools9\files\adb\adb.exe'; $serial = '2G0YC5ZG3406T3'
$pkg = 'com.ht.turret'
$dir = $PSScriptRoot
$full = Join-Path $dir 'tarlog_full.txt'
$short = Join-Path $dir 'tarlog.txt'

if (-not (Test-Path $adb)) { Write-Host "找不到 adb: $adb" -ForegroundColor Red; exit 1 }

$devices = & $adb devices 2>&1 | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' }
if (-not $devices) { Write-Host "没有已授权的设备（戴上头显、插 USB、允许调试）" -ForegroundColor Yellow; exit 1 }

if ($Live) {
    Write-Host "实时日志，Ctrl+C 退出" -ForegroundColor Cyan
    & $adb -s $serial logcat -v time | Select-String -Pattern '\[App\]|\[HeadSender\]|BleLink|AndroidRuntime'
    exit 0
}

Write-Host "[1/3] 重启 $pkg" -ForegroundColor Cyan
& $adb -s $serial shell am force-stop $pkg 2>&1 | Out-Null
& $adb -s $serial shell am start -n "$pkg/com.unity3d.player.UnityPlayerGameActivity" 2>&1 | Out-Null
Start-Sleep -Seconds $WaitSeconds

Write-Host "[2/3] 等 Unity 完成初始化后抓日志" -ForegroundColor Cyan
& $adb -s $serial logcat -d -v time 2>&1 | Out-File -FilePath $full -Encoding utf8

if ($Raw) {
    Write-Host "原始日志: $full" -ForegroundColor Green
    & notepad $full
    exit 0
}

Write-Host "[3/3] 过滤关键行" -ForegroundColor Cyan
$lines = Get-Content $full | Select-String -Pattern '\[App\]|\[HeadSender\]|BleLink'
$lines | ForEach-Object { $_.Line } | Out-File -FilePath $short -Encoding utf8

if (-not $lines) {
    Write-Host "没抓到关键行，改用原始日志" -ForegroundColor Yellow
    & notepad $full
    exit 0
}

Write-Host ""
$lines | Select-Object -Last 30 | ForEach-Object { Write-Host $_.Line }
Write-Host ""
Write-Host "完整精简日志: $short" -ForegroundColor Green
& notepad $short
