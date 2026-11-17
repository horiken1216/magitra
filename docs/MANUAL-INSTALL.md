# 手作業での導入（開発・調査用）

普段は [インストーラー](INSTALL.md) を使う。この文書は、インストーラーを使わずに、ビルドしたものを手で入れる手順。
インストーラーが中で行っていること（証明書・署名・WinUSB・コピー）を、1 つずつ行う。

USB 接続の Magic Trackpad のインターフェイス 1（MI_01）に、Windows 標準の HID ドライバーの代わりに
**Microsoft 署名済みの WinUSB**（`winusb.sys`）を当てる。自作のカーネルコードはなく、テスト署名モードも使わない。
署名するのは INF のカタログ（`mtpusb.cat`）だけで、自己署名証明書をこの PC に信頼させて使う。

**入れている間、USB 接続のトラックパッドは Windows のマウスとして動かない。** 常駐アプリが入力を作る。

## 始める前に

- 影響するのは `USB\VID_05AC&PID_0265&MI_01`（と第 4 世代の `PID_0324&MI_01`）だけ。ほかのインターフェイス（MI_00/02/03）、Bluetooth 接続、ほかの機器は変えない
- 証明書の信頼登録（手順 3）はセキュリティ設定の変更。登録した証明書で署名されたものは、この PC で信頼される。
  検証が終わったら「元に戻す」の手順で削除する。**組織が管理する PC では、規定で許されているかを先に確かめる**
- 第 3 世代では、テスト署名モードなしで導入できることを確認した。手順 4 で署名エラーが出たら、テスト署名モードは有効にせず、エラー内容を確かめる
- 手順 2 で作る証明書は秘密鍵が残る（インストーラーは署名後に消す）。秘密鍵は他へ出さない

以下、2〜4・7 と「元に戻す」は**管理者権限の PowerShell** で、リポジトリのルートから実行する。

## 1. パッケージと確認ツールを作る（管理者権限は不要）

```bat
drivers\MtpUsb\package.cmd
tools\mtpusbdump\build.cmd
```

`build\drivers\MtpUsb\` に `MtpUsb.inf` と `mtpusb.cat`、`build\mtpusbdump\` に `mtpusbdump.exe` ができる。

## 2. 証明書を作り、カタログに署名する

```powershell
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq "CN=MAGITRA Test Signing" | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=MAGITRA Test Signing" `
    -CertStoreLocation Cert:\CurrentUser\My -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(2)
}
Export-Certificate -Cert $cert -FilePath build\MtpTest.cer | Out-Null
$kit = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0"
& "$kit\x64\signtool.exe" sign /fd sha256 /s My /n "MAGITRA Test Signing" build\drivers\MtpUsb\mtpusb.cat
```

`package.cmd` を実行し直すとカタログが作り直されるので、署名もやり直す。

## 3. 証明書を信頼する（初回のみ・セキュリティ設定の変更）

```powershell
certutil -addstore Root build\MtpTest.cer
certutil -addstore TrustedPublisher build\MtpTest.cer
```

WinUSB の導入だけなら、この手順なしでも入った。手順 7 の UIAccess 版には必須。

## 4. WinUSB を当てる

トラックパッドを USB ケーブルでつないだ状態で実行する。

```powershell
pnputil /add-driver build\drivers\MtpUsb\MtpUsb.inf /install
```

デバイスマネージャーの「ユニバーサル シリアル バス デバイス」に **Magic Trackpad (MAGITRA WinUSB)** が出れば成功。

Windows 標準のドライバーが選ばれたままの場合は、手動で切り替える。
デバイスマネージャー →「ヒューマン インターフェイス デバイス」→「USB 入力デバイス」のうち、プロパティの「詳細」→「ハードウェア ID」が
`USB\VID_05AC&PID_0265&MI_01` のもの → ドライバーの更新 →「コンピューターを参照」→「一覧から選択」→ **Magic Trackpad (MAGITRA WinUSB)**。

## 5. 生フレームを見る（管理者権限は不要）

マルチタッチモードへ切り替えずに、既定モードで何が流れるかを見る。実行したらトラックパッドを触る。

```bat
build\mtpusbdump\mtpusbdump.exe --no-enable --count 30
```

切り替えてから、生バイト列付きで見る（1 本指 → 2 本指 → 押し込みの順に触る）。

```bat
build\mtpusbdump\mtpusbdump.exe --hex --count 200 > build\usb_frames.txt
```

## 6. ビルドした常駐アプリを動かす

```bat
agent\build.cmd
settings\build.cmd
launcher\build.cmd
build\agent\mtpagent.exe
```

設定画面とランチャーは `mtpagent.exe` と同じフォルダーに置く。使い方は [USAGE.md](USAGE.md)。

- 「ログオン時に起動」は、そのときの `mtpagent.exe` の場所を登録する。フォルダーを移したら、メニューで一度オフにしてからオンにし直す
- ビルドし直すときは、先にメニューの「終了」で止める（動いている間は実行ファイルを上書きできない）
- この手順では、ロック中に標準の HID へ戻すサービスは入らない（ロック画面では USB 接続のトラックパッドは動かない）

## 7. UIAccess 版を入れる（管理者権限のウィンドウでも操作できるようにする）

普通の `mtpagent.exe` は、管理者権限で動いているウィンドウ（タスクマネージャーなど）に入力を送れない。
**UIAccess 版**を `C:\Program Files\MAGITRA` に入れると送れるようになる。
Windows は、UIAccess 版を「信頼された証明書で署名され、保護されたフォルダーに置かれている」ときだけ起動する。

1. トレイのメニューの「終了」で常駐アプリを止める
2. 手順 6 のビルドを済ませる（`build\agent\install\mtpagent.exe` が UIAccess 版。署名前なので、このままでは起動できない）
3. 手順 3 で証明書を「信頼されたルート」に登録しておく
4. 手順 2 と同じユーザーの管理者権限の PowerShell で、署名・確認・コピーする

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\manual-install\install.ps1
   ```

5. **普段の画面で**（管理者として実行しない）、エクスプローラーから `C:\Program Files\MAGITRA\mtpagent.exe` を起動する
6. UIAccess が効いているか確かめる（管理者権限は不要）

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\manual-install\check_uiaccess.ps1
   ```

   `path = C:\Program Files\MAGITRA\mtpagent.exe`、`uiAccess = True`、`elevated = False` なら成功

取り除くときは、常駐アプリと設定画面を終了してから
`powershell -ExecutionPolicy Bypass -File tools\manual-install\uninstall.ps1`（WinUSB・証明書・設定は残る）。

## 元に戻す

常駐アプリをメニューの「終了」で止め、「ログオン時に起動」をオンにしていた場合は先にオフにしておく。

```powershell
pnputil /enum-drivers | Select-String -Context 1,4 "MAGITRA"   # oemNN.inf の番号を確認
pnputil /delete-driver oemNN.inf /uninstall /force
```

USB ケーブルを抜き差しすると、MI_01 は Windows 標準の HID ドライバーに戻る。証明書も削除する。

```powershell
certutil -delstore Root "MAGITRA Test Signing"
certutil -delstore TrustedPublisher "MAGITRA Test Signing"
Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq "CN=MAGITRA Test Signing" | Remove-Item -DeleteKey   # 秘密鍵も消す
```
