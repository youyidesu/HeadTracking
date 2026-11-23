#Requires -Version 5.1
<#
    VideoBridge —— 采集卡 → 编码 → RTSP/WebRTC → Quest 浏览器，带 adb reverse 的一键开关

    设计目标
      · 用完即退：只有 start 期间才有 ffmpeg / mediamtx 进程，脚本自己不常驻 → 空闲 0 占用
      · 封装：所有可调参数都在 config.psd1，脚本只读配置
      · 可恢复：PID 存 state.json，stop 能收拾上次崩掉留下的孤儿进程
      · 自检：doctor 一次把 ffmpeg 设备、NVENC、端口、adb、C5 连通性全查一遍

    用法
      .\videobridge.ps1 start|stop|status|toggle|restart|doctor|logs
#>
# 引擎库：只定义函数，不执行动作。CLI 与 GUI 都 dot-source 它。

$script:Cfg = $null

function Initialize-Bridge {
    param([string]$ConfigPath)

    if (-not $ConfigPath) { $ConfigPath = Join-Path (Split-Path $PSScriptRoot -Parent) 'config.psd1' }
    $script:Cfg        = Import-PowerShellDataFile -Path $ConfigPath
    $script:ConfigPath = $ConfigPath
    $script:StateDir  = Join-Path $env:LOCALAPPDATA 'videobridge'
    $script:StateFile = Join-Path $script:StateDir 'state.json'
    $script:LogDir    = $script:Cfg.Logging.Dir
    if (-not [System.IO.Path]::IsPathRooted($script:LogDir)) {
        $script:LogDir = Join-Path $script:StateDir $script:LogDir
    }
    $script:CheckOk  = 0
    $script:CheckBad = 0

    foreach ($d in @($script:StateDir, $script:LogDir)) {
        if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
    }
}

# GUI / CLI 共用的状态快照
function Get-StatusSnapshot {
    $st = Get-State
    $r  = $script:Cfg.Relay
    $o = [ordered]@{
        Running     = $false
        FfmpegAlive = $false
        RelayAlive  = $false
        FfmpegPid   = 0
        RelayPid    = 0
        StartedAt   = ''
        RtspPort    = $r.RtspPort
        HttpPort    = $r.HttpPort
        PathName    = $r.PathName
        RtspListening = Test-PortListening $r.RtspPort
        HttpListening = Test-PortListening $r.HttpPort
        Device      = $script:Cfg.Video.DeviceName
        Size        = $script:Cfg.Video.Size
        Fps         = $script:Cfg.Video.Fps
        Bitrate     = $script:Cfg.Video.Bitrate
        Encoder     = $script:Cfg.Video.Encoder
        PagePort    = $script:Cfg.Node.Port
    }
    if ($st) {
        $o.FfmpegPid   = [int]$st.ffmpegPid
        $o.RelayPid    = [int]$st.mediamtxPid
        $o.FfmpegAlive = Test-Alive $st.ffmpegPid
        $o.RelayAlive  = Test-Alive $st.mediamtxPid
        $o.StartedAt   = [string]$st.startedAt
        $o.Running     = ($o.FfmpegAlive -or $o.RelayAlive)
    }
    $o
}

# ───────────────────────────────────────────────────────────────────────────
# 基础设施
# ───────────────────────────────────────────────────────────────────────────
function Write-Log {
    param([string]$Msg, [ValidateSet('INFO', 'WARN', 'ERR')][string]$Level = 'INFO')
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, $Msg
    $color = switch ($Level) { 'WARN' { 'Yellow' } 'ERR' { 'Red' } default { 'Gray' } }
    Write-Host $line -ForegroundColor $color
    try {
        $file = Join-Path $script:LogDir ('bridge-{0}.log' -f (Get-Date -Format 'yyyyMMdd'))
        Add-Content -Path $file -Value $line -Encoding UTF8
    } catch { }
}

function Get-State {
    if (Test-Path $script:StateFile) {
        try { return Get-Content $script:StateFile -Raw | ConvertFrom-Json } catch { }
    }
    return $null
}

