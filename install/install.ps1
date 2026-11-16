#Requires -RunAsAdministrator
# install.ps1 - MTP を C:\Program Files\MTP に導入する（UIAccess 版の常駐アプリと設定画面）
#
# UIAccess 版は、管理者権限で動いているウィンドウにも入力を送れる。Windows がこれを起動するのは、
# 信頼された証明書で署名され、Program Files などの保護されたフォルダーに置かれているときだけ。
#
# 前提:
#   - agent\build.cmd と settings\build.cmd を実行済み（build\agent\install\mtpagent.exe と build\agent\mtpsettings.exe がある）
#   - docs\INSTALL-USB.md の手順 2 で作った証明書「MTP Test Signing」が、この管理者権限の PowerShell の
#     ユーザーの証明書ストア（Cert:\CurrentUser\My）にある
#   - その証明書が、この PC の「信頼されたルート」に登録済み（docs\INSTALL-USB.md の手順 3）。
#     WinUSB の導入には要らなかったが、UIAccess には必須
#   - 常駐アプリをトレイのメニューの「終了」で止めてある
#
# このスクリプトがすること: 署名と確認、C:\Program Files\MTP へのコピーだけ。
# ログオン時の起動の登録や常駐アプリの起動は、普段の画面で行う（最後に案内を出す）。
#
# 使い方（管理者権限の PowerShell で）: powershell -ExecutionPolicy Bypass -File install\install.ps1
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$src = Join-Path $repo 'build\agent'
$dst = Join-Path $env:ProgramFiles 'MTP'
$signtool = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$certName = 'MTP Test Signing'
$agentSrc = Join-Path $src 'install\mtpagent.exe'
$settingsSrc = Join-Path $src 'mtpsettings.exe'

# ---- 事前の確認 ----
if (Get-Process mtpagent -ErrorAction SilentlyContinue) {
    throw '常駐アプリ（MTP Agent）が動いています。トレイのメニューの「終了」で止めてから、もう一度実行してください。'
}
foreach ($f in @($agentSrc, $settingsSrc)) {
    if (-not (Test-Path $f)) { throw "見つかりません: $f 。先に agent\build.cmd と settings\build.cmd を実行してください。" }
}
if (-not (Test-Path $signtool)) { throw "signtool が見つかりません: $signtool" }
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq "CN=$certName" | Select-Object -First 1
if (-not $cert) {
    throw "署名用の証明書「$certName」が、このユーザー（$env:USERNAME）の証明書ストアに見つかりません。" +
        "docs\INSTALL-USB.md の手順 2 を、この管理者権限の PowerShell で実行したか確かめてください。"
}
if (-not (Get-ChildItem Cert:\LocalMachine\Root | Where-Object Thumbprint -eq $cert.Thumbprint)) {
    throw "証明書「$certName」が、この PC の「信頼されたルート」に登録されていません。UIAccess にはこの登録が必要です。" +
        "docs\INSTALL-USB.md の手順 3（certutil -addstore Root ...）を先に実行してください。"
}

# ---- 1. 署名して確かめる ----
& $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My $agentSrc
if ($LASTEXITCODE -ne 0) { throw '署名に失敗しました。' }
& $signtool verify /pa $agentSrc
if ($LASTEXITCODE -ne 0) { throw '署名を確かめられませんでした（証明書が信頼されていない可能性があります）。' }

# ---- 2. Program Files へコピーする ----
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item $agentSrc (Join-Path $dst 'mtpagent.exe') -Force
Copy-Item $settingsSrc (Join-Path $dst 'mtpsettings.exe') -Force
Write-Host "導入しました: $dst"

# ---- この後の操作は、普段の画面（管理者権限ではない）で行う ----
# 管理者権限の PowerShell は、普段とは別の管理者アカウントで動いていることがある。
# その場合、ここでログオン時の起動の登録（ユーザーごとの設定）を変えたり常駐アプリを起動したりすると、
# 別のアカウントに対して行われてしまうので、スクリプトではしない。
Write-Host ''
Write-Host 'この後は、普段の画面で次を行ってください:'
Write-Host "  1. エクスプローラーで $dst\mtpagent.exe をダブルクリックして起動する（管理者として実行はしない）"
Write-Host '  2. ログオン時に起動を使う場合は、トレイのメニューの「ログオン時に起動」を一度クリックする'
Write-Host '     （登録先が新しい場所に変わる。チェックが付いていれば完了）'
Write-Host '今後は C:\Program Files\MTP\mtpagent.exe を使ってください（build\agent\mtpagent.exe は開発用）。'
