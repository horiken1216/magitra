# MtpBth ドライバーの開発者向けの手順

**普通に使うときは、インストーラーの詳細設定で入れる（[../BLUETOOTH.md](../BLUETOOTH.md)）。** ここは、インストーラーを使わずに
Debug ビルドを入れて、生のフレームを `mtpdump` で見るための手順（開発・調査用）。

MtpBth は Magic Trackpad（Bluetooth）の HID の L2CAP チャネルを自分で開き、生フレームをユーザーモード（常駐アプリ・mtpdump）へ渡す。
**入れている間、トラックパッドは常駐アプリなしでは Windows のマウスとして動かない。**
ロック中に Windows 標準のドライバーへ戻すのはサービス MagitraSwitch（インストーラーで入れる）の役目。

## 始める前に

- このドライバーはテスト署名で、カーネルで動く。不具合があるとブルースクリーンになりうる。作業前に復元ポイントを作っておく
- テスト署名モードは Secure Boot がオフでないと有効にできないことが多い。**組織が管理する PC では、規定で許されているかを先に確かめる**
- BitLocker が有効な場合は、ブート設定を変えると回復キーを求められることがある。回復キーを手元に用意しておく
- 影響するのは Magic Trackpad（`BTHENUM\{00001124-...}_VID&0001004c_PID&0265`）だけ。他の Bluetooth 機器のドライバーは変えない

以下、2〜6 は**管理者権限の PowerShell** で、リポジトリのルートから実行する。

2〜4 と 6 は、`drivers\spike.ps1` でまとめて行える（テスト署名モードが有効なときだけ動く。証明書は `LocalMachine\My` に作る）。

```powershell
.\drivers\spike.ps1 install     # 署名・カタログ・証明書の信頼・トラックパッドへの適用
.\drivers\spike.ps1 uninstall   # ドライバーと証明書を取り除く
```

## 0. Secure Boot をオフにする（初回のみ・セキュリティ設定の変更）

Secure Boot がオンのままだと、`bcdedit /set testsigning on` は「セキュア ブート ポリシーによって保護されている」というエラーになる。

1. `manage-bde -protectors -get C:` で、BitLocker の回復キー（数字のパスワード）があれば控える。デバイスの暗号化の場合は https://account.microsoft.com/devices/recoverykey でも確認できる
2. 設定 → システム → 回復 →「PC の起動をカスタマイズする」の「今すぐ再起動」→ トラブルシューティング → 詳細オプション →「UEFI ファームウェアの設定」で UEFI の設定画面に入る
3. Security（または Boot）にある **Secure Boot** を **Disabled** にして、保存して再起動する（項目の場所は PC のメーカーで異なる）
4. `msinfo32` の「セキュア ブートの状態」が「オフ」になっていれば完了

カーネル ドライバーが「メモリ整合性」（HVCI）に合わず読み込まれない場合は、Windows セキュリティ → デバイス セキュリティ → コア分離 で、メモリ整合性をオフにする（再起動が要る）。

## 1. ビルド

```bat
drivers\build.cmd Debug
tools\mtpdump\build.cmd
```

`build\drivers\Debug\MtpBth\` に `MtpBth.sys` と `MtpBth.inf`、`build\mtpdump\` に確認ツール `mtpdump.exe` ができる。

## 2. テスト署名用の証明書を作る（初回のみ）

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=MAGITRA Test Signing" `
  -CertStoreLocation Cert:\CurrentUser\My -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(2)
Export-Certificate -Cert $cert -FilePath build\MtpTest.cer
```

## 3. 署名とカタログ作成

`.sys` に署名してからカタログを作り、最後にカタログにも署名する（カタログは `.sys` のハッシュを含むため、順序を守る）。

```powershell
$kit = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0"
$pkg = "build\drivers\Debug\MtpBth"
& "$kit\x64\signtool.exe" sign /fd sha256 /s My /n "MAGITRA Test Signing" "$pkg\MtpBth.sys"
& "$kit\x86\Inf2Cat.exe" /driver:$pkg /os:10_X64
& "$kit\x64\signtool.exe" sign /fd sha256 /s My /n "MAGITRA Test Signing" "$pkg\MtpBth.cat"
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

デバイスマネージャーの「システム デバイス」に **Magic Trackpad (MAGITRA Bluetooth)** が出れば成功。

Windows 標準の hidbth が選ばれたままの場合は、手動で切り替える。
デバイスマネージャー →「ヒューマン インターフェイス デバイス」→「Bluetooth HID デバイス」（Magic Trackpad のもの）→ ドライバーの更新 →「コンピューターを参照」→「一覧から選択」→ **Magic Trackpad (MAGITRA Bluetooth)**。

## 7. 動作を確認する（管理者権限は不要）

```bat
build\mtpdump\mtpdump.exe --status
build\mtpdump\mtpdump.exe --hex
```

- `--status` で `link=connected handshake=0x0` なら、L2CAP チャネル確立とマルチタッチ有効化が成功している
- `--hex` を付けて実行し、トラックパッドを 1 本指 → 2 本指 → 押し込み（クリック）の順に触る。出力を保存して共有してほしい（レポート形式の確認に使う）
- `--haptic` は振動レポートを 3 通りの送り方で 1 回ずつ送る（鳴るのは割り込みチャネルの `[3]`）
- `--hex --send <1|2|3> <16 進>` は任意のレポートを 1 つ送ってから表示を続ける（例: `--send 3 F10200` でマウスのモードに戻る）
- 常駐アプリ（`build\agent\mtpagent-cli.exe`）も、USB が見つからなければ MtpBth を使う

うまくいかない場合は `--status` の `link` と `last`（NTSTATUS）を共有してほしい。

## 元に戻す

```powershell
pnputil /enum-drivers | Select-String -Context 1,4 "MAGITRA"   # oemNN.inf の番号を確認
pnputil /delete-driver oemNN.inf /uninstall /force
```

トラックパッドは Windows 標準の hidbth に戻る。戻らない場合は、Bluetooth 設定でトラックパッドを削除してペアリングし直す。

検証を終えたら、テスト署名モードと証明書も元に戻す。

```powershell
bcdedit /set testsigning off
certutil -delstore Root "MAGITRA Test Signing"
certutil -delstore TrustedPublisher "MAGITRA Test Signing"
```

`bcdedit` の変更は次回起動から効く。再起動し、デスクトップ右下の「テスト モード」表示が消えたことを確認して完了。
