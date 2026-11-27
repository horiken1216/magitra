# Linux 版

Linux では、カーネルの hid-magicmouse が USB と Bluetooth のどちらでも Magic Trackpad を扱う。
MAGITRA の Linux 版は、トラックパッドの生レポートを hidraw から読み、Windows 版と同じ解析・ジェスチャー・慣性のコード（`core/`・`agent/mtp_gesture.c`・`agent/mtp_momentum.c`）で処理して、
uinput で作った仮想のポインターとキーボードから入力を送る。ドライバーの差し替えも署名も要らず、**Bluetooth でもそのまま使える**。

**実機での確認はまだ**（GitHub Actions の Ubuntu でビルドと単体テストだけ確かめている）。

## 構成

| もの | 役目 |
|---|---|
| `magitrad` | 常駐アプリ（C）。hidraw を読み、カーネル側の入力デバイスを `EVIOCGRAB` で止め（ポインターが二重に動かないように）、uinput で入力を作る。Force Touch で振動させる |
| `magitra-gui` | トレイアイコン（引数なし）・設定画面（`--settings`）・ランチャー（`--launcher`）。Qt 6 |
| `70-magitra.rules` | ログイン中のユーザーに、トラックパッドの hidraw・input と `/dev/uinput` を使わせる udev ルール（`TAG+="uaccess"`） |
| `magitrad.desktop`・`magitra-tray.desktop` | ログイン時の自動起動（XDG autostart） |

設定は `~/.config/magitra/agent.conf`（名前・範囲・既定値は Windows 版のレジストリの値と同じ。`agent/mtp_settings.h`）、
ランチャーに並べるものは `~/.config/magitra/launcher.conf`。設定画面で変えるとすぐ反映される。
常駐アプリとは `$XDG_RUNTIME_DIR/magitra/agent.sock` でやりとりする（`RELOAD`・`HAPTIC`・`PAUSE`・`RESUME`・`STATUS`・`QUIT`）。

## ビルドと導入

必要なもの: CMake 3.16 以上、C コンパイラー、Qt 6.4 以上（Widgets・Network）。Ubuntu 24.04 なら:

```sh
sudo apt install build-essential cmake qt6-base-dev
cmake -S linux -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build
sudo cmake --install build
sudo udevadm control --reload && sudo udevadm trigger
```

udev ルールは `/usr/lib/udev/rules.d`、自動起動は `/etc/xdg/autostart` に入る（udev と XDG autostart はここしか読まないので、`CMAKE_INSTALL_PREFIX` に関係なくこの場所。
変えるときは `-DMAGITRA_UDEV_DIR=…`・`-DMAGITRA_AUTOSTART_DIR=…`）。
GUI が要らなければ `-DMAGITRA_BUILD_GUI=OFF`。

導入したらログインし直す（または `magitrad &` と `magitra-gui &` を起動する）。トレイに虎のアイコンが出れば動いている。
うまく動かないときは、端末で `magitrad --log` を起動すると、接続の様子と届いたフレーム・イベントが出る。

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
- **ロック画面でも動く**: uinput の入力はロック画面にも届く。常駐アプリを止めると（トレイの「終了」・一時停止）、カーネルの hid-magicmouse の動きに戻る
- **管理者権限のウィンドウ**: Linux には UIPI に当たる仕組みがないので、UIAccess 版のような区別はない
- **/dev/uinput の権限**: udev ルールは、ログイン中のユーザーに `/dev/uinput` を開かせる。これを開けるプログラムは、本物と区別できないキー入力を送れる。
  共有の PC では、入れるかどうかを判断してから導入すること
