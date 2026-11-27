#Requires -RunAsAdministrator
# setup-core.ps1 - MAGITRA の導入・取り除きの本体（mtpsetup.exe が展開して呼び出す）
#
# 出力の各行は、画面（MtpSetup.cs）が読む。先頭が次のどれか:
#   STEP: 手順の見出し   INFO: 補足   WARN: 注意   DONE: 完了   FAIL: 失敗（終了コード 1）
#
# 導入（-Action Install）
#   1. 動いている常駐アプリと設定画面を終了させる
#   2. 使い捨ての証明書を作り、WinUSB のカタログ（両方の版）と UIAccess 版の常駐アプリ（UIAccess 版のみ）に署名する。
#      署名が済んだらすぐ秘密鍵を消す。信頼されるのは、この導入で署名したファイルだけになる
#   3. UIAccess 版のみ: その証明書（公開鍵だけ）を、この PC の「信頼されたルート」に登録する
#   4. WinUSB を Magic Trackpad の MI_01 に当てる（pnputil）
#   5. C:\Program Files\MAGITRA に常駐アプリ・設定画面・インストーラーをコピーし、「アプリ」の一覧に登録する
#   6. -LockScreenService のとき: サービス MagitraSwitch（ロック中・ログオン前だけ標準の HID に戻す）を登録して開始する。
#      付けないときは、以前に登録したサービスを削除する
#   4b. -Bluetooth のとき（詳細設定。テスト署名モードの PC だけ）: MtpBth.sys とカタログにも同じ証明書で署名し、
#      証明書を「信頼されたルート」と「信頼された発行元」に登録して、Bluetooth の Magic Trackpad に MtpBth を当てる。
#      付けないときは、以前に入れた MtpBth（spike.ps1 で入れたものも含む）を取り除き、Windows 標準の HidBth に戻す
#   7. 以前の MAGITRA のドライバーパッケージと証明書を片付ける（新しいものが揃うまでは残す）
#   8. 普段のユーザーと同じアカウントで動いていれば、ログオン時の起動の登録先を直し、常駐アプリを起動する
#   5 より前に失敗したら、この導入で加えたドライバーと証明書を戻す（以前の状態のままになる）
# 取り除き（-Action Uninstall）: 上の逆。ユーザーの設定（HKCU\Software\MAGITRA\Agent）は残す。
#   WinUSB を外せなかったら、そこで止めてファイル・証明書・登録は残す（トラックパッドを使う手段をなくさないため）
param(
    [Parameter(Mandatory = $true)][ValidateSet('Install', 'Uninstall')][string]$Action,
    [ValidateSet('UIAccess', 'Standard')][string]$Mode = 'UIAccess',
    [string]$Payload = $PSScriptRoot,
    [string]$SetupExe = '',
    [switch]$RemoveLegacyCertificate,
    [switch]$LockScreenService,
    [switch]$Bluetooth,
    [switch]$Launch,
    [switch]$AutoStart
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$InstallDir = Join-Path $env:ProgramFiles 'MAGITRA'
$StateKey = 'HKLM:\SOFTWARE\MAGITRA\Installer'
$UninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\MAGITRA'
$CertSubject = 'CN=MAGITRA Local Signing'
$LegacyCertSubject = 'CN=MAGITRA Test Signing'   # 手作業の手順（docs\MANUAL-INSTALL.md）で作った証明書
$DriverInfName = 'mtpusb.inf'
$BthInfName = 'mtpbth.inf'
$BthHardwareIds = @(
    'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0265',
    'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0324')
$ServiceName = 'MagitraSwitch'
$Version = '0.4.0'

function Step($m) { Write-Output "STEP: $m" }
function Info($m) { Write-Output "INFO: $m" }
function Warn($m) { Write-Output "WARN: $m" }

Add-Type -Namespace MtpSetup -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string name);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
[DllImport("ntdll.dll")] public static extern int NtQuerySystemInformation(int infoClass, byte[] buffer, int length, out int returned);
[DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId, string infPath, uint flags, out bool reboot);
'@

