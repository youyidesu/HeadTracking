#Requires -Version 5.1
<#
    HeadTracker.ps1 —— 头追桥接控制台（WPF 设置页 + 状态监测）

    · 引擎逻辑全部复用 app/lib/engine.ps1，这里只做界面
    · 关掉窗口 = 停止桥接（不留后台进程）
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase

. (Join-Path $PSScriptRoot 'lib/engine.ps1')
Initialize-Bridge -ConfigPath (Join-Path $PSScriptRoot 'config.psd1')

$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="HeadTracking.Bridge — 头追桥接控制台"
        Height="640" Width="1000" WindowStartupLocation="CenterScreen"
        Background="#1E1E1E" Foreground="#DDDDDD" FontFamily="Microsoft YaHei UI" FontSize="12">
  <Grid Margin="14">
    <Grid.ColumnDefinitions>
      <ColumnDefinition Width="310"/>
      <ColumnDefinition Width="14"/>
      <ColumnDefinition Width="*"/>
    </Grid.ColumnDefinitions>
    <Grid.RowDefinitions>
      <RowDefinition Height="*"/>
      <RowDefinition Height="Auto"/>
    </Grid.RowDefinitions>

    <!-- 左：状态 -->
    <Border Grid.Column="0" Background="#252526" CornerRadius="6" Padding="14">
      <StackPanel>
        <TextBlock Text="运行状态" FontSize="13" FontWeight="Bold" Foreground="#9CDCFE" Margin="0,0,0,10"/>
        <TextBlock Name="txtState" Text="未运行" FontSize="22" FontWeight="Bold" Foreground="#808080"/>
        <TextBlock Name="txtLines" FontFamily="Consolas" FontSize="12" Foreground="#BBBBBB"
                   TextWrapping="Wrap" Margin="0,12,0,0"/>
      </StackPanel>
    </Border>

    <!-- 右：设置 / 自检 / 日志 -->
    <TabControl Grid.Column="2" Background="#252526" BorderBrush="#3F3F46">
      <TabItem Header="设置">
        <ScrollViewer VerticalScrollBarVisibility="Auto" Background="#1E1E1E">
          <StackPanel Name="pnlSettings" Margin="14"/>
        </ScrollViewer>
      </TabItem>
      <TabItem Header="自检">
        <Grid Background="#1E1E1E">
          <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
          </Grid.RowDefinitions>
          <Button Name="btnDoctor" Content="运行 doctor" Width="120" Height="30"
                  HorizontalAlignment="Left" Margin="12"/>
          <TextBox Name="txtDoctor" Grid.Row="1" Margin="12,0,12,12" IsReadOnly="True"
                   Background="#111111" Foreground="#D4D4D4" FontFamily="Consolas"
                   FontSize="12" TextWrapping="NoWrap"
                   VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Auto"/>
        </Grid>
      </TabItem>
      <TabItem Header="日志">
        <Grid Background="#1E1E1E">
          <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
          </Grid.RowDefinitions>
          <Button Name="btnReloadLogs" Content="刷新日志" Width="100" Height="30"
                  HorizontalAlignment="Left" Margin="12"/>
          <TextBox Name="txtLogs" Grid.Row="1" Margin="12,0,12,12" IsReadOnly="True"
                   Background="#111111" Foreground="#D4D4D4" FontFamily="Consolas"
                   FontSize="12" VerticalScrollBarVisibility="Auto"/>
        </Grid>
      </TabItem>
    </TabControl>

    <!-- 底：操作 -->
    <StackPanel Grid.Row="1" Grid.ColumnSpan="3" Orientation="Horizontal" Margin="0,14,0,0">
      <Button Name="btnStart"    Content="启动"     Width="96"  Height="34" Margin="0,0,8,0"/>
      <Button Name="btnStop"     Content="停止"     Width="96"  Height="34" Margin="0,0,8,0"/>
      <Button Name="btnRestart"  Content="重启"     Width="96"  Height="34" Margin="0,0,8,0"/>
      <Button Name="btnSave"     Content="保存设置" Width="110" Height="34" Margin="24,0,8,0"/>
      <Button Name="btnOpenDir"  Content="打开日志目录" Width="120" Height="34" Margin="0,0,8,0"/>
      <Button Name="btnPage"     Content="测试页面"  Width="100" Height="34" Margin="0,0,8,0"/>
      <Button Name="btnPack"     Content="打包独立版" Width="120" Height="34"/>
      <TextBlock Name="txtHint" VerticalAlignment="Center" Margin="16,0,0,0" Foreground="#808080"/>
    </StackPanel>
  </Grid>
