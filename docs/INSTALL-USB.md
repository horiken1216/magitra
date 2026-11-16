# MtpUsb（方式 B: WinUSB）の導入手順

USB 接続の Magic Trackpad のインターフェイス 1（MI_01）に、Windows 標準の HID ドライバーの代わりに
**Microsoft 署名済みの WinUSB**（`winusb.sys`）を当てる。自作のカーネルコードはなく、テスト署名モードも使わない。
署名するのは INF のカタログ（`mtpusb.cat`）だけで、自己署名証明書をこの PC に信頼させて使う。

**入れている間、USB 接続のトラックパッドは Windows のマウスとして動かない。**
常駐アプリ（手順 6 の `mtpagent.exe`）が、ポインター・クリック・スクロールなどの入力を作る。

## 対応機種

| 機種 | PID | 状態 |
|---|---|---|
| Magic Trackpad（Lightning、2021 年の第 3 世代） | 0x0265 | **実機で確認済み**（このリポジトリの試験はすべてこの機種） |
| Magic Trackpad 2（Lightning、2015 年） | 0x0265 | 第 3 世代と同じ PID・同じ扱い。実機では未確認 |
| Magic Trackpad（USB-C、2024 年の第 4 世代） | 0x0324 | **仮対応**（実機未確認）。受け付けるようにしてあるが、動く保証はない |

第 4 世代は、公開情報で「Actuator が MI_02・出力レポート 0x53」など第 3 世代と同じ作りと報告されているため、
INF・常駐アプリ・調査スクリプトが PID 0x0324 も受け付ける**仮対応**にしてある。次の点は確かめていない。

- マルチタッチのレポートが MI_01 にあるか（INF は MI_01 に WinUSB を当てる）
- マルチタッチへの切り替えが同じ `02 01` で、フレームの形式（ヘッダー 12 バイト＋指 9 バイト）・座標の範囲が同じか
- 振動レポート（gist の 14 バイト）が同じ意味か

第 4 世代を試すときは、WinUSB を当てる前に、標準の HID のまま構成を調べる。

```bat
python probe\enum_devices.py
python probe\caps.py
```

インターフェイスごとのコレクション・レポート ID・長さが、`probe/FINDINGS.md` の「USB 接続時」の表と同じかを比べる。
違っていれば、INF の `MI_01` や、`core/` のフレーム形式を直す必要がある。

## 始める前に

- 影響するのは `USB\VID_05AC&PID_0265&MI_01`（と第 4 世代の `PID_0324&MI_01`）だけ。他のインターフェイス（MI_00/02/03）、BT 接続、他の機器は変えない
- 証明書の信頼登録（手順 3）はセキュリティ設定の変更。登録した証明書で署名されたものは、この PC で信頼される。
  検証が終わったら「元に戻す」の手順で削除する。**会社の PC では、情シスの規定で許されるか先に確認する**
- 第 3 世代では、テスト署名モードなしで導入できることを確認した。手順 4 で署名エラーが出たら、テスト署名モードは有効にせず、エラー内容を確かめる

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

デバイスマネージャーの「ユニバーサル シリアル バス デバイス」に **Magic Trackpad (MTP WinUSB)** が出れば成功。

Windows 標準のドライバーが選ばれたままの場合は、手動で切り替える。
デバイスマネージャー →「ヒューマン インターフェイス デバイス」→「USB 入力デバイス」のうち、プロパティの「詳細」→「ハードウェア ID」が
`USB\VID_05AC&PID_0265&MI_01` のもの → ドライバーの更新 →「コンピューターを参照」→「一覧から選択」→ **Magic Trackpad (MTP WinUSB)**。

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
settings\build.cmd
build\agent\mtpagent.exe
```

`settings\build.cmd` は設定画面 `build\agent\mtpsettings.exe` を作る（Windows 標準の C# コンパイラーを使うので、追加の導入は要らない）。
設定画面は `mtpagent.exe` と同じフォルダーに置く。

タスクトレイにアイコンが出る。アイコンにマウスを重ねると状態（接続中など）が表示され、クリックするとメニューが開く。

| メニュー | 内容 |
|---|---|
| 設定... | 設定画面を開く（下の「設定画面」） |
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

### 設定画面

左の分類を選んで、右側の項目を変える。**変えた瞬間に保存され、動いている常駐アプリにすぐ反映される**（保存ボタンや再起動は要らない）。

| 分類 | 変えられるもの |
|---|---|
| ポインター | ポインターの速さ・加速、スクロールの速さ、ナチュラルスクロール |
| クリック | Force Touch のオン・オフ、発動に必要な押し込みの強さ、強く押し込んだときの操作 |
| 振動 | Force Touch の振動の強さ・鋭さ・重さ（「試す」で鳴らせる。上限は安全とされる範囲に固定） |
| ジェスチャー | 3・4 本指ジェスチャーのオン・オフ、3・4 本指 × 上下左右の割り当て |
| 全般 | 常駐アプリの状態と起動、ログオン時に起動、すべての設定を既定に戻す |

- スライダーは、つまみ以外の場所を押すとその位置へ動く。右端の「既定」でそのスライダーだけを既定値に戻せる（既定値のときは押せない）
- 設定は `HKCU\Software\MTP\Agent` に保存される（名前・範囲・既定値は `agent/mtp_settings.h`）
- 割り当てられる操作: なし、右隣・左隣の仮想デスクトップ、タスクビュー、デスクトップの表示、直前のアプリに切り替え、
  中クリック、戻る、進む、再生・一時停止、検索
- 調査用の `mtpagent-cli.exe` も保存された設定を読む。`--defaults` を付けると既定値で動く

### 3・4 本指ジェスチャー

| ジェスチャー | 操作 | 送るキー |
|---|---|---|
| 3 本指で左へ払う | 右隣の仮想デスクトップへ（Mac と同じ向き） | Ctrl+Win+→ |
| 3 本指で右へ払う | 左隣の仮想デスクトップへ | Ctrl+Win+← |
| 3 本指で上（奥）へ払う | タスクビュー | Win+Tab |
| 3 本指で下（手前）へ払う | デスクトップの表示 | Win+D |
| 4 本指で左右に払う | 直前のアプリに切り替え | Alt+Tab |
| 4 本指で上下に払う | 未割り当て（ランチャー用） | - |

上の表は既定の割り当て。設定画面の「ジェスチャー」で変えられる。

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
