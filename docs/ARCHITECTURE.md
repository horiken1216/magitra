# 仕組み

## 全体

```
Magic Trackpad（USB）
 ├─ MI_01 ──[WinUSB]──────────▶ mtpagent.exe（常駐アプリ、ユーザーのセッション）
 │   マルチタッチの生フレーム       │  読み取りスレッド: フレーム → core（解析）→ mtp_gesture（判定）→ SendInput
 │                                  │  慣性スレッド: 8ms ごとに減速しながらホイールを送る（mtp_momentum）
 ├─ MI_02 ──[標準の HID]◀───────────┤  振動: 出力レポート 0x53（core/mtp_haptic.c）
 └─ MI_00/03 は標準の HID のまま    │
                                    ├──▶ mtplauncher.exe（ランチャー、隠れたまま常駐）
 mtpsettings.exe（設定画面）────────┤      登録メッセージ MTP_LAUNCHER で表示
   HKCU\Software\MAGITRA\Agent に保存し、MTP_SETTINGS_CHANGED で知らせる
                                    │
 mtpswitchsvc.exe（サービス MagitraSwitch、システム権限）
   ロック・ログオフ・起動時は MI_01 を標準の HidUsb に、ログオン・解除後は WinUSB に切り替える。
   ログオンを見つけたら、そのユーザーのトークンで mtpagent.exe を起動する
```

## なぜこの形か

- **カーネルドライバーを使わない**: 自作のカーネルドライバーは、EV 証明書と Microsoft の署名がなければテスト署名モードでしか読み込めない。
  そこで、USB の MI_01 に **Microsoft 署名済みの WinUSB** を INF で当て、ユーザーモードで生フレームを読む。
  署名が要るのは INF のカタログだけで、インストーラーがその場限りの証明書で署名する
- **入力は SendInput で作る**: WinUSB を当てた MI_01 は Windows のマウスとして扱われないので、ポインター・クリック・スクロールは常駐アプリが作る。
  Windows 標準の Precision Touchpad の機能（設定・ジェスチャー）は使えない代わりに、Force Touch や独自のジェスチャーを自由に作れる
- **UIAccess 版**: 普通のプロセスの SendInput は、UIPI により管理者権限のウィンドウに届かない。
  `uiAccess="true"` の常駐アプリを、信頼された証明書で署名して `C:\Program Files` に置くと届く（画面読み上げソフトと同じ仕組み）
- **ロック画面**: 常駐アプリはユーザーのセッションでしか動かないので、ロック画面・ログオン画面ではサービスが MI_01 を標準の HID に戻す。
  常駐アプリはロックされると WinUSB のハンドルを閉じる（開いたままだと、ドライバーの切り替えが「登録だけ」で終わる。下の「落とし穴」）
- **Bluetooth は非対応**: Windows の HID スタックが指ごとのレポート（0x31）を捨てるため、ユーザーモードでは取り出せない
  （[research/FINDINGS.md](research/FINDINGS.md)）

## データの流れ

1. 常駐アプリが MI_01 に Feature レポート `02 01` を送り、マルチタッチのモードに切り替える
2. 割り込み IN パイプから約 90 フレーム/秒で生フレームを読む（ヘッダー 12 バイト＋指 9 バイト × 本数。`core/src/mtp_report.c`）
3. `agent/mtp_gesture.c` が、指の本数・動き・圧力・物理クリックから、ポインター・ボタン・スクロール・Force Touch・スワイプ・
   慣性の開始・ピンチ・ダブルタップのイベントを作る（OS に依存しない。`agent/tests/` で単体テスト）
4. イベントを `SendInput` で送る。スワイプなどの操作は `agent/mtp_actions.c` の表でキー操作に変える
5. Force Touch では MI_02（標準の HID のまま）に振動の出力レポートを書く

## プロセスとやりとり

| プロセス | 権限 | やりとり |
|---|---|---|
| `mtpagent.exe` | ユーザー（UIAccess 版は uiAccess 付き） | 見えないウィンドウ（クラス `MtpAgentTrayWindow`、題名 `MTP Agent`）で、`MTP_SETTINGS_CHANGED`（設定の読み直し）・`MTP_HAPTIC_TEST`（振動の試し打ち）を受ける |
| `mtpsettings.exe` | ユーザー | 設定を `HKCU\Software\MAGITRA\Agent` に書き、常駐アプリに知らせる |
| `mtplauncher.exe` | ユーザー | 常駐アプリが起動時に隠れたまま起動する。題名 `MTP Launcher` のウィンドウで `MTP_LAUNCHER`（1 表示・2 隠す・3 終了）を受ける。アプリの起動はエクスプローラーの `IShellDispatch2.ShellExecute` に頼む（UIAccess を引き継がせないため） |
| `mtpswitchsvc.exe` | システム | 1 秒ごとにコンソールのセッションを見て、ドライバーを切り替える。ログオンを見つけたら `WTSQueryUserToken` → `CreateProcessAsUser` で常駐アプリを起動する |
| `mtpsetup.exe` | 管理者 | 中身を埋め込んだインストーラー。処理本体は `installer/setup-core.ps1` |

設定の名前・範囲・既定値は `agent/mtp_settings.h`（設定画面 `settings/MtpSettings.cs` と同じにしておく）。
操作の番号は `agent/mtp_actions.h`（保存済みの設定の意味が変わるので、既存の番号は変えない）。

## 落とし穴

- **ドライバーの切り替えが「登録だけ」で終わる**: どれかのプロセスが機器を開いたままだと、`UpdateDriverForPlugAndPlayDevices` は
  新しいドライバーを登録するだけで、機器は古いドライバーのまま動く（`DN_NEED_RESTART`）。サービス名だけ見ると成功に見える。
  `agent/mtp_switch.c` はこれを見つけて `DIF_PROPERTYCHANGE` で機器を動かし直す
- **シャットダウン中は切り替えられない**: PnP に断られる（`0xE0000223`）。起動したあとにサービスが標準の HID に戻す
- **ログオン時の自動起動が遅い**: Windows の Run は 1 つずつ順に起動するので 40 秒以上かかることがあった。サービスが起動する
- **普通のドラッグでも圧力が最大近くになる**: Force Touch は「押し始め 0.15 秒以内に強く押し、2mm 以上動いていない」ときだけにしている

詳しい調査記録は [research/FINDINGS.md](research/FINDINGS.md)。最初の設計の検討（既存 OSS の調査を含む）は [research/DESIGN-NOTES.md](research/DESIGN-NOTES.md)。
