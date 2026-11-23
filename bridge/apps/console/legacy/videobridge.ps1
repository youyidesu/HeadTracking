#Requires -Version 5.1
<#
    videobridge.ps1 —— 命令行入口（GUI 用的是同一套 lib/engine.ps1）
      start | stop | status | toggle | restart | doctor | logs
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('start', 'stop', 'status', 'toggle', 'restart', 'doctor', 'logs', 'pack')]
    [string]$Action,

    [string]$Config,
    [int]$Tail = 30
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib/engine.ps1')
Initialize-Bridge -ConfigPath $Config

switch ($Action) {
    'start'   { Invoke-Start }
    'stop'    { Invoke-Stop }
    'status'  { Show-Status }
    'restart' { Invoke-Stop; Start-Sleep -Milliseconds 300; Invoke-Start }
    'toggle'  {
        $st = Get-State
        $running = $st -and ((Test-Alive $st.ffmpegPid) -or (Test-Alive $st.mediamtxPid))
        if ($running) { Invoke-Stop } else { Invoke-Start }
    }
    'doctor'  { Invoke-Doctor }
    'logs'    { Show-Logs $Tail }
    'pack'    { Invoke-Pack }
}