function Save-State([hashtable]$S) {
    $S | ConvertTo-Json -Depth 4 | Set-Content -Path $script:StateFile -Encoding UTF8
}

function Clear-State {
    if (Test-Path $script:StateFile) { Remove-Item $script:StateFile -Force }
}

function Test-Alive($ProcId) {
    if (-not $ProcId) { return $false }
    return [bool](Get-Process -Id $ProcId -ErrorAction SilentlyContinue)
}

function Test-PortListening([int]$Port) {
    $conn = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
    if ($null -ne $conn) { return [bool]$conn }
    $ns = netstat -an | Select-String -Pattern ":$Port\s" | Select-String 'LISTENING'
    return [bool]$ns
}

function Stop-Tree($ProcId, [string]$Name) {
    if (-not (Test-Alive $ProcId)) { return }
    try {
        & taskkill.exe /PID $ProcId /T /F 2>$null | Out-Null
        Write-Log "已停止 $Name (PID $ProcId)" 'INFO'
    } catch {
        Write-Log "停止 $Name 失败: $_" 'WARN'
    }
}

function Get-FfmpegPath {
    $p = $null
    if ($script:Cfg.Video.ContainsKey('FfmpegPath')) { $p = $script:Cfg.Video.FfmpegPath }
    if ($p -and (Test-Path $p)) { return $p }
    $cmd = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Test-Nvenc([string]$Ffmpeg) {
    try {
        $out = & $Ffmpeg -hide_banner -encoders 2>&1 | Out-String
        return ($out -match 'h264_nvenc')
    } catch { return $false }
}

# ───────────────────────────────────────────────────────────────────────────
# 各部件
# ───────────────────────────────────────────────────────────────────────────
function Start-Relay([hashtable]$S) {
    $r = $script:Cfg.Relay
    if (-not $r.AutoStart) { Write-Log 'Relay 关闭（使用外部 RTSP）'; return }
    if (-not (Test-Path $r.ExePath)) { throw "找不到 MediaMTX: $($r.ExePath)" }
    if (Test-PortListening $r.RtspPort) { Write-Log "RTSP 端口 $($r.RtspPort) 已被占用，沿用现有服务" 'WARN'; return }

    $log = Join-Path $script:LogDir 'mediamtx.log'
    $p = Start-Process -FilePath $r.ExePath -WorkingDirectory (Split-Path $r.ExePath) `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
                       -WindowStyle Hidden -PassThru
    $S.mediamtxPid = $p.Id
    Write-Log "MediaMTX 启动 (PID $($p.Id))  RTSP:$($r.RtspPort)  WHEP:http://127.0.0.1:$($r.HttpPort)/$($r.PathName)"
    Start-Sleep -Milliseconds 600
}

function Start-Video([hashtable]$S) {
    $v = $script:Cfg.Video
    $relay = $script:Cfg.Relay
    $ff = Get-FfmpegPath
    if (-not $ff) { throw '找不到 ffmpeg.exe：加进 PATH，或在 config.psd1 写 Video.FfmpegPath' }

    $enc = $v.Encoder
    if ($enc -eq 'auto') {
        if (Test-Nvenc $ff) { $enc = 'nvenc' }
        else { Write-Log '未检测到 NVENC，退回 libx264' 'WARN'; $enc = 'x264' }
    }

    $gop = [int]($v.Fps * $v.KeyFrameSec)
    if ($enc -eq 'nvenc') {
        $encLine = @('-c:v','h264_nvenc','-preset','p1','-tune','ull','-rc','cbr',
                     '-b:v',$v.Bitrate,'-maxrate',$v.Bitrate,'-bufsize','4M',
                     '-g',"$gop",'-bf','0') -join ' '
    } else {
        $encLine = @('-c:v','libx264','-preset','ultrafast','-tune','zerolatency',
                     '-b:v',$v.Bitrate,'-g',"$gop",'-bf','0') -join ' '
    }

    $extra = ''
    if ($v.ExtraArgs -and $v.ExtraArgs.Count -gt 0) { $extra = ' ' + ($v.ExtraArgs -join ' ') }

    # 设备名带空格，必须显式加引号，否则 Start-Process 会拆成两个参数
    $argLine = @(
        '-hide_banner','-loglevel','warning',
        '-f', $v.InputFormat, '-rtbufsize','64M',
        '-video_size', $v.Size, '-framerate', "$($v.Fps)",
        "-i `"video=$($v.DeviceName)`""
    ) -join ' '
    $argLine += " $encLine -pix_fmt yuv420p$extra"
    $argLine += " -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:$($relay.RtspPort)/$($relay.PathName)"

    Write-Log ("ffmpeg: {0}  {1}@{2}fps  → rtsp/{3}" -f $enc, $v.Size, $v.Fps, $relay.PathName)
    $log = Join-Path $script:LogDir 'ffmpeg.log'
    $p = Start-Process -FilePath $ff -ArgumentList $argLine -RedirectStandardError $log `
                       -WindowStyle Hidden -PassThru
    $S.ffmpegPid = $p.Id
    Start-Sleep -Milliseconds 900
    if (-not (Test-Alive $p.Id)) {
        $tail = (Get-Content $log -Tail 15 -ErrorAction SilentlyContinue) -join "`n"
        throw "ffmpeg 启动后立即退出。最后几行:`n$tail"
    }
    Write-Log "ffmpeg 运行中 (PID $($p.Id))"
}

