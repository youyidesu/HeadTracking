# gen.ps1 —— 生成自签证书（PowerShell 版；cmd 的 ^ 续行在这里用不了）
#
# 用法：
#   cd D:\project\esp32c5_test\main\certs
#   .\gen.ps1
#   cd ..\..
#   python tools\build_web_assets.py
#
# 行为：
#   · server_key.pem 不存在 → 生成（SEC1 格式，mbedTLS 最兼容）
#   · server_key.pem 已存在 → 直接复用，不覆盖
#   · 始终重新生成 server_cert.pem（带 SAN）

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Get-Command openssl -ErrorAction SilentlyContinue)) {
    Write-Host '找不到 openssl：Git 自带的那份在 <Git>\usr\bin\openssl.exe，加进 PATH 再试' -ForegroundColor Red
    exit 1
}
Write-Host ('openssl: ' + (& openssl version)) -ForegroundColor DarkGray

# 1) 私钥（已存在就复用）
if (Test-Path server_key.pem) {
    Write-Host '复用已有的 server_key.pem' -ForegroundColor DarkGray
    $head = (Get-Content server_key.pem -TotalCount 1)
    Write-Host ('  格式: ' + $head) -ForegroundColor DarkGray
    if ($head -match 'BEGIN PRIVATE KEY') {
        Write-Host '  注意：这是 PKCS#8 格式，mbedTLS 一般也能读；若握手失败就删掉重生成' -ForegroundColor Yellow
    }
} else {
    & openssl ecparam -genkey -name prime256v1 -out server_key.pem
    Write-Host '已生成 server_key.pem（EC P-256 / SEC1）' -ForegroundColor Green
}

# 2) 自签证书：整条写一行，最稳；PowerShell 里反引号续行亦可
& openssl req -new -x509 -key server_key.pem -out server_cert.pem -days 3650 -sha256 `
    -subj "/C=CN/O=HT-TURRET/CN=192.168.4.1" `
    -addext "subjectAltName=IP:192.168.4.1,DNS:ht-turret.local" `
    -addext "basicConstraints=critical,CA:TRUE" `
    -addext "keyUsage=critical,digitalSignature,keyEncipherment"

if (-not (Test-Path server_cert.pem)) {
    Write-Host '生成失败：server_cert.pem 没出现' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host '=== 自查 ===' -ForegroundColor Cyan
& openssl x509 -in server_cert.pem -noout -subject -dates
Write-Host '--- SAN（必须含 IP Address:192.168.4.1） ---'
(& openssl x509 -in server_cert.pem -noout -text) |
    Select-String -Pattern 'DNS:|IP Address:|CA:TRUE' | ForEach-Object { Write-Host ('  ' + $_.Line.Trim()) }

Write-Host ''
Write-Host '=== 文件 ===' -ForegroundColor Cyan
Get-ChildItem server_cert.pem, server_key.pem | ForEach-Object {
    Write-Host ('  {0,-18} {1,6} B' -f $_.Name, $_.Length)
}

Write-Host ''
Write-Host '下一步：' -ForegroundColor Yellow
Write-Host '  cd ..\..'
Write-Host '  python tools\build_web_assets.py'
Write-Host '  ninja -C build   （或 IDF 终端里 idf.py build）'
