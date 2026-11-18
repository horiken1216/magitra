# ビルドとテスト

## 必要なもの

| もの | 使うところ |
|---|---|
| Visual Studio 2022 Build Tools（「C++ によるデスクトップ開発」、x64 の MSVC と Windows SDK 10.0.26100） | C のコード（常駐アプリ・サービス・道具・テスト）、リソース（`rc.exe`） |
| Windows Driver Kit 10.0.26100 | WinUSB 用 INF のカタログ（`Inf2Cat.exe`）。Bluetooth のドライバー `drivers/MtpBth`（KMDF） |
| .NET Framework 4.8 の C# コンパイラー（Windows に入っている `csc.exe`） | 設定画面・ランチャー・インストーラー（WPF） |
| Python 3 と `hidapi`（任意） | `probe/` の調査スクリプトだけ |

Visual Studio の IDE や MSBuild の連携は使わない。各フォルダーの `build.cmd` が `vswhere` で Build Tools を探し、`cl` / `link` / `csc` を直接呼ぶ。
WDK のバージョンは `drivers/MtpUsb/package.cmd` と `drivers/build.cmd` に書いてある（別のバージョンなら書き換える）。

## ビルド

リポジトリのルートで:

```bat
build.cmd
```

単体テストをすべて実行してから、インストーラー `build\mtpsetup.exe` まで作る（中身もすべて作り直す）。
`build\agent\` から常駐アプリを動かしている場合は、先に終了しておく（動いている間は上書きできない）。

部分ごとに作る場合:

| コマンド | できるもの |
|---|---|
| `core\build_tests.cmd` | レポート解析・振動レポートの単体テスト（`build\core\test_report.exe`） |
| `agent\build.cmd` | ジェスチャー・慣性・設定の単体テスト、常駐アプリ（`build\agent\mtpagent.exe`、UIAccess 版 `build\agent\install\mtpagent.exe`、コンソール版 `mtpagent-cli.exe`） |
| `agent\build.cmd test` | 上の単体テストだけ |
| `settings\build.cmd` | 設定画面（`build\agent\mtpsettings.exe`） |
| `launcher\build.cmd` | ランチャー（`build\agent\mtplauncher.exe`） |
| `service\build.cmd` | ロック中に標準の HID へ切り替えるサービス（`build\service\mtpswitchsvc.exe`） |
| `tools\mtpswitch\build.cmd` | 手動で切り替える道具（`build\mtpswitch\mtpswitch.exe`） |
| `drivers\MtpUsb\package.cmd` | WinUSB 用 INF と未署名のカタログ（`build\drivers\MtpUsb\`）。署名はインストーラーが導入先の PC で行う |
| `drivers\MtpBth\package.cmd` | Bluetooth のドライバー（Release）と未署名のカタログ（`build\drivers\Release\MtpBth\`）。署名はインストーラーが導入先の PC で行う |
| `installer\build.cmd` | 上をすべて作り直してから、インストーラー（`build\mtpsetup.exe`） |
| `tools\mtpusbdump\build.cmd` | USB の生フレームを表示する調査用の道具 |
| `drivers\build.cmd` / `tools\mtpdump\build.cmd` | Bluetooth のドライバー（`Debug analyze` で静的解析）とその表示ツール（[research/BLUETOOTH-SPIKE.md](research/BLUETOOTH-SPIKE.md)） |
| `tools\mkbrand\build.cmd` | `assets\brand` の元画像からアイコンとロゴの PNG を作り直す（元画像を変えたときだけ） |

## ビルドしたものを試す

- インストーラーを使わずに `build\agent\mtpagent.exe` を動かすこともできる。そのためには、先に WinUSB を当てておく必要がある
  （[MANUAL-INSTALL.md](MANUAL-INSTALL.md)。インストーラーで一度導入してあれば、ドライバーはそのまま使える）
- `build\agent\mtpagent-cli.exe` はコンソール版（`--dry-run` で入力を送らずイベントだけ表示、`--log` で記録、`--defaults` で既定の設定）。
  トレイ版と同時には動かせない
- `build\agent\replay_gesture.exe` は、`mtpagent-cli --log` の記録をジェスチャー判定に流し直す

## フォルダー

| フォルダー | 中身 |
|---|---|
| `core/` | Magic Trackpad のレポート解析と振動レポート（C、OS に依存しない） |
| `agent/` | 常駐アプリ（C）。WinUSB での読み取り、ジェスチャー判定、入力の注入（SendInput）、設定、ランチャーの呼び出し |
| `settings/` | 設定画面（C#、WPF） |
| `launcher/` | ランチャー（C#、WPF） |
| `service/` | ロック中・ログオン前に標準の HID へ切り替え、ログオン時に常駐アプリを起動するサービス（C） |
| `installer/` | インストーラー（C#、WPF）と、その処理本体 `setup-core.ps1` |
| `drivers/MtpUsb/` | Magic Trackpad の MI_01 に WinUSB を当てる INF |
| `drivers/MtpBth/` | Bluetooth のドライバー（KMDF。テスト署名モードでのみ使う詳細設定） |
| `tools/` | 調査・保守用の道具（生フレームの表示、手動の切り替え、手作業での導入、アイコン作り） |
| `probe/` | 調査スクリプト（Python） |
| `assets/brand/` | ロゴとアイコン（[利用条件](../assets/brand/README.md)） |
| `docs/` | 資料。`docs/research/` は調査記録 |

## リリース

`.github/workflows/release.yml` が、GitHub の Windows ランナーでビルドする（WDK・SDK 10.0.26100 がなければ winget で入れる）。

1. `installer/setup-core.ps1` の `$Version` を上げる
2. `docs/releases/vX.Y.Z.md` にリリースの本文を書く（SHA-256 の行はワークフローが足す）
3. コミットして push し、`vX.Y.Z` のタグを push する。単体テストと `build.cmd` が通れば、`mtpsetup.exe` を添付した Release が作られる

Actions の画面から手動で動かすと、ビルドとテストだけ行い、`mtpsetup.exe` をワークフローの成果物として残す（タグの前の確認用）。
署名はしない（インストーラーが導入先の PC で行う）ので、証明書や秘密を GitHub に置く必要はない。
