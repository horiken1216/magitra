# MTP アーキテクチャ案（2026-11-16 時点の検討）

## 1. 既存 OSS（imbushuo/mac-precision-touchpad）の調査結果

取得元: https://github.com/imbushuo/mac-precision-touchpad（最終コミット 2022-03-14、`29d2778`）
配置: `third_party/mac-precision-touchpad`

### Magic Trackpad 2 (BT/USB) を扱うのは `AmtPtpHidFilter`
- Col01（マウス）に `mshidkmdf` を機能ドライバー、自身を LowerFilter として入れ、PTP（Precision Touchpad）の記述子を返す
- 起動時に `F1 02 01`（BT）または `02 01 00 00`（USB）を送ってマルチタッチモードへ切り替える
- 下位から生レポートを読み、PTP レポートに変換する（指 9 バイト/本。X は 13bit、Y は符号付き、Pressure 1 バイト、TouchMajor/Minor、ContactID）
- 圧力（Pressure）は読み取れるが PTP に載せていない。ハプティクス（クリック感の制御）は未実装

### 問題 1: ライセンスが不明
`LICENSE.md` は USB 用 (`AmtPtpDeviceUsbUm/Km`: GPLv2) と SPI 用 (`AmtPtpDeviceSpiKm`: MIT) しか定めていない。
**BT 対応に必要な `AmtPtpHidFilter` にはライセンス表記がない**（ソース内にもヘッダーなし）。
表記がない場合は著作権者の許諾なしにコードを流用できないため、そのままフォークするのは避けるべき。

### 問題 2: システム全体に及ぶ非公開構造体の書き換え（`Detour.c`）
`PtpFilterDetourWindowsHIDStack` は、下位デバイスの DriverObject（BT なら `hidbth.sys`、USB なら `hidusb.sys`）の
`MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL]` を、hidclass の非公開構造体から取り出した元の関数に書き戻す。

- `MajorFunction` はドライバー単位なので、**同じ hidbth/hidusb を使う他の全 HID デバイス**（このPCの BT マウスなど）の内部 IOCTL も hidclass を経由しなくなる
- `IO_CLIENT_EXTENSION` / `HIDCLASS_DRIVER_EXTENSION` のレイアウトは非公開で、Windows 更新で変わると BSOD の恐れ

## 2. 推奨方針: 公開 API だけで自前ドライバーを書く（OSS はプロトコル資料として参照）

```
[Magic Trackpad 2]
   │ BT (L2CAP PSM 0x11/0x13)        │ USB (Interface 1, Interrupt IN)
   ▼                                  ▼
 MtpBth.sys (KMDF)                  MtpUsb (UMDF2 or KMDF)
 BTHENUM の HID PDO に機能          USB の MI_01 に機能ドライバー
 ドライバーとして入る（hidbth の置換）│
   └──────────────┬───────────────────┘
                  ▼
           MtpCore（共通ライブラリ: 有効化・レポート解析・ハプティクス）
                  │ VHF (Virtual HID Framework) で仮想 HID を生成
                  ▼
   ┌─────────── 仮想 HID デバイス ───────────┐
   │ TLC1: Precision Touchpad (0x0D/0x05)    │ → Windows 標準のジェスチャー・設定がそのまま動く
   │ TLC2: ベンダー定義 (0xFF00)             │ → 全指の座標・圧力・サイズの生フレーム、ハプティクス制御
   └─────────────────────────────────────────┘
                  │ HID (ユーザーモード)
                  ▼
   MtpAgent（常駐）: Force Touch 判定、独自ジェスチャー、ランチャー呼び出し
   MtpSettings（GUI）: Logicool 風の設定画面
```

- ドライバー層は公開 API（BRB / L2CAP、USB、VHF）だけで構成し、他デバイスに影響しない
- 接続方式の差（有効化レポート、入力 ID 0x31/0x02、ヘッダー長 4/12 バイト）は MtpCore で吸収する
- OSS はレポート形式などの事実を確認する資料として使い、コードは流用しない。これで自前のライセンスを選べる

### 代替案: `AmtPtpHidFilter` をフォーク
最速で動く可能性は高いが、問題 1（ライセンス不明）と問題 2（他デバイスへの影響・Windows 更新時の破損リスク）を抱える。
個人の検証用なら選択肢になるが、社内配布や製品化には勧めない。

