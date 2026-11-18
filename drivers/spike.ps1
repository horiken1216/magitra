<#
  MtpBth スパイクドライバー（Bluetooth）を、テスト署名して入れる・取り除く（検証用）。
  管理者権限の PowerShell で、リポジトリのルートから実行する。先に drivers\build.cmd Debug でビルドしておく。

    .\drivers\spike.ps1 install     署名 → カタログ作成 → 証明書を信頼 → トラックパッドに当てる
    .\drivers\spike.ps1 uninstall   ドライバーと証明書を取り除く（トラックパッドは hidbth に戻る）

  テスト署名モード（bcdedit /set testsigning on）と Secure Boot の切り替えは、このスクリプトでは行わない。
  手順は docs\research\BLUETOOTH-SPIKE.md を参照。
#>
param([Parameter(Mandatory)][ValidateSet('install', 'uninstall')][string]$Action)

$ErrorActionPreference = 'Stop'
$Subject = 'CN=MAGITRA Test Signing'
$Kit = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0"
$Root = Split-Path -Parent $PSScriptRoot
$Pkg = Join-Path $Root 'build\drivers\Debug\MtpBth'
$HwIds = @(
    'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0265',
    'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0324')

function Invoke-Tool([string]$Exe, [string[]]$ToolArgs) {
    & $Exe @ToolArgs
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Exe) が失敗しました（終了コード $LASTEXITCODE）" }
}

function Assert-Admin {
    $p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '管理者権限の PowerShell で実行してください'
    }
}

Add-Type -Namespace Mtp -Name Nt -MemberDefinition @'
[DllImport("ntdll.dll")]
public static extern int NtQuerySystemInformation(int infoClass, byte[] buffer, int length, out int returned);
'@

function Test-TestSigning {
    # SystemCodeIntegrityInformation（103）: ENABLED（0x01）が落ちている＝「ドライバー署名の強制を無効にする」で起動した、
    # TESTSIGN（0x02）が立っている＝テスト署名モード。どちらかなら、このドライバーは読み込まれる
    $buf = [byte[]]::new(8)
    [BitConverter]::GetBytes([int]8).CopyTo($buf, 0)
    $ret = 0
    if ([Mtp.Nt]::NtQuerySystemInformation(103, $buf, 8, [ref]$ret) -ne 0) { return $false }
    $opt = [BitConverter]::ToUInt32($buf, 4)
    return (($opt -band 0x02) -ne 0) -or (($opt -band 0x01) -eq 0)
}

function Get-SpikeInfs {
    # pnputil の一覧から、元の名前が mtpbth.inf のパッケージ（oemNN.inf）を探す
    $names = @()
    $cur = @{}
    foreach ($line in (& pnputil /enum-drivers)) {
        if ($line -match '^\s*$') { $cur = @{}; continue }
        if ($line -match ':\s*(oem\d+\.inf)\s*$') { $cur.oem = $Matches[1] }
        if ($line -match ':\s*mtpbth\.inf\s*$' -and $cur.oem) { $names += $cur.oem }
    }
    return $names | Select-Object -Unique
}

Add-Type -Namespace Mtp -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId, string infPath, uint flags, out bool reboot);
'@

Assert-Admin

if ($Action -eq 'install') {
    if (-not (Test-TestSigning)) {
        throw 'テスト署名モードでも「ドライバー署名の強制を無効にする」での起動でもありません。どちらかで起動してから実行してください'
    }
    if (-not (Test-Path "$Pkg\MtpBth.sys")) { throw "$Pkg\MtpBth.sys がありません。先に drivers\build.cmd Debug を実行してください" }
    Remove-Item "$Pkg\MtpBth.cat" -ErrorAction SilentlyContinue

    $cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $Subject | Select-Object -First 1
    if (-not $cert) {
        Write-Host '署名用の証明書を作ります'
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\LocalMachine\My `
            -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(2)
    }
    $cer = Join-Path $env:TEMP 'MtpTest.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null

    Write-Host '署名とカタログを作ります'
    Invoke-Tool "$Kit\x64\signtool.exe" @('sign', '/q', '/fd', 'sha256', '/sm', '/s', 'My', '/sha1', $cert.Thumbprint, "$Pkg\MtpBth.sys")
    Invoke-Tool "$Kit\x86\Inf2Cat.exe" @("/driver:$Pkg", '/os:10_X64')
    Invoke-Tool "$Kit\x64\signtool.exe" @('sign', '/q', '/fd', 'sha256', '/sm', '/s', 'My', '/sha1', $cert.Thumbprint, "$Pkg\MtpBth.cat")

    Write-Host '証明書を信頼させます（Root と TrustedPublisher）'
    Invoke-Tool certutil.exe @('-f', '-addstore', 'Root', $cer)
    Invoke-Tool certutil.exe @('-f', '-addstore', 'TrustedPublisher', $cer)
    Remove-Item $cer

    Write-Host 'ドライバーを追加します'
    Invoke-Tool pnputil.exe @('/add-driver', "$Pkg\MtpBth.inf")

    # 標準の hidbth より優先されないことがあるので、つながっているトラックパッドに強制で当てる
    $applied = $false
    foreach ($id in $HwIds) {
        $reboot = $false
        if ([Mtp.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $id, "$Pkg\MtpBth.inf", 1, [ref]$reboot)) {
            Write-Host "当てました: $id$(if ($reboot) { '（再起動が必要）' })"
            $applied = $true
        }
    }
    if (-not $applied) {
        Write-Warning 'つながっている Magic Trackpad が見つかりませんでした。Bluetooth でつないでから、もう一度 install を実行してください'
    } else {
        Write-Host '完了。次は build\mtpdump\mtpdump.exe --status で確かめてください'
    }
}
else {
    $infs = @(Get-SpikeInfs)
    foreach ($inf in $infs) {
        Write-Host "取り除きます: $inf"
        Invoke-Tool pnputil.exe @('/delete-driver', $inf, '/uninstall', '/force')
    }
    if (-not $infs) { Write-Host 'MtpBth は入っていません' }
    foreach ($store in 'Root', 'TrustedPublisher', 'My') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq $Subject | ForEach-Object {
            Write-Host "証明書を削除します: LocalMachine\$store"
            Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue
            if (Test-Path $_.PSPath) { Remove-Item $_.PSPath }
        }
    }
    Write-Host '完了。トラックパッドが動かない場合は、Bluetooth をオフ→オンするか、ペアリングし直してください'
    Write-Host 'テスト署名モードを戻すには: bcdedit /set testsigning off（再起動で反映）'
}
