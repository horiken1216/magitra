# MtpUsb（方式 B: WinUSB）の導入手順

USB 接続の Magic Trackpad 2 のインターフェイス 1（MI_01）に、Windows 標準の HID ドライバーの代わりに
**Microsoft 署名済みの WinUSB**（`winusb.sys`）を当てる。自作のカーネルコードはなく、テスト署名モードも使わない。
署名するのは INF のカタログ（`mtpusb.cat`）だけで、自己署名証明書をこの PC に信頼させて使う。

**入れている間、USB 接続のトラックパッドは Windows のマウスとして動かない。**
ポインター操作は内蔵タッチパッドなどで行う（自前の入力注入はまだない）。

## 始める前に

- 影響するのは `USB\VID_05AC&PID_0265&MI_01` だけ。他のインターフェイス（MI_00/02/03）、BT 接続、他の機器は変えない
- 証明書の信頼登録（手順 3）はセキュリティ設定の変更。登録した証明書で署名されたものは、この PC で信頼される。
  検証が終わったら「元に戻す」の手順で削除する。**会社の PC では、情シスの規定で許されるか先に確認する**
- この方式でテスト署名モードなしに導入できるかは、まだこの PC で確認していない。手順 4 で署名エラーが出たら、テスト署名モードは有効にせず、エラー内容を共有してほしい

以下、2〜4 と「元に戻す」は**管理者権限の PowerShell** で、リポジトリのルート（`C:\work\MTP`）から実行する。

## 1. パッケージと確認ツールを作る（管理者権限は不要）

```bat
drivers\MtpUsb\package.cmd
tools\mtpusbdump\build.cmd
```