</Window>
'@

$win = [Windows.Markup.XamlReader]::Parse($xaml)
foreach ($n in 'txtState','txtLines','pnlSettings','txtDoctor','txtLogs','txtHint',
               'btnStart','btnStop','btnRestart','btnSave','btnOpenDir','btnPage','btnDoctor','btnReloadLogs','btnPack') {
    Set-Variable -Name $n -Value $win.FindName($n) -Scope Script
}

# ───────────────────────────────────────────────────────────────────────────
# 设置表单
# ───────────────────────────────────────────────────────────────────────────
$Fields = @(
    @{ k='Link.Mode';         l='运行模式';        t='combo'; o=@('pc','standalone') }
    @{ k='Video.DeviceName';  l='采集设备名';      t='text' }
    @{ k='Video.Size';        l='分辨率';          t='combo'; o=@('1920x1080','1280x720','2560x1440') }
    @{ k='Video.Fps';         l='帧率';            t='combo'; o=@('60','30') }
    @{ k='Video.Bitrate';     l='码率';            t='combo'; o=@('4M','6M','8M','12M') }
    @{ k='Video.Encoder';     l='编码器';          t='combo'; o=@('auto','nvenc','x264') }
    @{ k='Video.KeyFrameSec'; l='关键帧间隔(秒)';  t='text' }
    @{ k='Relay.ExePath';     l='MediaMTX 路径';   t='text' }
    @{ k='Relay.RtspPort';    l='RTSP 端口';       t='text' }
    @{ k='Relay.HttpPort';    l='WHEP 端口';       t='text' }
    @{ k='Relay.PathName';    l='流名称';          t='text' }
    @{ k='Link.UseAdbReverse';l='启用 adb reverse';t='check' }
    @{ k='Link.AdbPath';      l='adb 路径';        t='text' }
    @{ k='Link.ReversePorts'; l='映射端口(逗号)';  t='text' }
    @{ k='Link.C5Ip';         l='C5 IP';           t='text' }
    @{ k='Node.Enabled';      l='启动 Node 服务';  t='check' }
    @{ k='Node.WorkDir';      l='Node 工作目录';   t='text' }
    @{ k='Node.Port';         l='页面端口';        t='text' }
)

$script:Ctl = @{}

function Get-CfgValue([string]$Key) {
    $parts = $Key -split '\.'
    $node  = $script:Cfg
    foreach ($p in $parts) {
        if ($node -is [hashtable] -or $node -is [System.Collections.IDictionary]) { $node = $node[$p] }
        else { $node = $node.$p }
    }
    return $node
}

