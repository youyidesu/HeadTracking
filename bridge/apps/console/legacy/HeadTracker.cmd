@echo off
chcp 65001 >nul
setlocal
set PS=pwsh
where pwsh >nul 2>nul || set PS=powershell

if /i "%~1"=="cli" (
  shift
  %PS% -NoProfile -ExecutionPolicy Bypass -File "%~dp0videobridge.ps1" %*
  goto :end
)

:: 默认开 GUI 控制台
start "" %PS% -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0HeadTracker.ps1"

:end
endlocal