## 3. 未確認事項
- USB 接続での記述子と動作（データ通信用ケーブルで要確認）
- ハプティクスの制御レポート（OSS・Linux とも未実装。リバースエンジニアリングが必要）
- この PC で Secure Boot / テスト署名が許可されるか（社内規定の確認が必要。設定変更はユーザー本人が行う）

## 2.5 署名の制約による方針変更（2026-11-16）

EV 証明書やドライバー署名用の Microsoft アカウントは用意しない前提になった。自作カーネルドライバーは
テスト署名モードでしか読み込めないため、通常利用の経路は次の方式 B とする。

- **方式 B（USB、主経路）**: MI_01 に Microsoft 署名済みの WinUSB を当て、ユーザーモードで
  SET_REPORT（Feature 0x02 = `02 01`）を送り、割り込み IN パイプから生フレームを読む。
  署名が要るのは INF カタログだけで、自己署名証明書をローカルで信頼させる（`drivers/MtpUsb`、`docs/INSTALL-USB.md`）
  - 制約: 有線接続が前提。MI_01 が Windows の HID から外れるため、ポインター・スクロール・ジェスチャーは
    自前の常駐アプリが入力注入で作る（Windows 標準の PTP ジェスチャーは使えない）
- **BT（`drivers/MtpBth`）**: Windows のユーザーモード API では L2CAP を扱えないため、カーネルドライバーが必須。
  テスト署名モードで使う検証用として残す

USB でも標準 HID スタックの上からは切り替えレポートを送れないことを実機で確認した（`probe/FINDINGS.md`）。

## 4. 開発環境と現状（2026-11-16）
- 導入済み: VS 2022 Build Tools（MSVC 14.44、Windows SDK 10.0.26100）、WDK 10.0.26100
- WDK の VS 連携コンポーネントは Build Tools に追加できなかった（インストーラー終了コード 5007）。
  そのためドライバーは MSBuild を使わず `drivers/build.cmd` で cl/link を直接呼んでビルドする
- 実装済み
  - `core/`: 有効化レポートと入力レポートの解析（単体テスト 45 件通過。期待値は手計算で、実機データではない）
  - `drivers/MtpBth/`: BT スパイク。HID の L2CAP チャネルを自分で開き、生フレームをユーザーモードへ渡す（ビルド・静的解析済み、実機未検証）
  - `tools/mtpdump/`: 生フレームの表示ツール
  - `drivers/MtpUsb/`: 方式 B の WinUSB 用 INF（テスト署名モードなしで導入できた）
  - `tools/mtpusbdump/`: 方式 B の生フレーム表示ツール
  - `core/src/mtp_haptic.c`: Actuator の振動レポート（強さ・鋭さ・重さ、上限付き）
  - `agent/`: 常駐アプリ `mtpagent`（C、SendInput）とジェスチャー判定 `mtp_gesture`
    - 1 本指でポインター（加速付き）、物理クリックで左、2 本指で押すと右、2 本指でスクロール（ナチュラル既定）、
      クリックしたままのドラッグ
    - 実機（USB）で 30 秒の試験: 約 89 フレーム/秒、ポインターの速さ・向き、右クリック、スクロールの向き・速さとも問題なし（2026-11-16）
    - タスクトレイ常駐版（mtpagent.exe）: 一時停止、ナチュラルスクロール、Force Touch、ログオン時に起動、USB 抜き差しの自動再接続
    - Force Touch: 押し始め約 0.15 秒の見極め、重い振動（Actuator）、中クリックへの割り当て（体感で確認済み）
    - 3・4 本指ジェスチャー: 仮想デスクトップの切り替え、タスクビュー、デスクトップの表示、アプリの切り替え（agent/mtp_actions.c の表で割り当て）
    - まだないもの: 慣性スクロール、設定画面、ランチャー（4 本指の上下を空けてある）、割り当ての変更
- 導入手順: `docs/INSTALL-SPIKE.md`（BT スパイク）、`docs/INSTALL-USB.md`（方式 B）
- 製品版ツール層の言語は未決（候補: C#/.NET、Python 継続、Go）
