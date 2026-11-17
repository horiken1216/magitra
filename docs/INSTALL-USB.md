# MtpUsb（方式 B: WinUSB）の導入手順

USB 接続の Magic Trackpad のインターフェイス 1（MI_01）に、Windows 標準の HID ドライバーの代わりに
**Microsoft 署名済みの WinUSB**（`winusb.sys`）を当てる。自作のカーネルコードはなく、テスト署名モードも使わない。
署名するのは INF のカタログ（`mtpusb.cat`）だけで、自己署名証明書をこの PC に信頼させて使う。

**入れている間、USB 接続のトラックパッドは Windows のマウスとして動かない。**
常駐アプリ（手順 6 の `mtpagent.exe`）が、ポインター・クリック・スクロールなどの入力を作る。

## アプリ名

画面に出る名前は **MAGITRA**。トレイ・設定画面・インストーラー・「アプリ」の一覧・サービスの表示名・デバイスマネージャーの表示がこの名前になる。
ロゴとアイコンは `assets\brand`（元画像は `tiger.webp`・`logotype.webp`、作り直しは `tools\mkbrand\build.cmd`）。
導入先（`C:\Program Files\MTP`）・レジストリ（`HKCU\Software\MTP\Agent` など）・exe 名・サービス名（MtpSwitch）は、
導入済みの環境と設定を引き継ぐため MTP のまま。

## インストーラーで導入する（おすすめ）

`build\mtpsetup.exe` を実行すると、この文書の手順 1〜7（証明書・署名・WinUSB・コピー）をまとめて行う。
作るときは `installer\build.cmd`（中身もすべて作り直す。build\agent の常駐アプリが動いていれば先に終了する）。

| 選択肢 | 内容 |
|---|---|
| UIAccess 版を導入する（おすすめ） | 管理者権限のウィンドウでも操作できる。その場限りの証明書を作って署名し、公開鍵だけをこの PC の「信頼されたルート」に登録する |
| 標準版を導入する | 証明書の信頼は不要。管理者権限のウィンドウでは操作できない。**UIAccess 版が動かなくなったとき（会社の管理ツールが証明書を取り除いた場合など）は、これを選んで入れ直す** |
| 取り除く | WinUSB を外して標準のマウスに戻し、証明書の信頼・C:\Program Files\MTP・「アプリ」の一覧の登録を消す（設定は残る） |

- 証明書は導入のたびに作り、署名したらすぐ秘密鍵を消す。信頼されるのは、その導入で署名した MTP のファイルだけ
  （手作業の手順のように、信頼された証明書の秘密鍵が残ることはない）。タイムスタンプを付けられないので、証明書の期限は 20 年にしている
- WinUSB のカタログは、どちらの版でもその場限りの証明書で署名する（標準版では信頼の登録はしない。署名が未信頼でも導入できることを確認済み）
- 手作業の手順で作った証明書（MTP Test Signing）が残っていれば、取り除く選択肢が出る
- 「ロック中とログオン前は、標準のマウスとして使えるようにする」（既定でオン）を選ぶと、サービス MtpSwitch
  （`C:\Program Files\MTP\mtpswitchsvc.exe`、システム権限・自動起動）を登録する。
  ログオンして使っている間だけ WinUSB、それ以外（起動直後のログオン画面・ロック中・ログオフ・ユーザーの切り替え中）と
  起動時は Windows 標準の HidUsb に切り替える（シャットダウン中は切り替えられないため、起動してサービスが動き始めてから数秒かかる）。標準の HID でもポインター移動と左右のクリックが使える。
  切り替えには 2〜3 秒かかる（常駐アプリはロックされるとトラックパッドを手放し、解除されるとつなぎ直す）。
  記録は `C:\ProgramData\MTP\switch.log`
- ログオン時の起動を選んでいれば、サービスがログオン直後に常駐アプリを起動する（Windows の自動起動は 1 つずつ順に起動するため、
  ほかのアプリが多いと 40 秒以上かかった）。あとから Windows の自動起動でも起動されるが、`/autostart` 付きなので 2 つ目は黙って終わる
- 手動で切り替える道具 `C:\Program Files\MTP\mtpswitch.exe`（`status` / 管理者権限で `hid`・`winusb`）も入る
- 取り除くときは「設定」→「アプリ」の一覧からも行える（`C:\Program Files\MTP\mtpsetup.exe /uninstall`）
- インストーラーが普段とは別の管理者アカウントで動いた場合、常駐アプリの起動とログオン時の起動の設定は行わない（案内を出す）

以下の手順 1〜7 は、インストーラーを使わずに手作業で導入する方法（開発・調査用）。

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

デバイスマネージャーの「ユニバーサル シリアル バス デバイス」に **Magic Trackpad (MAGITRA WinUSB)** が出れば成功。

Windows 標準のドライバーが選ばれたままの場合は、手動で切り替える。
デバイスマネージャー →「ヒューマン インターフェイス デバイス」→「USB 入力デバイス」のうち、プロパティの「詳細」→「ハードウェア ID」が
`USB\VID_05AC&PID_0265&MI_01` のもの → ドライバーの更新 →「コンピューターを参照」→「一覧から選択」→ **Magic Trackpad (MAGITRA WinUSB)**。

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
| 4 本指で上（奥）へ払う | ランチャーを開く | - |
| 4 本指で下（手前）へ払う | 未割り当て | - |