$row = 0
foreach ($f in $Fields) {
    $lbl = New-Object System.Windows.Controls.TextBlock
    $lbl.Text = $f.l
    $lbl.Foreground = '#BBBBBB'
    $lbl.VerticalAlignment = 'Center'
    $lbl.Margin = '0,0,10,0'
    [System.Windows.Controls.Grid]::SetRow($lbl, $row)

    $cur = Get-CfgValue $f.k

    switch ($f.t) {
        'combo' {
            $c = New-Object System.Windows.Controls.ComboBox
            $c.Height = 26
            foreach ($o in $f.o) { [void]$c.Items.Add($o) }
            $c.Text = [string]$cur
            $f.k | ForEach-Object { }
            $script:Ctl[$f.k] = $c
        }
        'check' {
            $c = New-Object System.Windows.Controls.CheckBox
            $c.IsChecked = [bool]$cur
            $c.VerticalAlignment = 'Center'
            $script:Ctl[$f.k] = $c
        }
        default {
            $c = New-Object System.Windows.Controls.TextBox
            $c.Height = 26
            if ($cur -is [array]) { $c.Text = ($cur -join ',') } else { $c.Text = [string]$cur }
            $script:Ctl[$f.k] = $c
        }
    }
    [System.Windows.Controls.Grid]::SetRow($c, $row)

    $g = New-Object System.Windows.Controls.Grid
    $g.Margin = '0,0,0,8'
    $g.ColumnDefinitions.Add((New-Object System.Windows.Controls.ColumnDefinition -Property @{ Width = '150' }))
    $g.ColumnDefinitions.Add((New-Object System.Windows.Controls.ColumnDefinition -Property @{ Width = '*'   }))
    [void]$g.Children.Add($lbl)
    [System.Windows.Controls.Grid]::SetColumn($c, 1)
    [void]$g.Children.Add($c)

    [void]$pnlSettings.Children.Add($g)
    $row++
}

# ───────────────────────────────────────────────────────────────────────────
# 保存配置
# ───────────────────────────────────────────────────────────────────────────
function Esc([string]$s) { return $s -replace "'", "''" }

function Save-Config {
    $v  = { param($k) $script:Ctl[$k] }
    $dev = Esc $script:Ctl['Video.DeviceName'].Text
    $size = Esc $script:Ctl['Video.Size'].Text
    $fps  = Esc $script:Ctl['Video.Fps'].Text
    $br   = Esc $script:Ctl['Video.Bitrate'].Text
    $enc  = Esc $script:Ctl['Video.Encoder'].Text
    $kf   = Esc $script:Ctl['Video.KeyFrameSec'].Text
    $relay= Esc $script:Ctl['Relay.ExePath'].Text
    $rtsp = Esc $script:Ctl['Relay.RtspPort'].Text
    $http = Esc $script:Ctl['Relay.HttpPort'].Text
    $pn   = Esc $script:Ctl['Relay.PathName'].Text
    $mode = Esc $script:Ctl['Link.Mode'].Text
    $useAdb = [bool]$script:Ctl['Link.UseAdbReverse'].IsChecked
    $adb  = Esc $script:Ctl['Link.AdbPath'].Text
    $ports = (Esc $script:Ctl['Link.ReversePorts'].Text) -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    $portList = ($ports | ForEach-Object { [int]$_ }) -join ','
    $c5   = Esc $script:Ctl['Link.C5Ip'].Text
    $nEn  = [bool]$script:Ctl['Node.Enabled'].IsChecked
    $nDir = Esc $script:Ctl['Node.WorkDir'].Text
    $nPort= Esc $script:Ctl['Node.Port'].Text

    $text = @"
@{`n    Version = '1.1.0'`n`n    Video = @{`n        DeviceName  = '$dev'`n        InputFormat = 'dshow'`n        Size        = '$size'`n        Fps         = $fps`n        Bitrate     = '$br'`n        Encoder     = '$enc'`n        KeyFrameSec = $kf`n        ExtraArgs   = @()`n    }`n`n    Relay = @{`n        ExePath   = '$relay'`n        RtspPort  = $rtsp`n        HttpPort  = $http`n        PathName  = '$pn'`n        AutoStart = `$true`n    }`n`n    Link = @{`n        Mode          = '$mode'`n        UseAdbReverse = `$$($useAdb.ToString().ToLower())`n        AdbPath       = '$adb'`n        ReversePorts  = @($portList)`n        C5Ip          = '$c5'`n        C5UdpPort     = 5005`n    }`n`n    Node = @{`n        Enabled = `$$($nEn.ToString().ToLower())`n        ExePath = 'node'`n        WorkDir = '$nDir'`n        Args    = @('server.js')`n        Port    = $nPort`n    }`n`n    Logging = @{`n        Dir      = 'logs'`n        KeepDays = 14`n    }`n}`n
"@
    $path = Join-Path $PSScriptRoot 'config.psd1'
    Set-Content -Path $path -Value $text -Encoding UTF8
    Initialize-Bridge -ConfigPath $path
    $txtHint.Text = '设置已保存'
}

# ───────────────────────────────────────────────────────────────────────────
# 状态刷新
# ───────────────────────────────────────────────────────────────────────────
function Refresh-Status {
    try {
        $s = Get-StatusSnapshot
        if ($s.Running) {
            $txtState.Text = '运行中'
            $txtState.Foreground = '#4EC9B0'
        } else {
            $txtState.Text = '未运行'
            $txtState.Foreground = '#808080'
        }
        $ff = '○ 未起';  if ($s.FfmpegAlive) { $ff = "● PID $($s.FfmpegPid)" }
        $rl = '○ 未起';  if ($s.RelayAlive)  { $rl = "● PID $($s.RelayPid)" }
        $rtsp = '未监听'; if ($s.RtspListening) { $rtsp = '在监听' }
        $http = '未监听'; if ($s.HttpListening) { $http = '在监听' }

        $txtLines.Text = @(
            "ffmpeg    $ff"
            "mediamtx  $rl"
            "RTSP:$($s.RtspPort)   $rtsp"
            "WHEP:$($s.HttpPort)   $http"
            ""
            "设备      $($s.Device)"
            "格式      $($s.Size) @ $($s.Fps)fps  $($s.Bitrate)  $($s.Encoder)"
            ""
            "页面      http://127.0.0.1:$($s.PagePort)"
            "视频      http://127.0.0.1:$($s.HttpPort)/$($s.PathName)"
            ""
            "启动于    $($s.StartedAt)"
        ) -join "`n"
    } catch {
        $txtLines.Text = "状态读取失败: $_"
    }
}

