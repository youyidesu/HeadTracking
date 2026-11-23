@{
    # ─────────────────────────────────────────────────────────────────────
    # VideoBridge 配置 —— 改这里就够了，脚本本身不用动
    # ─────────────────────────────────────────────────────────────────────
    Version = '1.0.0'

    Video = @{
        DeviceName  = 'UGREEN 95348'   # 本机实测的采集卡（doctor 会列出所有可用设备）
        InputFormat = 'dshow'
        Size        = '1920x1080'      # 降级到 '1280x720' 能省一半带宽
        Fps         = 60
        Bitrate     = '8M'
        Encoder     = 'auto'           # auto | nvenc | x264
        KeyFrameSec = 2                # 关键帧间隔（秒）；网络抖动大时改 1
        ExtraArgs   = @()              # 额外 ffmpeg 参数（例如烧时间码的 -vf）
    }

    Relay = @{
        ExePath   = 'C:\tools\mediamtx\mediamtx.exe'
        RtspPort  = 8554
        HttpPort  = 8889
        PathName  = 'cam'
        AutoStart = $true              # 关掉表示用外部已有的 RTSP 服务
    }

    Link = @{
        # pc          = 头显接电脑（adb reverse + PC 出图 + PC 转发头姿）
        # standalone  = 头显独立（页面由 C5 提供，头姿直连 C5，无图传）
        Mode          = 'pc'    # pc | standalone（见 docs/STANDALONE_MODE.md）
        UseAdbReverse = $true          # Quest 走 USB 时把 PC 端口映射进头显 localhost
        AdbPath       = 'adb'
        ReversePorts  = @(8080, 8889)
        C5Ip          = '192.168.4.1'  # 只用于连通性自检（doctor）
        C5UdpPort     = 5005
    }

    Node = @{
        Enabled = $false               # ControlAnything 的 Node 服务，按需打开
        ExePath = 'node'
        WorkDir = 'C:\tools\controlanything'
        Args    = @('server.js')
        Port    = 8080
    }

    Logging = @{
        Dir      = 'logs'              # 相对 -> %LOCALAPPDATA%\videobridge\logs
        KeepDays = 14
    }
}
