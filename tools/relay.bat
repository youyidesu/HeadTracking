@echo off
chcp 65001 >nul
title HT TURRET 图传转接
cd /d "%~dp0"
echo ============================================
echo  HT TURRET 图传转接
echo   - 自动选视频源 / 起服务 / 建反向隧道
echo   - 想换视频源：关掉窗口后运行 relay.py --pick
echo ============================================
echo.
python relay.py %*
if errorlevel 1 pause
pause