function Start-Node([hashtable]$S) {
    $n = $script:Cfg.Node
    if (-not $n.Enabled) { return }
    if (Test-PortListening $n.Port) { Write-Log "Node 端口 $($n.Port) 已在监听"; return }
    if (-not (Test-Path $n.WorkDir)) { Write-Log "Node 工作目录不存在: $($n.WorkDir)" 'WARN'; return }

    $log = Join-Path $script:LogDir 'node.log'
    $p = Start-Process -FilePath $n.ExePath -ArgumentList ($n.Args -join ' ') -WorkingDirectory $n.WorkDir `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
                       -WindowStyle Hidden -PassThru
    $S.nodePid = $p.Id
    Write-Log "Node 启动 (PID $($p.Id))"
}

function Test-Standalone {
    return ([string]$script:Cfg.Link.Mode -eq 'standalone')
}

function Set-Link([hashtable]$S) {
    $l = $script:Cfg.Link
    if (Test-Standalone) {
        Write-Log '独立模式：跳过 adb reverse（头显直连 C5，不经电脑）'
        return
    }
    if (-not $l.UseAdbReverse) { return }
    try { & $l.AdbPath version 2>&1 | Out-Null }
    catch { Write-Log '未找到 adb，跳过 reverse' 'WARN'; return }

    $dev = (& $l.AdbPath devices 2>&1) -join "`n"
    if ($dev -notmatch 'device') {
        Write-Log 'adb 没看到设备（头显没连 / 没开 USB 调试），跳过 reverse' 'WARN'
        return
    }
    foreach ($port in $l.ReversePorts) {
        & $l.AdbPath reverse "tcp:$port" "tcp:$port" 2>&1 | Out-Null
        Write-Log "adb reverse tcp:$port → localhost:$port"
    }
    $S.adbPorts = @($l.ReversePorts)
}

function Clear-Link {
    $l = $script:Cfg.Link
    if (-not $l.UseAdbReverse) { return }
    try {
        foreach ($port in $l.ReversePorts) { & $l.AdbPath reverse --remove "tcp:$port" 2>&1 | Out-Null }
        Write-Log 'adb reverse 已撤销'
    } catch { }
}

