#Requires -RunAsAdministrator
# uninstall.ps1 - install.ps1 で C:\Program Files\MAGITRA に入れたものを取り除く
#
# 取り除くのは C:\Program Files\MAGITRA の中身と、そこを指すログオン時の起動の登録だけ。
# WinUSB の導入・証明書・設定（HKCU\Software\MAGITRA\Agent）は変えない（docs\MANUAL-INSTALL.md の「元に戻す」を参照）。
#
# 使い方（管理者権限の PowerShell で）: powershell -ExecutionPolicy Bypass -File tools\manual-install\uninstall.ps1
$ErrorActionPreference = 'Stop'

$dst = Join-Path $env:ProgramFiles 'MAGITRA'

if (Get-Process mtpagent, mtpsettings -ErrorAction SilentlyContinue) {
    throw '常駐アプリか設定画面が動いています。終了してから、もう一度実行してください。'
}

$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$current = (Get-ItemProperty $run -ErrorAction SilentlyContinue).'MAGITRA'
if ($current -and $current -like "*$dst*") {
    Remove-ItemProperty $run -Name 'MAGITRA'
    Write-Host 'ログオン時の起動の登録を外しました。'
}

foreach ($name in @('mtpagent.exe', 'mtpsettings.exe')) {
    $path = Join-Path $dst $name
    if (Test-Path $path) {
        Remove-Item $path -Force
        Write-Host "削除しました: $path"
    }
}
if ((Test-Path $dst) -and -not (Get-ChildItem $dst)) {
    Remove-Item $dst -Force
    Write-Host "フォルダーを削除しました: $dst"
}
Write-Host '取り除きました。'