# 自己署名のカーネルドライバー（MtpBth）が読み込まれる起動か。SystemCodeIntegrityInformation（103）で、
# TESTSIGN（0x02）が立っている＝テスト署名モード、ENABLED（0x01）が落ちている＝署名の強制が無効
function Test-TestSigning {
    $buf = [byte[]]::new(8)
    [BitConverter]::GetBytes([int]8).CopyTo($buf, 0)
    $ret = 0
    if ([MtpSetup.Native]::NtQuerySystemInformation(103, $buf, 8, [ref]$ret) -ne 0) { return $false }
    $opt = [BitConverter]::ToUInt32($buf, 4)
    return (($opt -band 0x02) -ne 0) -or (($opt -band 0x01) -eq 0)
}

# ドライバーストアに入れた MtpBth（%windir%\INF\oemNN.inf）を、ペアリング済みの Magic Trackpad に当てる。
# 標準の HidBth より優先されないことがあるので強制で当てる。当てた機器があれば $script:BthApplied を $true にする
# （Info の行も出力に流れるので、結果は戻り値ではなく変数で返す）
function Set-BluetoothDriver([string]$PublishedInf) {
    $inf = Join-Path $env:windir "INF\$PublishedInf"
    $applied = $false
    foreach ($id in $BthHardwareIds) {
        $reboot = $false
        # 1 = INSTALLFLAG_FORCE
        if ([MtpSetup.Native]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $id, $inf, 1, [ref]$reboot)) {
            Info "MtpBth を当てました: $id"
            if ($reboot) { Warn '再起動が必要です' }
            $applied = $true
        }
    }
    $script:BthApplied = $applied
}

# MtpBth のパッケージを取り除く（当たっている機器は Windows 標準の HidBth に戻る）。
# 取り除けなかった公開名を $script:BthRemoveFailed に入れる（Info の行も出力に流れるので、戻り値にはしない）
function Remove-BluetoothDriver {
    $failed = @()
    foreach ($pkg in @(Get-MtpDriverPackages $BthInfName)) {
        $out = & pnputil.exe /delete-driver $pkg /uninstall /force
        if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 3010) {
            Info "Bluetooth のドライバーパッケージを取り除きました: $pkg"
            if ($LASTEXITCODE -eq 3010) { Warn '再起動が必要です' }
        }
        else {
            $out | ForEach-Object { if ($_.Trim()) { Info $_.Trim() } }
            $failed += $pkg
        }
    }
    $script:BthRemoveFailed = $failed
}

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
        Info 'サービス MagitraSwitch を止めました'
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
    $out = & sc.exe delete $ServiceName
    # 1072 = 削除の予約済み（ハンドルが閉じれば消える）
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1072) {
        throw "サービス $ServiceName を削除できませんでした（sc.exe の終了コード $LASTEXITCODE）: $($out -join ' ')"
    }
    for ($i = 0; $i -lt 50 -and (Get-Service $ServiceName -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
    if (Get-Service $ServiceName -ErrorAction SilentlyContinue) {
        Warn "サービス $ServiceName は削除を予約しました（再起動すると消えます）"
    }
    else {
        Info "サービス $ServiceName を削除しました"
    }
}

# サービスの記録のフォルダー（%ProgramData%\MAGITRA）を、管理者と SYSTEM だけが書けるように用意する。
# %ProgramData% の既定の権限では一般ユーザーも中にファイルを作れるため、SYSTEM が書く記録を差し替えられないよう継承を切る。
# 先に一般ユーザーが作っていた（所有者が SYSTEM・Administrators でない）か、リンクになっていれば、消して作り直す
$LogDirSddl = 'D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)'
function Initialize-LogDirectory {
    $dir = Join-Path $env:ProgramData 'MAGITRA'
    $item = Get-Item -LiteralPath $dir -Force -ErrorAction SilentlyContinue
    if ($item) {
        $owner = (Get-Acl -LiteralPath $dir).GetOwner([System.Security.Principal.SecurityIdentifier]).Value
        $isLink = ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
        if ($isLink -or -not $item.PSIsContainer -or ($owner -ne 'S-1-5-18' -and $owner -ne 'S-1-5-32-544')) {
            # rmdir はリンクの先をたどらない
            & cmd.exe /c rmdir /s /q "`"$dir`"" 2>$null
            if (Test-Path -LiteralPath $dir) { & cmd.exe /c del /f /q "`"$dir`"" 2>$null }
            if (Test-Path -LiteralPath $dir) { throw "記録のフォルダーを作り直せませんでした: $dir" }
            Warn "管理者以外が作った記録のフォルダーを消して、作り直しました: $dir"
            $item = $null
        }
    }
    $sec = New-Object System.Security.AccessControl.DirectorySecurity
    $sec.SetSecurityDescriptorSddlForm($LogDirSddl)
    if ($item) { $item.SetAccessControl($sec) }
    else { [void][System.IO.Directory]::CreateDirectory($dir, $sec) }
}

# ロック中・ログオン前だけ標準の HID に戻すサービスを登録して開始する（既にあれば登録し直す）
function Install-MtpService {
    Remove-MtpService
    Initialize-LogDirectory
    $bin = '"' + (Join-Path $InstallDir 'mtpswitchsvc.exe') + '"'
    New-Service -Name $ServiceName -BinaryPathName $bin -StartupType Automatic `
        -DisplayName 'MAGITRA ロック中の標準マウス切り替え' `
        -Description 'ロック中とログオン前だけ、Magic Trackpad を Windows 標準のマウスとして使えるようにします（MAGITRA）。' | Out-Null
    # 落ちたら自動で再起動する
    & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/60000 | Out-Null
    Start-Service $ServiceName
    Info 'サービス MagitraSwitch を登録して開始しました'
}

# 普段のユーザー（エクスプローラーを動かしているアカウント）と、このスクリプトのアカウントが同じか
function Test-SameUserAsDesktop {
    $me = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $owners = Get-CimInstance Win32_Process -Filter "Name = 'explorer.exe'" |
        ForEach-Object { (Invoke-CimMethod -InputObject $_ -MethodName GetOwnerSid).Sid }
    return ($owners -contains $me)
}

# pnputil /enum-drivers の出力から、元の名前が $InfName（既定は mtpusb.inf）のパッケージの公開名（oemNN.inf）を集める（表示言語に依存しない）
function Get-MtpDriverPackages([string]$InfName = $DriverInfName) {
    $text = (& pnputil.exe /enum-drivers) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "ドライバーの一覧を読めませんでした（pnputil の終了コード $LASTEXITCODE）" }
    $names = @()
    foreach ($block in ($text -split "`n\s*`n")) {
        if ($block -match [regex]::Escape($InfName) -and $block -match '(oem\d+\.inf)') {
            $names += $Matches[1]
        }
    }
    return $names | Select-Object -Unique
}

