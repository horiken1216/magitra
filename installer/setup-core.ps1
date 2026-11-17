#Requires -RunAsAdministrator
# setup-core.ps1 - MTP の導入・取り除きの本体（mtpsetup.exe が展開して呼び出す）
#
# 出力の各行は、画面（MtpSetup.cs）が読む。先頭が次のどれか:
#   STEP: 手順の見出し   INFO: 補足   WARN: 注意   DONE: 完了   FAIL: 失敗（終了コード 1）
#
# 導入（-Action Install）
#   1. 動いている常駐アプリと設定画面を終了させる
#   2. 使い捨ての証明書を作り、WinUSB のカタログ（両方の版）と UIAccess 版の常駐アプリ（UIAccess 版のみ）に署名する。
#      署名が済んだらすぐ秘密鍵を消す。信頼されるのは、この導入で署名したファイルだけになる
#   3. UIAccess 版のみ: その証明書（公開鍵だけ）を、この PC の「信頼されたルート」に登録する
#   4. WinUSB を Magic Trackpad の MI_01 に当てる（pnputil）。以前の MTP のドライバーパッケージは取り除く
#   5. C:\Program Files\MTP に常駐アプリ・設定画面・インストーラーをコピーし、「アプリ」の一覧に登録する
#   6. -LockScreenService のとき: サービス MtpSwitch（ロック中・ログオン前だけ標準の HID に戻す）を登録して開始する。
#      付けないときは、以前に登録したサービスを削除する
#   7. 普段のユーザーと同じアカウントで動いていれば、ログオン時の起動の登録先を直し、常駐アプリを起動する
# 取り除き（-Action Uninstall）: 上の逆。ユーザーの設定（HKCU\Software\MTP\Agent）は残す
param(
    [Parameter(Mandatory = $true)][ValidateSet('Install', 'Uninstall')][string]$Action,
    [ValidateSet('UIAccess', 'Standard')][string]$Mode = 'UIAccess',
    [string]$Payload = $PSScriptRoot,
    [string]$SetupExe = '',
    [switch]$RemoveLegacyCertificate,
    [switch]$LockScreenService,
    [switch]$Launch,
    [switch]$AutoStart
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$InstallDir = Join-Path $env:ProgramFiles 'MTP'
$StateKey = 'HKLM:\SOFTWARE\MTP\Installer'
$UninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\MTP'
$CertSubject = 'CN=MTP Local Signing'
$LegacyCertSubject = 'CN=MTP Test Signing'   # 手作業の手順（docs\MANUAL-INSTALL.md）で作った証明書
$DriverInfName = 'mtpusb.inf'
$ServiceName = 'MtpSwitch'
$Version = '0.2.0'

function Step($m) { Write-Output "STEP: $m" }
function Info($m) { Write-Output "INFO: $m" }
function Warn($m) { Write-Output "WARN: $m" }

Add-Type -Namespace MtpSetup -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string name);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
'@

# 常駐アプリにトレイのメニューの「終了」と同じ指示（WM_COMMAND / IDM_EXIT=104）を送り、設定画面は閉じる。
# ランチャー（mtplauncher.exe）は常駐アプリが終わるときに一緒に終わる
function Stop-MtpApps {
    $hwnd = [MtpSetup.Native]::FindWindowW('MtpAgentTrayWindow', 'MTP Agent')
    if ($hwnd -ne [IntPtr]::Zero) {
        [void][MtpSetup.Native]::PostMessageW($hwnd, 0x0111, [IntPtr]104, [IntPtr]::Zero)
        Info '常駐アプリに終了を指示しました'
    }
    Get-Process mtpsettings -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.CloseMainWindow() }
    # サービスも止める（実行ファイルを上書きするため）。止めるだけなので、ドライバーは今のまま
    $svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -ne 'Stopped') {
        Stop-Service $ServiceName -Force
        $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
        Info 'サービス MtpSwitch を止めました'
    }
    for ($i = 0; $i -lt 50; $i++) {
        if (-not (Get-Process mtpagent, mtpsettings, mtplauncher -ErrorAction SilentlyContinue)) { return }
        # 常駐アプリが終わってもランチャーが残っていれば（常駐アプリが知らせる前に落ちていたなど）、止める
        if ($i -ge 30 -and -not (Get-Process mtpagent -ErrorAction SilentlyContinue)) {
            Get-Process mtplauncher -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Milliseconds 100
    }
    throw '常駐アプリか設定画面が終了しませんでした。手動で終了してから、もう一度実行してください。'
}