function Invoke-Safe([scriptblock]$Block, [string]$Name) {
    try { & $Block; $txtHint.Text = "$Name 完成" }
    catch { $txtHint.Text = "$Name 失败：$($_.Exception.Message)" }
    Refresh-Status
}

# ───────────────────────────────────────────────────────────────────────────
# 事件
# ───────────────────────────────────────────────────────────────────────────
$btnStart.Add_Click({   Invoke-Safe { Invoke-Start } '启动' })
$btnStop.Add_Click({    Invoke-Safe { Invoke-Stop  } '停止' })
$btnRestart.Add_Click({ Invoke-Safe { Invoke-Stop; Start-Sleep -Milliseconds 300; Invoke-Start } '重启' })
$btnSave.Add_Click({    Invoke-Safe { Save-Config } '保存设置' })

$btnDoctor.Add_Click({
    $txtDoctor.Text = '正在自检…'
    try {
        $out = Invoke-Doctor 6>&1 | Out-String
        $txtDoctor.Text = ($out -replace "`e\[[0-9;]*m", '')
    } catch { $txtDoctor.Text = "自检失败: $_" }
})

$btnReloadLogs.Add_Click({
    try {
        $files = Get-ChildItem $script:LogDir -Filter '*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 3
        $txtLogs.Text = ($files | ForEach-Object { "=== $($_.Name) ==="; (Get-Content $_.FullName -Tail 40) }) -join "`n"
    } catch { $txtLogs.Text = "读取日志失败: $_" }
})

$btnOpenDir.Add_Click({ Start-Process explorer.exe $script:LogDir })
$btnPack.Add_Click({ Invoke-Safe { Invoke-Pack } '打包独立版' })
$btnPage.Add_Click({ Start-Process "http://127.0.0.1:$($script:Cfg.Node.Port)" })

$timer = New-Object System.Windows.Threading.DispatcherTimer
$timer.Interval = [TimeSpan]::FromSeconds(1)
$timer.Add_Tick({ Refresh-Status })
$timer.Start()

Refresh-Status
[void]$win.ShowDialog()