# $Keep に渡した拇印の証明書は残す（導入し直したときの新しい証明書）
function Remove-InstallerCertificates([string]$Keep = '', [string[]]$Also = @()) {
    $thumbs = @($Also)
    $saved = (Get-ItemProperty $StateKey -ErrorAction SilentlyContinue).CertThumbprint
    if ($saved) { $thumbs += $saved }
    $thumbs += Get-ChildItem Cert:\LocalMachine\Root | Where-Object Subject -eq $CertSubject | ForEach-Object Thumbprint
    # Bluetooth のドライバー（詳細設定）を入れたときは「信頼された発行元」にも登録している
    $thumbs += Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object Subject -eq $CertSubject | ForEach-Object Thumbprint
    foreach ($t in ($thumbs | Where-Object { $_ -and $_ -ne $Keep } | Select-Object -Unique)) {
        $path = "Cert:\LocalMachine\Root\$t"
        if (Test-Path $path) { Remove-Item $path; Info "信頼されたルートから外しました: $t" }
        $path = "Cert:\LocalMachine\TrustedPublisher\$t"
        if (Test-Path $path) { Remove-Item $path; Info "信頼された発行元から外しました: $t" }
    }
    # 万一、秘密鍵つきの証明書が残っていたら消す（通常は署名直後に消している）
    Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $CertSubject -and $_.Thumbprint -ne $Keep } | ForEach-Object {
        Remove-Item "Cert:\LocalMachine\My\$($_.Thumbprint)" -DeleteKey
    }
}