`build\drivers\MtpUsb\` に `MtpUsb.inf` と `mtpusb.cat`、`build\mtpusbdump\` に `mtpusbdump.exe` ができる。

## 2. 証明書を作り、カタログに署名する

証明書は `docs/INSTALL-SPIKE.md` と同じ「MTP Test Signing」を使う。まだ作っていなければ作る。

```powershell
cd C:\work\MTP   # 以降のパスはリポジトリのルートからの相対パス
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq "CN=MTP Test Signing" | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=MTP Test Signing" `
    -CertStoreLocation Cert:\CurrentUser\My -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(2)
}
Export-Certificate -Cert $cert -FilePath build\MtpTest.cer | Out-Null
$kit = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0"
& "$kit\x64\signtool.exe" sign /fd sha256 /s My /n "MTP Test Signing" build\drivers\MtpUsb\mtpusb.cat
```

`package.cmd` を実行し直すとカタログが作り直されるので、署名もやり直す。

## 3. 証明書を信頼する（初回のみ・セキュリティ設定の変更）

```powershell
certutil -addstore Root build\MtpTest.cer
certutil -addstore TrustedPublisher build\MtpTest.cer
```

## 4. WinUSB を当てる

トラックパッドを USB ケーブルでつないだ状態で実行する。

```powershell
pnputil /add-driver build\drivers\MtpUsb\MtpUsb.inf /install
```

デバイスマネージャーの「ユニバーサル シリアル バス デバイス」に **Magic Trackpad 2 (MTP WinUSB)** が出れば成功。

Windows 標準のドライバーが選ばれたままの場合は、手動で切り替える。
デバイスマネージャー →「ヒューマン インターフェイス デバイス」→「USB 入力デバイス」のうち、プロパティの「詳細」→「ハードウェア ID」が
`USB\VID_05AC&PID_0265&MI_01` のもの → ドライバーの更新 →「コンピューターを参照」→「一覧から選択」→ **Magic Trackpad 2 (MTP WinUSB)**。

## 5. 動作を確認する（管理者権限は不要）

まず、マルチタッチモードへ切り替えずに、既定モードで何が流れるかを見る。実行したらトラックパッドを触る。

```bat
build\mtpusbdump\mtpusbdump.exe --no-enable --count 30
```

次に、切り替えてから生バイト列付きで見る。1 本指 → 2 本指 → 押し込み（クリック）の順に触る。

```bat
build\mtpusbdump\mtpusbdump.exe --hex --count 200 > build\usb_frames.txt
```

最初に表示されるパイプ一覧と `SET_REPORT` の結果、`build\usb_frames.txt` を共有してほしい。レポート形式の確認に使う。

## 6. 常駐アプリで普段使いする

```bat
agent\build.cmd
build\agent\mtpagent.exe
```

タスクトレイにアイコンが出る。アイコンにマウスを重ねると状態（接続中など）が表示され、クリックするとメニューが開く。

| メニュー | 内容 |
|---|---|
| 一時停止 | 入力を送るのを止める（押したままのボタンは離す） |
| ナチュラルスクロール | スクロールの向き（オンで Mac と同じ。設定は保存される） |
| Force Touch | オンにすると、1 本指で強く押し込んだときに重い振動を返して中クリックを送る（設定は保存される） |
| 3・4本指ジェスチャー | オンにすると、3・4 本指で払う操作を下の表のキー操作に変える（設定は保存される） |
| ログオン時に起動 | オンにすると、次回のログオンから自動で起動する（ユーザーごとのスタートアップ登録。管理者権限は不要） |
| 終了 | 常駐アプリを終了する |

- USB ケーブルを抜いても終了しない。つなぎ直すと 2 秒以内に再接続する
- 2 つ目を起動しようとすると「すでに起動しています」と出て終了する
- 「ログオン時に起動」は `build\agent\mtpagent.exe` の場所を登録する。フォルダーを移動したら、メニューで一度オフにしてからオンにし直す
- ビルドし直すときは、先にメニューの「終了」で止める（動いている間は実行ファイルを上書きできない）
- 調査用のコンソール版は `build\agent\mtpagent-cli.exe`（`--dry-run`、`--log`、`--no-force` など）。トレイ版と同時には動かせない

### 3・4 本指ジェスチャー

| ジェスチャー | 操作 | 送るキー |
|---|---|---|
| 3 本指で左へ払う | 右隣の仮想デスクトップへ（Mac と同じ向き） | Ctrl+Win+→ |
| 3 本指で右へ払う | 左隣の仮想デスクトップへ | Ctrl+Win+← |
| 3 本指で上（奥）へ払う | タスクビュー | Win+Tab |
| 3 本指で下（手前）へ払う | デスクトップの表示 | Win+D |
| 4 本指で左右に払う | 直前のアプリに切り替え | Alt+Tab |
| 4 本指で上下に払う | 未割り当て（ランチャー用） | - |

- 約 10mm 払うと 1 回だけ発動する。指を離すまで続けては発動しない
- 斜めすぎる動きは向きを決められないので発動しない
- 3 本以上で触れた後は、指をすべて離すまでポインターもスクロールも動かない（離す途中の指の動きを拾わないため）

### Force Touch の動き

- 1 本指で押し込むと、約 0.15 秒だけ様子を見てから左ボタンを送る。その間に強く押し込めば Force Touch になる
  （左クリックは送らず、重い振動と中クリックを送る）
- 様子を見ている間に指を約 2mm 動かすと、すぐにドラッグ（左ボタンの押下）になる。押したままのドラッグで Force Touch が誤って発動しないための規則
- 押してしばらくたってから強く押し込んでも Force Touch にはならない
- 普通のクリックの確定が最大 0.15 秒遅れる。気になる場合はメニューで Force Touch をオフにすると、押した瞬間に左ボタンを送る
- 押した・離したときの「カチッ」はトラックパッド本体の振動。Force Touch の振動は、押し込んだ直後に加わる重い「ブルッ」

### できないこと（Windows の仕様）

- **ログオン画面とロック画面では動かない**。WinUSB を当てた MI_01 は Windows のマウスとしては扱われず、
  常駐アプリはログオン後に起動し、保護された画面へは入力を送れないため。これらの画面では内蔵タッチパッドかキーボードを使う
- 管理者権限で動いているウィンドウ（タスクマネージャーなど）には入力が届かない

## 元に戻す

常駐アプリをメニューの「終了」で止め、「ログオン時に起動」をオンにしていた場合は先にオフにしておく。

```powershell
pnputil /enum-drivers | Select-String -Context 1,4 "MTP Project"   # oemNN.inf の番号を確認
pnputil /delete-driver oemNN.inf /uninstall /force
```

USB ケーブルを抜き差しすると、MI_01 は Windows 標準の HID ドライバーに戻る。

検証を終えたら証明書も削除する（`docs/INSTALL-SPIKE.md` のスパイクでも使っている場合は、両方を撤去してから）。

```powershell
certutil -delstore Root "MTP Test Signing"
certutil -delstore TrustedPublisher "MTP Test Signing"
Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq "CN=MTP Test Signing" | Remove-Item
```