# 把 quest/ 打成一个可直接丢给 C5 的独立包
function Invoke-Pack {
    $srcQuest = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'quest'
    $out = Join-Path (Split-Path $PSScriptRoot -Parent) 'out\standalone'
    if (-not (Test-Path $srcQuest)) { throw "找不到 quest 目录: $srcQuest" }

    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    New-Item -ItemType Directory -Path $out -Force | Out-Null
    Copy-Item (Join-Path $srcQuest '*') $out -Recurse -Force

    $mode = [ordered]@{
        mode     = 'standalone'
        wsUrl    = "ws://$($script:Cfg.Link.C5Ip)/ws"
        httpUrl  = "http://$($script:Cfg.Link.C5Ip)/"
        video    = $false
        note     = 'C5 需要以 https 提供本目录，头显访问后 WebXR 才可用（见 docs/STANDALONE_MODE.md）'
    }
    $mode | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'mode.json') -Encoding UTF8

    Write-Log "独立包已生成: $out（含 mode.json）"
    Write-Log '把整个目录烧进 C5 的 SPIFFS/文件系统，或用 C5 的 httpd 挂载'
}

# ───────────────────────────────────────────────────────────────────────────
# 动作
# ───────────────────────────────────────────────────────────────────────────
function Stop-All($S) {
    if ($S.ffmpegPid)   { Stop-Tree $S.ffmpegPid   'ffmpeg' }
    if ($S.mediamtxPid) { Stop-Tree $S.mediamtxPid 'mediamtx' }
    if ($S.nodePid)     { Stop-Tree $S.nodePid     'node' }
}

function Invoke-Start {
    if (Test-Standalone) {
        Write-Log '独立模式：PC 端不启动任何服务' 'INFO'
        Write-Log '  头显连 C5 的 AP，浏览器打开 C5 上的页面（见 docs/STANDALONE_MODE.md）'
        Write-Log '  要出图传或走 PC 转发头姿，请把 config.psd1 的 Link.Mode 改回 pc'
        return
    }

    $st = Get-State
    if ($st -and ((Test-Alive $st.ffmpegPid) -or (Test-Alive $st.mediamtxPid))) {
        Write-Log '已经在运行（需要重来请用 restart）' 'WARN'
        Show-Status
        return
    }

    $S = @{ startedAt = (Get-Date).ToString('s'); version = $script:Cfg.Version }
    try {
        Set-Link $S
        Start-Relay $S
        Start-Video $S
        Start-Node $S
        Save-State $S
        Write-Log '启动完成'
        Show-Status
    } catch {
        Write-Log "启动失败：$_" 'ERR'
        Stop-All $S
        Clear-Link
        Clear-State
        throw
    }
}

function Invoke-Stop {
    $st = Get-State
    if ($st) {
        Stop-All $st
    } else {
        Get-Process ffmpeg, mediamtx -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Log "清理孤儿进程 $($_.ProcessName) (PID $($_.Id))" 'WARN'
            Stop-Tree $_.Id $_.ProcessName
        }
    }
    Clear-Link
    Clear-State
    Write-Log '已停止，当前无后台进程占用'
}

function Show-Status {
    $st = Get-State
    $r  = $script:Cfg.Relay
    Write-Host ''
    Write-Host '── VideoBridge 状态 ─────────────────────────────' -ForegroundColor Cyan

    if (-not $st) {
        Write-Host '  未运行（无状态文件）' -ForegroundColor DarkGray
    } else {
        $rows = @(
            @{ n = 'ffmpeg';   p = $st.ffmpegPid },
            @{ n = 'mediamtx'; p = $st.mediamtxPid },
            @{ n = 'node';     p = $st.nodePid }
        )
        foreach ($row in $rows) {
            if (-not $row.p) { continue }
            $alive = Test-Alive $row.p
            $mark  = '○ 已退出'
            $col   = 'DarkGray'
            if ($alive) { $mark = '● 运行中'; $col = 'Green' }
            Write-Host ("  {0,-9} PID {1,-7} {2}" -f $row.n, $row.p, $mark) -ForegroundColor $col
        }
        Write-Host ("  启动于   {0}" -f $st.startedAt)
    }

    $rtsp = '未监听'
    if (Test-PortListening $r.RtspPort) { $rtsp = '在监听' }
    Write-Host ("  RTSP     :{0} {1}" -f $r.RtspPort, $rtsp)
    Write-Host ("  WHEP     http://127.0.0.1:{0}/{1}" -f $r.HttpPort, $r.PathName)
    Write-Host ("  页面     http://127.0.0.1:{0}" -f $script:Cfg.Node.Port)
    Write-Host '─────────────────────────────────────────────────' -ForegroundColor Cyan
    Write-Host ''
}