function Remove-LegacyCertificates {
    # LocalMachine\My は drivers\spike.ps1（Bluetooth の開発用）が秘密鍵付きで作る場所
    foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher', 'Cert:\CurrentUser\My', 'Cert:\LocalMachine\My') {
        Get-ChildItem $store -ErrorAction SilentlyContinue | Where-Object Subject -eq $LegacyCertSubject | ForEach-Object {
            if ($store -like '*\My') { Remove-Item "$store\$($_.Thumbprint)" -DeleteKey }
            else { Remove-Item "$store\$($_.Thumbprint)" }
            Info "旧手順の証明書を取り除きました: $store ($($_.Thumbprint))"
        }
    }
}

function Install-Mtp {
    $agentSource = if ($Mode -eq 'UIAccess') { 'mtpagent-uiaccess.exe' } else { 'mtpagent-standard.exe' }
    $files = @($agentSource, 'mtpsettings.exe', 'mtplauncher.exe', 'MtpUsb.inf', 'mtpusb.cat', 'mtpswitchsvc.exe', 'mtpswitch.exe')
    if ($Bluetooth) { $files += 'MtpBth.inf', 'MtpBth.sys', 'mtpbth.cat' }
    foreach ($f in $files) {
        if (-not (Test-Path (Join-Path $Payload $f))) { throw "導入するファイルが見つかりません: $f" }
    }
    # 何かを変える前に確かめる。テスト署名モードへの切り替えは利用者が自分で行う（インストーラーは起動の設定を変えない）
    if ($Bluetooth -and -not (Test-TestSigning)) {
        throw 'Bluetooth 接続には、この PC がテスト署名モードで起動している必要があります。何も変更していません'
    }
    # 証明書を「信頼されたルート」に入れるのは、UIAccess 版か Bluetooth を選んだとき
    $trustRoot = ($Mode -eq 'UIAccess') -or $Bluetooth

    Step '常駐アプリと設定画面を終了しています'
    Stop-MtpApps

    Step '作業用フォルダーにファイルを用意しています'
    # 署名するファイルは、管理者と SYSTEM だけが書ける展開先（mtpsetup.exe が用意する $Payload）の中で扱う
    $stage = Join-Path $Payload ('stage-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item (Join-Path $Payload $agentSource) (Join-Path $stage 'mtpagent.exe')
    Copy-Item (Join-Path $Payload 'mtpsettings.exe') (Join-Path $stage 'mtpsettings.exe')
    Copy-Item (Join-Path $Payload 'MtpUsb.inf') (Join-Path $stage 'MtpUsb.inf')
    Copy-Item (Join-Path $Payload 'mtpusb.cat') (Join-Path $stage 'mtpusb.cat')
    if ($Bluetooth) {
        # INF とカタログと .sys を同じフォルダーに置く（pnputil はカタログの中のハッシュで確かめる）
        $bthStage = Join-Path $stage 'MtpBth'
        New-Item -ItemType Directory -Path $bthStage | Out-Null
        foreach ($f in 'MtpBth.inf', 'MtpBth.sys', 'mtpbth.cat') { Copy-Item (Join-Path $Payload $f) (Join-Path $bthStage $f) }
    }

    # 途中で失敗したときに戻すための記録。以前の証明書・ドライバー・ファイルは、新しいものが揃うまで残す
    $oldThumb = (Get-ItemProperty $StateKey -ErrorAction SilentlyContinue).CertThumbprint
    $cert = $null
    $trustedNew = $false
    $trustedPublisherNew = $false
    $published = $null
    $bthPublished = $null
    $filesTouched = $false
    try {
        Step '使い捨ての証明書を作って署名しています'
        # タイムスタンプを付けられないため、証明書が切れると署名も無効になる。秘密鍵は消すので、期限を長くとる
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\LocalMachine\My `
            -KeyUsage DigitalSignature -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(20)
        try {
            $toSign = @('mtpusb.cat')
            if ($Mode -eq 'UIAccess') { $toSign += 'mtpagent.exe' }
            if ($Bluetooth) { $toSign += 'MtpBth\MtpBth.sys', 'MtpBth\mtpbth.cat' }
            foreach ($f in $toSign) {
                $r = Set-AuthenticodeSignature -FilePath (Join-Path $stage $f) -Certificate $cert -HashAlgorithm SHA256
                if ($r.SignerCertificate -eq $null -or $r.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
                    throw "署名できませんでした: $f（$($r.StatusMessage)）"
                }
                Info "署名しました: $f"
            }
            $cerFile = Join-Path $stage 'MagitraLocalSigning.cer'
            Export-Certificate -Cert $cert -FilePath $cerFile | Out-Null
        }
        finally {
            Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
            Info '署名に使った秘密鍵を消しました（今後この証明書では何にも署名できません）'
        }

        if ($trustRoot) {
            $why = if ($Mode -eq 'UIAccess' -and $Bluetooth) { 'UIAccess と Bluetooth のドライバー' }
                   elseif ($Bluetooth) { 'Bluetooth のドライバー' } else { 'UIAccess' }
            Step "この PC で証明書を信頼させています（$why に必要）"
            Import-Certificate -FilePath $cerFile -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
            $trustedNew = $true
            if ($Bluetooth) {
                # 自己署名のドライバーパッケージを、確認の画面なしで入れるために「信頼された発行元」にも登録する
                Import-Certificate -FilePath $cerFile -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
                $trustedPublisherNew = $true
            }
            $checkFile = if ($Mode -eq 'UIAccess') { 'mtpagent.exe' } else { 'MtpBth\MtpBth.sys' }
            $check = Get-AuthenticodeSignature (Join-Path $stage $checkFile)
            if ($check.Status -ne 'Valid') { throw "署名を確かめられませんでした: $($check.StatusMessage)" }
            Info "署名を確かめました（証明書 $($cert.Thumbprint)）"
        }

        Step 'WinUSB を Magic Trackpad に当てています'
        $before = @(Get-MtpDriverPackages)
        $out = & pnputil.exe /add-driver (Join-Path $stage 'MtpUsb.inf') /install
        $code = $LASTEXITCODE
        $out | ForEach-Object { if ($_.Trim()) { Info $_.Trim() } }
        # この導入で加わったパッケージ（失敗したときに戻すため、終了コードを見る前に調べる）
        $published = @(Get-MtpDriverPackages) | Where-Object { $before -notcontains $_ } | Select-Object -First 1
        if (-not $published) {
            $published = ($out -join "`n") | Select-String -Pattern 'oem\d+\.inf' | ForEach-Object { $_.Matches[0].Value } | Select-Object -First 1
        }
        # 0 = 成功、259 = 追加したが今は当てる機器がない、3010 = 再起動が必要
        if ($code -ne 0 -and $code -ne 259 -and $code -ne 3010) { throw "ドライバーを導入できませんでした（pnputil の終了コード $code）" }
        if ($code -eq 259) { Warn 'トラックパッドが USB でつながっていないため、ドライバーは登録だけしました。つなぐと自動で当たります' }
        if ($code -eq 3010) { Warn '再起動が必要です' }

        if ($Bluetooth) {
            Step 'Bluetooth のドライバー（MtpBth）を Magic Trackpad に当てています'
            $bthBefore = @(Get-MtpDriverPackages $BthInfName)
            $out = & pnputil.exe /add-driver (Join-Path $bthStage 'MtpBth.inf')
            $code = $LASTEXITCODE
            $out | ForEach-Object { if ($_.Trim()) { Info $_.Trim() } }
            $bthPublished = @(Get-MtpDriverPackages $BthInfName) | Where-Object { $bthBefore -notcontains $_ } | Select-Object -First 1
            if (-not $bthPublished) {
                $bthPublished = ($out -join "`n") | Select-String -Pattern 'oem\d+\.inf' | ForEach-Object { $_.Matches[0].Value } | Select-Object -First 1
            }
            if ($code -ne 0 -and $code -ne 3010) { throw "Bluetooth のドライバーを導入できませんでした（pnputil の終了コード $code）" }
            if (-not $bthPublished) { throw 'Bluetooth のドライバーパッケージの公開名を確かめられませんでした' }
            Set-BluetoothDriver $bthPublished
            if (-not $script:BthApplied) {
                Warn 'ペアリング済みの Magic Trackpad が見つからないため、ドライバーは登録だけしました。ペアリングしてから、もう一度導入してください'
            }
        }

        Step "$InstallDir にコピーしています"
        $filesTouched = $true
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

        # この先で失敗しても「アプリ」の一覧（mtpsetup.exe の「取り除く」）から元に戻せるよう、コピーの直後に登録する
        Step '「アプリ」の一覧に登録しています'
        New-Item -Path $StateKey -Force | Out-Null
        Set-ItemProperty $StateKey -Name Mode -Value $Mode
        Set-ItemProperty $StateKey -Name Version -Value $Version
        Set-ItemProperty $StateKey -Name LockScreenService -Value ([int][bool]$LockScreenService) -Type DWord
        Set-ItemProperty $StateKey -Name Bluetooth -Value ([int][bool]$Bluetooth) -Type DWord
        if ($trustRoot) { Set-ItemProperty $StateKey -Name CertThumbprint -Value $cert.Thumbprint }
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

        if ($LockScreenService) {
            Step 'ロック中とログオン前の切り替えサービスを登録しています'
            Install-MtpService
        }
        else {
            Remove-MtpService
        }

        # 新しいものが揃ったので、以前のドライバーパッケージと証明書を片付ける
        Step '以前のドライバーと証明書を片付けています'
        foreach ($old in $before) {
            if ($old -ne $published) {
                $null = & pnputil.exe /delete-driver $old
                if ($LASTEXITCODE -eq 0) { Info "以前のドライバーパッケージを取り除きました: $old" }
                else { Warn "以前のドライバーパッケージを取り除けませんでした: $old（pnputil の終了コード $LASTEXITCODE）" }
            }
        }
        if ($Bluetooth) {
            foreach ($old in @(Get-MtpDriverPackages $BthInfName)) {
                if ($old -ne $bthPublished) {
                    # 新しいパッケージを強制で当てたので、以前のものはもう使われていない
                    $null = & pnputil.exe /delete-driver $old /force
                    if ($LASTEXITCODE -eq 0) { Info "以前の Bluetooth のドライバーパッケージを取り除きました: $old" }
                    else { Warn "以前の Bluetooth のドライバーパッケージを取り除けませんでした: $old（pnputil の終了コード $LASTEXITCODE）" }
                }
            }
        }
        else {
            # Bluetooth を選ばなかった: 以前に入れた MtpBth（spike.ps1 で入れたものも）を外し、標準の HidBth に戻す
            Remove-BluetoothDriver
            foreach ($p in @($script:BthRemoveFailed)) { Warn "Bluetooth のドライバーパッケージを取り除けませんでした: $p" }
        }
        $keep = if ($trustRoot) { $cert.Thumbprint } else { '' }
        Remove-InstallerCertificates -Keep $keep -Also @($oldThumb)
        if ($RemoveLegacyCertificate) { Remove-LegacyCertificates }

        $agentPath = Join-Path $InstallDir 'mtpagent.exe'
        if (Test-SameUserAsDesktop) {
            $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
            if ($AutoStart) {
                # 常駐アプリのメニュー「ログオン時に起動」と同じ値（あとからメニューで外せる）
                Set-ItemProperty $run -Name 'MAGITRA' -Value ('"' + $agentPath + '" /autostart')
                Info 'ログオン時に常駐アプリを起動するよう登録しました'
            }
            elseif ((Get-ItemProperty $run -ErrorAction SilentlyContinue).'MAGITRA') {
                Remove-ItemProperty $run -Name 'MAGITRA'
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
    catch {
        $err = $_
        if (-not $filesTouched) {
            # ファイルはまだ以前のまま。この導入で加えたドライバーと証明書だけを戻せば、導入前の状態に戻る
            if ($bthPublished) {
                $null = & pnputil.exe /delete-driver $bthPublished /uninstall /force
                if ($LASTEXITCODE -eq 0) { Info "この導入で加えた Bluetooth のドライバーパッケージを取り除きました: $bthPublished" }
                else { Warn "この導入で加えた Bluetooth のドライバーパッケージを取り除けませんでした: $bthPublished（pnputil の終了コード $LASTEXITCODE）" }
            }
            if ($published) {
                $null = & pnputil.exe /delete-driver $published /uninstall /force
                if ($LASTEXITCODE -eq 0) {
                    Info "この導入で加えたドライバーパッケージを取り除きました: $published"
                    if ($before.Count -gt 0) { $null = & pnputil.exe /scan-devices }
                }
                else { Warn "この導入で加えたドライバーパッケージを取り除けませんでした: $published（pnputil の終了コード $LASTEXITCODE）" }
            }
            if ($trustedNew -and $cert) {
                try {
                    Remove-Item "Cert:\LocalMachine\Root\$($cert.Thumbprint)" -ErrorAction Stop
                    Info "この導入で信頼させた証明書を外しました: $($cert.Thumbprint)"
                }
                catch { Warn "この導入で信頼させた証明書を外せませんでした: $($cert.Thumbprint)" }
            }
            if ($trustedPublisherNew -and $cert) {
                try { Remove-Item "Cert:\LocalMachine\TrustedPublisher\$($cert.Thumbprint)" -ErrorAction Stop }
                catch { Warn "この導入で信頼された発行元に登録した証明書を外せませんでした: $($cert.Thumbprint)" }
            }
            Warn '途中で止まったため、この導入で加えた変更を戻しました。以前の状態のままです'
        }
        else {
            # ファイルを置き換え始めた後は、以前と新しい証明書の両方を信頼したまま残す（どちらの exe も起動できるように）
            Warn '新しいファイルを置いた後で止まりました。もう一度導入し直すか、「取り除く」で元に戻してください'
            Warn "すぐに標準のマウスに戻すには、管理者権限で $(Join-Path $InstallDir 'mtpswitch.exe') hid を実行します"
        }
        throw $err
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

    Step 'Bluetooth のドライバー（MtpBth）を外しています'
    $hadBth = @(Get-MtpDriverPackages $BthInfName).Count -gt 0
    Remove-BluetoothDriver
    $failedBth = @($script:BthRemoveFailed)
    if ($hadBth -and $failedBth.Count -eq 0) {
        # 切り替えで Bluetooth の接続が切れる。標準のドライバーには自分からつなぎ直す仕組みがない（実機、2026-11-18）
        Info 'Bluetooth のトラックパッドは、1 回クリックすると Windows 標準のマウスとしてつながり直します'
    }
    if ($failedBth.Count -gt 0) {
        # 署名の信頼を外すと、残った MtpBth が使えなくなる。ここで止めて、ほかは残す
        throw "Bluetooth のドライバーパッケージを取り除けませんでした（$($failedBth -join ', ')）。" +
            "ファイル・証明書・「アプリ」の一覧の登録は残しています。再起動してから、もう一度「取り除く」を実行してください"
    }

    Step 'WinUSB を外して、Windows 標準のドライバーに戻しています'
    $failedPkgs = @()
    foreach ($pkg in @(Get-MtpDriverPackages)) {
        $out = & pnputil.exe /delete-driver $pkg /uninstall /force
        # 3010 = 取り除いたが再起動が必要
        if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 3010) {
            Info "ドライバーパッケージを取り除きました: $pkg"
            if ($LASTEXITCODE -eq 3010) { Warn '再起動が必要です' }
        }
        else {
            $out | ForEach-Object { if ($_.Trim()) { Info $_.Trim() } }
            $failedPkgs += $pkg
        }
    }
    if ($failedPkgs.Count -gt 0 -or @(Get-MtpDriverPackages).Count -gt 0) {
        # WinUSB が残ったまま常駐アプリや mtpswitch.exe を消すと、トラックパッドを使う手段がなくなる。ここで止めて、ほかは残す
        throw "WinUSB のドライバーパッケージを取り除けませんでした（$((@($failedPkgs) + @(Get-MtpDriverPackages) | Select-Object -Unique) -join ', ')）。" +
            "ファイル・証明書・「アプリ」の一覧の登録は残しています。再起動してから、もう一度「取り除く」を実行してください"
    }
    Info 'USB ケーブルを抜き差しすると、確実に標準のドライバーに戻ります'

    Step '証明書の信頼を外しています'
    Remove-InstallerCertificates
    if ($RemoveLegacyCertificate) { Remove-LegacyCertificates }

    Step 'ログオン時の起動の登録を外しています'
    if (Test-SameUserAsDesktop) {
        $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
        $v = (Get-ItemProperty $run -ErrorAction SilentlyContinue).'MAGITRA'
        if ($v) { Remove-ItemProperty $run -Name 'MAGITRA'; Info '外しました' }
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
    Remove-Item 'HKLM:\SOFTWARE\MAGITRA' -Recurse -Force -ErrorAction SilentlyContinue
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
