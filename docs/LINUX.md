# Linux 版

Linux では、カーネルの hid-magicmouse が USB と Bluetooth のどちらでも Magic Trackpad を扱う。
MAGITRA の Linux 版は、トラックパッドの生レポートを hidraw から読み、Windows 版と同じ解析・ジェスチャー・慣性のコード（`core/`・`agent/mtp_gesture.c`・`agent/mtp_momentum.c`）で処理して、
uinput で作った仮想のポインターとキーボードから入力を送る。ドライバーの差し替えも署名も要らず、**Bluetooth でもそのまま使える**。

**実機での確認はまだ**（GitHub Actions の Ubuntu でビルドと単体テストだけ確かめている）。

## 構成

| もの | 役目 |
|---|---|
| `magitrad` | 常駐サービス（C）。systemd のシステムサービスとして root・capability なしで動く。hidraw を読み、カーネル側の入力デバイスを `EVIOCGRAB` で止め（ポインターが二重に動かないように）、uinput で入力を作る。Force Touch で振動させる |
| `magitra-gui` | トレイアイコン（引数なし）・設定画面（`--settings`）・ランチャー（`--launcher`）。Qt 6 |
| `magitrad.service` | `magitrad` の systemd ユニット（`/usr/lib/systemd/system`） |
| `magitra-tray.desktop` | トレイのログイン時の自動起動（XDG autostart） |

設定は `~/.config/magitra/agent.conf`（名前・範囲・既定値は Windows 版のレジストリの値と同じ。`agent/mtp_settings.h`）、
ランチャーに並べるものは `~/.config/magitra/launcher.conf`。設定画面で変えるとすぐ反映される。
サービスは `$HOME` のファイルを読まない。GUI が `agent.conf` の中身を `CONFIG` で送り、サービスはそれを解析して使う。
サービスとは `/run/magitra/agent.sock` でやりとりする（`CONFIG`・`HAPTIC`・`PAUSE`・`RESUME`・`STATUS`・`SUBSCRIBE`）。

## ビルドと導入

必要なもの: CMake 3.16 以上、C コンパイラー、Qt 6.4 以上（Widgets・Network）。Ubuntu 24.04 なら:

```sh
sudo apt install build-essential cmake pkg-config libsystemd-dev qt6-base-dev
cmake -S linux -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build
sudo cmake --install build
sudo systemctl enable --now magitrad
```

サービスファイルは `/usr/lib/systemd/system`、トレイの自動起動は `/etc/xdg/autostart` に入る（systemd と XDG autostart はここしか読まないので、`CMAKE_INSTALL_PREFIX` に関係なくこの場所。
変えるときは `-DMAGITRA_SYSTEMD_DIR=…`・`-DMAGITRA_AUTOSTART_DIR=…`）。
GUI が要らなければ `-DMAGITRA_BUILD_GUI=OFF`。

導入したらログインし直す（または `magitra-gui &` を起動する）。トレイに虎のアイコンが出れば動いている。
うまく動かないときは、サービスを止めて端末で動かすと、接続の様子と届いたフレーム・イベントが出る。

```sh
sudo systemctl stop magitrad && sudo magitrad --log
```

## 権限の分け方

`/dev/uinput` を使えるプログラムは、本物と区別できないキー入力を、他の VT・ログイン画面・ロック画面にまで送れる。
udev の `uaccess` でログイン中のユーザーに `/dev/uinput` を渡すと、そのユーザーのどのプロセスでも仮想キーボードを作れる。
ユーザーが乗っ取られると、別の VT の root シェルへの入力まで届き、root まで昇格できてしまう。
そこで、ユーザーのプロセスには `/dev/uinput` も生のデバイスも渡さない。

- `magitrad` は root のシステムサービスで、capability を持たない（root 所有の 0600 のノードは capability なしで開ける）。
  ファイルシステムは読み取り専用、`$HOME` は見えず、ネットワークもなく、開けるデバイスは hidraw・input・uinput だけ（`magitrad.service`）
- ソケットは誰でも接続できる（0666）が、接続のたびと要求のたびに `SO_PEERCRED` で相手の uid を調べ、`seat0` のアクティブなセッションのユーザー（`sd_seat_get_active`）と一致したときだけ受け付ける。
  違えば `ERR denied` を返して切る。ログイン前（アクティブなセッションなし）は既定の設定で動き、ポインターは使える
- アクティブなユーザーが替わる（ユーザー切り替え・ログアウト）と、サービスは設定を既定に戻し、一時停止を解き、押したままのボタンを離し、すべての接続を閉じる
- サービスはコマンドを実行しない。中クリックとキー操作は uinput で送るが、ランチャーと `exec:` の割り当ては、購読している GUI に `ACTION <n>` と知らせるだけで、
  GUI（ユーザー権限）が実行する。トレイが動いていなければ何も起きない
- トレイの「終了（一時停止して閉じる）」は、サービスに `PAUSE` を送ってから閉じる。サービスは一時停止のまま動き続け、カーネルのドライバーの動きに戻る

## 操作のキー

仮想デスクトップの切り替えなどの操作は、キー操作（例 `ctrl+alt+right`）か、`exec:` で始まるコマンドとして送る。
デスクトップ環境ごとにショートカットが違うので、設定画面の「操作のキー」で変えられる（`agent.conf` の `Key.<操作名>`）。

| 操作 | 既定（generic） |
|---|---|
| 右隣・左隣の仮想デスクトップ | `ctrl+alt+right`・`ctrl+alt+left` |
| タスクビュー | `super+w` |
| デスクトップの表示 | `super+d` |
| アプリの切り替え | `alt+tab` |
| 戻る・進む | `alt+left`・`alt+right` |
| 再生・一時停止 | `playpause` |
| 検索 | `super` |
| 拡大率を戻す | `ctrl+0` |

中クリックはそのまま中ボタン、ランチャーは `LauncherCommand`（既定 `magitra-gui --launcher`）を実行する。

## Windows 版との違い・注意

- **ポインターの加速が二重になることがある**: 仮想ポインターはマウスとして扱われ、libinput の加速もかかる。
  速すぎるときは、デスクトップのマウスの設定で「MAGITRA pointer」の加速を「なし（flat）」にし、MAGITRA 側の速さ・加速で合わせる
- **ロック画面でも動く**: uinput の入力はロック画面にも届く。一時停止すると（トレイの「終了」も一時停止する）、カーネルの hid-magicmouse の動きに戻る
- **管理者権限のウィンドウ**: Linux には UIPI に当たる仕組みがないので、UIAccess 版のような区別はない