function Remove-MtpService {
    $svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
    if (-not $svc) { return }
    if ($svc.Status -ne 'Stopped') {
        Stop-Service $ServiceName -Force
        $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }
    & sc.exe delete $ServiceName | Out-Null
    for ($i = 0; $i -lt 50 -and (Get-Service $ServiceName -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
    Info 'サービス MtpSwitch を削除しました'
}

# ロック中・ログオン前だけ標準の HID に戻すサービスを登録して開始する（既にあれば登録し直す）
function Install-MtpService {
    Remove-MtpService
    $bin = '"' + (Join-Path $InstallDir 'mtpswitchsvc.exe') + '"'
    New-Service -Name $ServiceName -BinaryPathName $bin -StartupType Automatic `
        -DisplayName 'MAGITRA ロック中の標準マウス切り替え' `
        -Description 'ロック中とログオン前だけ、Magic Trackpad を Windows 標準のマウスとして使えるようにします（MAGITRA）。' | Out-Null
    # 落ちたら自動で再起動する
    & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/60000 | Out-Null
    Start-Service $ServiceName
    Info 'サービス MtpSwitch を登録して開始しました'
}

# 普段のユーザー（エクスプローラーを動かしているアカウント）と、このスクリプトのアカウントが同じか
function Test-SameUserAsDesktop {
    $me = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $owners = Get-CimInstance Win32_Process -Filter "Name = 'explorer.exe'" |
        ForEach-Object { (Invoke-CimMethod -InputObject $_ -MethodName GetOwnerSid).Sid }
    return ($owners -contains $me)
}

# pnputil /enum-drivers の出力から、元の名前が mtpusb.inf のパッケージの公開名（oemNN.inf）を集める（表示言語に依存しない）
function Get-MtpDriverPackages {
    $text = (& pnputil.exe /enum-drivers) -join "`n"
    $names = @()
    foreach ($block in ($text -split "`n\s*`n")) {
        if ($block -match [regex]::Escape($DriverInfName) -and $block -match '(oem\d+\.inf)') {
            $names += $Matches[1]
        }
    }
    return $names | Select-Object -Unique
}

function Remove-InstallerCertificates {
    $thumbs = @()
    $saved = (Get-ItemProperty $StateKey -ErrorAction SilentlyContinue).CertThumbprint
    if ($saved) { $thumbs += $saved }
    $thumbs += Get-ChildItem Cert:\LocalMachine\Root | Where-Object Subject -eq $CertSubject | ForEach-Object Thumbprint
    foreach ($t in ($thumbs | Select-Object -Unique)) {
        $path = "Cert:\LocalMachine\Root\$t"
        if (Test-Path $path) { Remove-Item $path; Info "信頼されたルートから外しました: $t" }
    }
    # 万一、秘密鍵つきの証明書が残っていたら消す（通常は署名直後に消している）
    Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $CertSubject | ForEach-Object {
        Remove-Item "Cert:\LocalMachine\My\$($_.Thumbprint)" -DeleteKey
    }
}

function Remove-LegacyCertificates {
    foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher', 'Cert:\CurrentUser\My') {
        Get-ChildItem $store -ErrorAction SilentlyContinue | Where-Object Subject -eq $LegacyCertSubject | ForEach-Object {
            if ($store -eq 'Cert:\CurrentUser\My') { Remove-Item "$store\$($_.Thumbprint)" -DeleteKey }
            else { Remove-Item "$store\$($_.Thumbprint)" }
            Info "旧手順の証明書を取り除きました: $store ($($_.Thumbprint))"
        }
    }
}

function Install-Mtp {
    $agentSource = if ($Mode -eq 'UIAccess') { 'mtpagent-uiaccess.exe' } else { 'mtpagent-standard.exe' }
    foreach ($f in @($agentSource, 'mtpsettings.exe', 'mtplauncher.exe', 'MtpUsb.inf', 'mtpusb.cat', 'mtpswitchsvc.exe', 'mtpswitch.exe')) {
        if (-not (Test-Path (Join-Path $Payload $f))) { throw "導入するファイルが見つかりません: $f" }
    }

    Step '常駐アプリと設定画面を終了しています'
    Stop-MtpApps

    Step '作業用フォルダーにファイルを用意しています'
    $stage = Join-Path $env:TEMP ('mtpsetup-stage-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item (Join-Path $Payload $agentSource) (Join-Path $stage 'mtpagent.exe')
    Copy-Item (Join-Path $Payload 'mtpsettings.exe') (Join-Path $stage 'mtpsettings.exe')
    Copy-Item (Join-Path $Payload 'MtpUsb.inf') (Join-Path $stage 'MtpUsb.inf')
    Copy-Item (Join-Path $Payload 'mtpusb.cat') (Join-Path $stage 'mtpusb.cat')

    try {
        Step '以前の MAGITRA の証明書を片付けています'
        Remove-InstallerCertificates
        if ($RemoveLegacyCertificate) { Remove-LegacyCertificates }

        Step '使い捨ての証明書を作って署名しています'
        # タイムスタンプを付けられないため、証明書が切れると署名も無効になる。秘密鍵は消すので、期限を長くとる
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\LocalMachine\My `
            -KeyUsage DigitalSignature -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(20)
        try {
            $toSign = @('mtpusb.cat')
            if ($Mode -eq 'UIAccess') { $toSign += 'mtpagent.exe' }
            foreach ($f in $toSign) {
                $r = Set-AuthenticodeSignature -FilePath (Join-Path $stage $f) -Certificate $cert -HashAlgorithm SHA256
                if ($r.SignerCertificate -eq $null -or $r.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
                    throw "署名できませんでした: $f（$($r.StatusMessage)）"
                }
                Info "署名しました: $f"
            }
            $cerFile = Join-Path $stage 'MtpLocalSigning.cer'
            Export-Certificate -Cert $cert -FilePath $cerFile | Out-Null
        }
        finally {
            Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
            Info '署名に使った秘密鍵を消しました（今後この証明書では何にも署名できません）'
        }

        if ($Mode -eq 'UIAccess') {
            Step 'この PC で証明書を信頼させています（UIAccess に必要）'
            Import-Certificate -FilePath $cerFile -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
            $check = Get-AuthenticodeSignature (Join-Path $stage 'mtpagent.exe')
            if ($check.Status -ne 'Valid') { throw "署名を確かめられませんでした: $($check.StatusMessage)" }
            Info "署名を確かめました（証明書 $($cert.Thumbprint)）"
        }

        Step 'WinUSB を Magic Trackpad に当てています'
        $before = @(Get-MtpDriverPackages)
        $out = & pnputil.exe /add-driver (Join-Path $stage 'MtpUsb.inf') /install
        $code = $LASTEXITCODE
        $out | ForEach-Object { if ($_.Trim()) { Info $_.Trim() } }
        # 0 = 成功、259 = 追加したが今は当てる機器がない、3010 = 再起動が必要
        if ($code -ne 0 -and $code -ne 259 -and $code -ne 3010) { throw "ドライバーを導入できませんでした（pnputil の終了コード $code）" }
        if ($code -eq 259) { Warn 'トラックパッドが USB でつながっていないため、ドライバーは登録だけしました。つなぐと自動で当たります' }
        if ($code -eq 3010) { Warn '再起動が必要です' }
        $published = ($out -join "`n") | Select-String -Pattern 'oem\d+\.inf' | ForEach-Object { $_.Matches[0].Value } | Select-Object -First 1
        foreach ($old in $before) {
            if ($old -ne $published) {
                & pnputil.exe /delete-driver $old | Out-Null
                Info "以前のドライバーパッケージを取り除きました: $old"
            }
        }

        Step "$InstallDir にコピーしています"
        New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
        Copy-Item (Join-Path $stage 'mtpagent.exe') (Join-Path $InstallDir 'mtpagent.exe') -Force
        Copy-Item (Join-Path $stage 'mtpsettings.exe') (Join-Path $InstallDir 'mtpsettings.exe') -Force
        Copy-Item (Join-Path $Payload 'mtplauncher.exe') (Join-Path $InstallDir 'mtplauncher.exe') -Force
        # ロック中の切り替えサービスと、困ったときに手動で切り替える道具（mtpswitch hid / winusb）
        Copy-Item (Join-Path $Payload 'mtpswitchsvc.exe') (Join-Path $InstallDir 'mtpswitchsvc.exe') -Force
        Copy-Item (Join-Path $Payload 'mtpswitch.exe') (Join-Path $InstallDir 'mtpswitch.exe') -Force
        if ($SetupExe -and (Test-Path $SetupExe) -and ((Resolve-Path $SetupExe).Path -ne (Join-Path $InstallDir 'mtpsetup.exe'))) {
            Copy-Item $SetupExe (Join-Path $InstallDir 'mtpsetup.exe') -Force
        }

        if ($LockScreenService) {
            Step 'ロック中とログオン前の切り替えサービスを登録しています'
            Install-MtpService
        }
        else {
            Remove-MtpService
        }

        Step '「アプリ」の一覧に登録しています'
        New-Item -Path $StateKey -Force | Out-Null
        Set-ItemProperty $StateKey -Name Mode -Value $Mode
        Set-ItemProperty $StateKey -Name Version -Value $Version
        Set-ItemProperty $StateKey -Name LockScreenService -Value ([int][bool]$LockScreenService) -Type DWord
        if ($Mode -eq 'UIAccess') { Set-ItemProperty $StateKey -Name CertThumbprint -Value $cert.Thumbprint }
        else { Remove-ItemProperty $StateKey -Name CertThumbprint -ErrorAction SilentlyContinue }
        New-Item -Path $UninstallKey -Force | Out-Null
        $setupPath = Join-Path $InstallDir 'mtpsetup.exe'
        Set-ItemProperty $UninstallKey -Name DisplayName -Value 'MAGITRA（Magic Trackpad のドライバーとツール）'
        Set-ItemProperty $UninstallKey -Name DisplayVersion -Value $Version
        Set-ItemProperty $UninstallKey -Name Publisher -Value 'MAGITRA'
        Set-ItemProperty $UninstallKey -Name InstallLocation -Value $InstallDir
        Set-ItemProperty $UninstallKey -Name DisplayIcon -Value (Join-Path $InstallDir 'mtpagent.exe')
        Set-ItemProperty $UninstallKey -Name UninstallString -Value ('"' + $setupPath + '" /uninstall')
        Set-ItemProperty $UninstallKey -Name ModifyPath -Value ('"' + $setupPath + '"')

        $agentPath = Join-Path $InstallDir 'mtpagent.exe'
        if (Test-SameUserAsDesktop) {
            $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
            if ($AutoStart) {
                # 常駐アプリのメニュー「ログオン時に起動」と同じ値（あとからメニューで外せる）
                Set-ItemProperty $run -Name 'MTP Agent' -Value ('"' + $agentPath + '" /autostart')
                Info 'ログオン時に常駐アプリを起動するよう登録しました'
            }
            elseif ((Get-ItemProperty $run -ErrorAction SilentlyContinue).'MTP Agent') {
                Remove-ItemProperty $run -Name 'MTP Agent'
                Info 'ログオン時の起動の登録を外しました'
            }
            if ($Launch) {
                # 管理者権限から直接起動すると常駐アプリも管理者権限になるので、エクスプローラー経由で普通の権限で起動する
                Start-Process explorer.exe -ArgumentList ('"' + $agentPath + '"')
                Info '常駐アプリを起動しました'
            }
        }
        else {
            Warn 'このインストーラーは、普段とは別の管理者アカウントで動いています。'
            Warn "常駐アプリの起動とログオン時の起動の設定は、普段の画面で行ってください（$agentPath）"
        }
        Write-Output "DONE: 導入しました（$(if ($Mode -eq 'UIAccess') { 'UIAccess 版' } else { '標準版' })）"
    }
    finally {
        Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Uninstall-Mtp {
    Step '常駐アプリと設定画面を終了しています'
    Stop-MtpApps

    Step '切り替えサービスを削除しています'
    Remove-MtpService

    Step 'WinUSB を外して、Windows 標準のドライバーに戻しています'
    foreach ($pkg in @(Get-MtpDriverPackages)) {
        & pnputil.exe /delete-driver $pkg /uninstall /force | Out-Null
        Info "ドライバーパッケージを取り除きました: $pkg"
    }
    Info 'USB ケーブルを抜き差しすると、確実に標準のドライバーに戻ります'

    Step '証明書の信頼を外しています'
    Remove-InstallerCertificates
    if ($RemoveLegacyCertificate) { Remove-LegacyCertificates }

    Step 'ログオン時の起動の登録を外しています'
    if (Test-SameUserAsDesktop) {
        $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
        $v = (Get-ItemProperty $run -ErrorAction SilentlyContinue).'MTP Agent'
        if ($v) { Remove-ItemProperty $run -Name 'MTP Agent'; Info '外しました' }
    }
    else {
        Warn '普段とは別の管理者アカウントで動いているため、ログオン時の起動の登録は外していません（トレイのメニューで外せます）'
    }

    Step "$InstallDir を削除しています"
    foreach ($name in 'mtpagent.exe', 'mtpsettings.exe', 'mtplauncher.exe', 'mtpswitchsvc.exe', 'mtpswitch.exe') {
        $p = Join-Path $InstallDir $name
        if (Test-Path $p) { Remove-Item $p -Force }
    }
    Remove-Item $UninstallKey -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKLM:\SOFTWARE\MTP' -Recurse -Force -ErrorAction SilentlyContinue
    # mtpsetup.exe は動いている自分自身のことがあるので、終了後に消す
    $setup = Join-Path $InstallDir 'mtpsetup.exe'
    if (Test-Path $setup) {
        Start-Process cmd.exe -WindowStyle Hidden -ArgumentList ('/c ping -n 4 127.0.0.1 >nul & del /f /q "' + $setup + '" & rmdir "' + $InstallDir + '"')
        Info 'インストーラーは終了後に削除します'
    }
    elseif ((Test-Path $InstallDir) -and -not (Get-ChildItem $InstallDir)) {
        Remove-Item $InstallDir -Force
    }
    Info '設定（ポインターの速さなど）は残しています'
    Write-Output 'DONE: 取り除きました'
}

try {
    if ($Action -eq 'Install') { Install-Mtp } else { Uninstall-Mtp }
    exit 0
}
catch {
    Write-Output "FAIL: $($_.Exception.Message)"
    exit 1
}
