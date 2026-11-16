# MtpBth スパイクドライバーの導入手順

MtpBth は Magic Trackpad 2（Bluetooth）の生フレームを取り出して、レポート形式を実機で確認するための検証用ドライバー。
**入れている間、トラックパッドは Windows のマウスとして動かない。**入力を Windows へ渡す処理（PTP）はまだない。

## 始める前に

- このドライバーはテスト署名で、カーネルで動く。不具合があるとブルースクリーンになりうる。作業前に復元ポイントを作っておく
- テスト署名モードは Secure Boot がオフでないと有効にできないことが多い。**会社の PC では、情シスの規定で許されるか先に確認する**
- BitLocker が有効な場合は、ブート設定を変えると回復キーを求められることがある。回復キーを手元に用意しておく
- 影響するのは Magic Trackpad 2（`BTHENUM\{00001124-...}_VID&0001004c_PID&0265`）だけ。他の Bluetooth 機器のドライバーは変えない

以下、2〜6 は**管理者権限の PowerShell** で、リポジトリのルート（`C:\work\MTP`）から実行する。

## 1. ビルド

```bat
drivers\build.cmd Debug
tools\mtpdump\build.cmd
```

`build\drivers\Debug\MtpBth\` に `MtpBth.sys` と `MtpBth.inf`、`build\mtpdump\` に確認ツール `mtpdump.exe` ができる。

## 2. テスト署名用の証明書を作る（初回のみ）

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=MTP Test Signing" `
  -CertStoreLocation Cert:\CurrentUser\My -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(2)
Export-Certificate -Cert $cert -FilePath build\MtpTest.cer
```

## 3. 署名とカタログ作成

`.sys` に署名してからカタログを作り、最後にカタログにも署名する（カタログは `.sys` のハッシュを含むため、順序を守る）。

```powershell
$kit = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0"
$pkg = "build\drivers\Debug\MtpBth"
& "$kit\x64\signtool.exe" sign /fd sha256 /s My /n "MTP Test Signing" "$pkg\MtpBth.sys"
& "$kit\x86\Inf2Cat.exe" /driver:$pkg /os:10_X64
& "$kit\x64\signtool.exe" sign /fd sha256 /s My /n "MTP Test Signing" "$pkg\MtpBth.cat"
```

## 4. 証明書を信頼する（初回のみ・セキュリティ設定の変更）

```powershell
certutil -addstore Root build\MtpTest.cer
certutil -addstore TrustedPublisher build\MtpTest.cer
```

## 5. テスト署名モードを有効にする（初回のみ・セキュリティ設定の変更）

```powershell
bcdedit /set testsigning on
```

再起動する。デスクトップ右下に「テスト モード」と表示されれば有効。

## 6. ドライバーを入れる

```powershell
pnputil /add-driver build\drivers\Debug\MtpBth\MtpBth.inf /install
```

デバイスマネージャーの「システム デバイス」に **Magic Trackpad 2 (MTP Bluetooth spike)** が出れば成功。

Windows 標準の hidbth が選ばれたままの場合は、手動で切り替える。
デバイスマネージャー →「ヒューマン インターフェイス デバイス」→「Bluetooth HID デバイス」（Magic Trackpad のもの）→ ドライバーの更新 →「コンピューターを参照」→「一覧から選択」→ **Magic Trackpad 2 (MTP Bluetooth spike)**。

## 7. 動作を確認する（管理者権限は不要）

```bat
build\mtpdump\mtpdump.exe --status
build\mtpdump\mtpdump.exe --hex
```

- `--status` で `link=connected handshake=0x0` なら、L2CAP チャネル確立とマルチタッチ有効化が成功している
- `--hex` を付けて実行し、トラックパッドを 1 本指 → 2 本指 → 押し込み（クリック）の順に触る。出力を保存して共有してほしい（レポート形式の確認に使う）

うまくいかない場合は `--status` の `link` と `last`（NTSTATUS）を共有してほしい。

## 元に戻す

```powershell
pnputil /enum-drivers | Select-String -Context 1,4 "MTP Project"   # oemNN.inf の番号を確認
pnputil /delete-driver oemNN.inf /uninstall /force
```

トラックパッドは Windows 標準の hidbth に戻る。戻らない場合は、Bluetooth 設定でトラックパッドを削除してペアリングし直す。

検証を終えたら、テスト署名モードと証明書も元に戻す。

```powershell
bcdedit /set testsigning off
certutil -delstore Root "MTP Test Signing"
certutil -delstore TrustedPublisher "MTP Test Signing"
```

`bcdedit` の変更は次回起動から効く。再起動し、デスクトップ右下の「テスト モード」表示が消えたことを確認して完了。