上の表は既定の割り当て。設定画面の「ジェスチャー」で変えられる。

- 約 10mm 払うと 1 回だけ発動する。指を離すまで続けては発動しない
- 斜めすぎる動きは向きを決められないので発動しない
- 3 本以上で触れた後は、指をすべて離すまでポインターもスクロールも動かない（離す途中の指の動きを拾わないため）

### ランチャー

登録したアプリ・フォルダー・URL を、4 本指で上へ払って開いた画面からすぐ起動する（`mtplauncher.exe`、launcher\）。
登録は設定画面の「ランチャー」で行う（`HKCU\Software\MTP\Agent` の `LauncherItems`・`LauncherBrowser`）。

- ポインターのある画面の中央に一覧を出す。クリックするか、文字を打って絞り込み、矢印キーと Enter で起動。
  Esc・外側のクリック・ほかのウィンドウへの切り替えで閉じる
- 先頭には既定のブラウザー（https を開くアプリ）を出す。登録したものとは別で、設定でオフにできる
- 登録したものは、名前・対象・引数をあとから変えられる。名前を変えていなければ、対象を変えると名前も合わせて変わる

- ランチャーは常駐アプリが隠れたまま起動しておく（WPF の起動待ちなしで出すため）。常駐アプリを終了すると一緒に終わる
- 起動はエクスプローラーに頼む（スタートメニューから起動したのと同じになり、UIAccess 版の常駐アプリの権限を引き継がない）
- 「アプリやファイルを追加」はスタートメニューのフォルダーから選べる。ショートカットはショートカットのまま登録する。
  ストアアプリ（スタートメニューに .lnk がないもの）は今のところ選べない

### Force Touch の動き

- 1 本指で押し込むと、約 0.15 秒だけ様子を見てから左ボタンを送る。その間に強く押し込めば Force Touch になる
  （左クリックは送らず、重い振動と中クリックを送る）
- 様子を見ている間に指を約 2mm 動かすと、すぐにドラッグ（左ボタンの押下）になる。押したままのドラッグで Force Touch が誤って発動しないための規則
- 押してしばらくたってから強く押し込んでも Force Touch にはならない
- 普通のクリックの確定が最大 0.15 秒遅れる。気になる場合はメニューで Force Touch をオフにすると、押した瞬間に左ボタンを送る
- 押した・離したときの「カチッ」はトラックパッド本体の振動。Force Touch の振動は、押し込んだ直後に加わる重い「ブルッ」

## 7. 管理者権限のウィンドウでも使えるようにする（UIAccess 版の導入）

普通の `mtpagent.exe` は、管理者権限で動いているウィンドウ（タスクマネージャーなど）に入力を送れない。
**UIAccess 版**を `C:\Program Files\MTP` に入れると送れるようになる（画面読み上げソフトなどと同じ仕組み）。
Windows は、UIAccess 版を「信頼された証明書で署名され、保護されたフォルダーに置かれている」ときだけ起動する。

1. トレイのメニューの「終了」で常駐アプリを止める
2. ビルドする（管理者権限は不要）

   ```bat
   agent\build.cmd
   settings\build.cmd
   ```

   `build\agent\install\mtpagent.exe` が UIAccess 版（署名前なので、このままでは起動できない）

3. 証明書をこの PC で信頼する（**セキュリティ設定の変更**。手順 3 を済ませていれば不要）。
   **管理者権限の PowerShell** で、リポジトリのルートから:

   ```powershell
   certutil -addstore Root build\MtpTest.cer
   ```

   手順 3 は WinUSB の導入には必要なかった（2026-11-16、未登録のまま導入できた）が、UIAccess には必須。
   この証明書で署名したものは、この PC で信頼される。秘密鍵（証明書ストアの「MTP Test Signing」）は他へ出さない

4. 同じ管理者権限の PowerShell で導入する（署名・確認・コピー）

   ```powershell
   powershell -ExecutionPolicy Bypass -File install\install.ps1
   ```

   証明書が見つからないと言われたら、手順 2 を実行した PowerShell（のユーザー）で実行しているか確かめる

5. **普段の画面で**（管理者として実行しない）、エクスプローラーから `C:\Program Files\MTP\mtpagent.exe` を起動する。
   ログオン時に起動を使う場合は、トレイのメニューの「ログオン時に起動」を一度クリックして、登録先を新しい場所に変える
6. UIAccess が効いているか確かめる（管理者権限は不要）

   ```powershell
   powershell -ExecutionPolicy Bypass -File install\check_uiaccess.ps1
   ```

   `path = C:\Program Files\MTP\mtpagent.exe`、`uiAccess = True`、`elevated = False` なら成功

取り除くときは、常駐アプリと設定画面を終了してから、管理者権限の PowerShell で
`powershell -ExecutionPolicy Bypass -File install\uninstall.ps1`（WinUSB・証明書・設定は残る）。

### できないこと（Windows の仕様）

- **ログオン画面とロック画面では動かない**。WinUSB を当てた MI_01 は Windows のマウスとしては扱われず、
  常駐アプリはログオン後に起動し、保護された画面へは入力を送れないため。これらの画面では内蔵タッチパッドかキーボードを使う
- 管理者権限で動いているウィンドウ（タスクマネージャーなど）には、普通の `mtpagent.exe` からは入力が届かない（手順 7 の UIAccess 版で解決する）

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
