# 面板参数微调（改设备上的 ht.cfg 并重启 App，不需要重新打包）
#   .\tune.ps1 -Down 0.1          面板再往下 10cm（负值表示往上）
#   .\tune.ps1 -Set 0.55          直接指定 down（可负数）
#   .\tune.ps1 -Dist 1.8          面板推远（转头的"扫动感"会变小）
#   .\tune.ps1 -Width 1.0         面板变窄
#   .\tune.ps1 -FlipY 0           关掉上下翻转
#   .\tune.ps1 -Show              看当前值
param(
    [double]$Down = 0,
    [double]$Up = 0,
    [double]$Set = [double]::NaN,
    [double]$Dist = [double]::NaN,
    [double]$Width = [double]::NaN,
    [double]$Tilt = [double]::NaN,
    [int]$FlipX = -1,
    [int]$FlipY = -1,
    [switch]$Show,
    [switch]$NoRestart
)

$adb = 'D:\i4\i4Tools9\files\adb\adb.exe'; $serial = '2G0YC5ZG3406T3'
$cfgPath = '/sdcard/Android/data/com.ht.turret/files/ht.cfg'
$pkg = 'com.ht.turret'

if (-not (Test-Path $adb)) { Write-Host "找不到 adb: $adb" -ForegroundColor Red; exit 1 }

# 读当前值
$raw = (& $adb -s $serial shell "cat $cfgPath" 2>&1) -join "`n"
$v = @{ down = 1.25; dist = 2.20; width = 4.80; tilt = 0.0; follow = 1; followk = 0.30; flipx = 0; flipy = 1 }
foreach ($line in ($raw -split "`n")) {
    $t = $line.Trim()
    if ($t -match '^(\w+)\s*=\s*([-0-9.]+)') {
        $k = $matches[1].ToLower()
        if ($v.ContainsKey($k)) { $v[$k] = [double]$matches[2] }
    }
}

if ($Show) {
    Write-Host "down=$($v.down)  dist=$($v.dist)  width=$($v.width)  followk=$($v.followk)  flipx=$($v.flipx)  flipy=$($v.flipy)"
    exit 0
}

if (-not [double]::IsNaN($Set))   { $v.down  = $Set }
if ($Down -ne 0)                  { $v.down  = [math]::Round($v.down + $Down, 2) }
if ($Up -ne 0)                    { $v.down  = [math]::Round($v.down - $Up, 2) }
if (-not [double]::IsNaN($Dist))  { $v.dist  = $Dist }
if (-not [double]::IsNaN($Width)) { $v.width = $Width }
if (-not [double]::IsNaN($Tilt))  { $v.tilt  = $Tilt }
if ($FlipX -ge 0)                 { $v.flipx = $FlipX }
if ($FlipY -ge 0)                 { $v.flipy = $FlipY }

$text = "down=$($v.down)`ndist=$($v.dist)`nwidth=$($v.width)`ntilt=$($v.tilt)`nfollow=$($v.follow)`nfollowk=$($v.followk)`nflipx=$($v.flipx)`nflipy=$($v.flipy)`n"
$tmp = Join-Path $env:TEMP 'ht.cfg'
Set-Content -Path $tmp -Value $text -Encoding ascii -NoNewline
& $adb -s $serial push $tmp $cfgPath 2>&1 | Out-Null

Write-Host "down=$($v.down)  dist=$($v.dist)  width=$($v.width)  flipx=$($v.flipx)  flipy=$($v.flipy)" -ForegroundColor Green

if (-not $NoRestart) {
    & $adb -s $serial shell am force-stop $pkg 2>&1 | Out-Null
    Start-Sleep -Seconds 1
    & $adb -s $serial shell am start -n "$pkg/com.unity3d.player.UnityPlayerGameActivity" 2>&1 | Out-Null
    Write-Host "App 已重启" -ForegroundColor Cyan
}