function Test-Item([string]$Name, [bool]$Cond, [string]$Detail = '') {
    if ($Cond) {
        Write-Host ("  [ OK ] {0,-26} {1}" -f $Name, $Detail) -ForegroundColor Green
        $script:CheckOk++
    } else {
        Write-Host ("  [FAIL] {0,-26} {1}" -f $Name, $Detail) -ForegroundColor Red
        $script:CheckBad++
    }
}

function Invoke-Doctor {
    $v = $script:Cfg.Video; $r = $script:Cfg.Relay; $l = $script:Cfg.Link
    $script:CheckOk = 0; $script:CheckBad = 0

    Write-Host "`n── Doctor ───────────────────────────────────────" -ForegroundColor Cyan

    $ff = Get-FfmpegPath
    Test-Item 'ffmpeg' ([bool]$ff) ($(if ($ff) { $ff } else { '未找到' }))
    if ($ff) {
        $ver = (& $ff -hide_banner -version 2>&1 | Select-Object -First 1)
        Write-Host "         $ver" -ForegroundColor DarkGray
        $nvenc = Test-Nvenc $ff
        if ($nvenc) { Test-Item 'NVENC' $true 'h264_nvenc 可用' }
        else        { Test-Item 'NVENC' $false '只有 CPU 编码（1080p60 会吃满 CPU）' }

        $devOut = (& $ff -hide_banner -f dshow -list_devices true -i dummy 2>&1) -join "`n"
        $hasDev = $devOut -match [regex]::Escape($v.DeviceName)
        Test-Item "采集设备 '$($v.DeviceName)'" $hasDev
        if (-not $hasDev) {
            Write-Host '         可用设备：' -ForegroundColor DarkGray
            ($devOut -split "`n" | Where-Object { $_ -match '"' }) | ForEach-Object {
                Write-Host "         $_" -ForegroundColor DarkGray
            }
        }
    }

    Test-Item 'MediaMTX' (Test-Path $r.ExePath) $r.ExePath

    $state = Get-State
    $rtspFree = -not (Test-PortListening $r.RtspPort)
    Test-Item "RTSP 端口 $($r.RtspPort)" ($rtspFree -or [bool]$state) ($(if ($rtspFree) { '空闲' } else { '被占用（已在运行时属正常）' }))
    $httpFree = -not (Test-PortListening $r.HttpPort)
    Test-Item "WHEP 端口 $($r.HttpPort)" ($httpFree -or [bool]$state) ($(if ($httpFree) { '空闲' } else { '被占用（已在运行时属正常）' }))

    $adbOk = $false; $adbDetail = '未找到 adb'
    try {
        $dev = (& $l.AdbPath devices 2>&1) -join "`n"
        $adbOk = ($dev -match "device\s")
        $adbDetail = (($dev -split "`n") | Where-Object { $_ -match 'device' } | Select-Object -First 1)
    } catch { }
    Test-Item 'adb 设备' $adbOk $adbDetail

    $c5Ok = $false
    try { $c5Ok = [bool](Test-Connection -ComputerName $l.C5Ip -Count 1 -Quiet -ErrorAction Stop) } catch { }
    Test-Item "C5 $($l.C5Ip)" $c5Ok ($(if ($c5Ok) { 'ping 通过' } else { 'ping 失败（检查 PC 是否连上 C5 的 AP）' }))

    Write-Host ("─────────────────────────────────────────────────`n  通过 {0} / 失败 {1}`n" -f $script:CheckOk, $script:CheckBad) -ForegroundColor Cyan
}

function Show-Logs([int]$N) {
    $files = Get-ChildItem $script:LogDir -Filter '*.log' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 3
    if (-not $files) { Write-Host '还没有日志' -ForegroundColor DarkGray; return }
    foreach ($f in $files) {
        Write-Host "`n=== $($f.Name) ===" -ForegroundColor Cyan
        Get-Content $f.FullName -Tail $N
    }
}
