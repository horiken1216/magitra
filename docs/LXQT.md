# LXQt 版

LXQt 版は Linux 版（[LINUX.md](LINUX.md)）と同じプログラムで、次の点だけが違う。

- **操作のキーの既定値が LXQt のもの**（下の表）。`-DMAGITRA_DEFAULT_PROFILE=lxqt` でビルドする
- **設定画面が LXQt の設定センター（lxqt-config）の「LXQt の設定」の欄に出る**（`.desktop` の `Categories` に `DesktopSettings` と `LXQt` を入れる。Linux 版のままだと「その他の設定」の欄に出る）
- **Debian パッケージの名前が `magitra-lxqt`**

ほかの Linux 版を使っていても、`~/.config/magitra/agent.conf` に `Profile=lxqt` と書けば同じ既定値になる。

**実機での確認はまだ**（GitHub Actions の Ubuntu でビルドと単体テストだけ確かめている）。

## ビルドと導入

```sh
sudo apt install build-essential cmake qt6-base-dev
cmake -S linux -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DMAGITRA_DEFAULT_PROFILE=lxqt
cmake --build build
ctest --test-dir build
cd build && cpack -G DEB && sudo apt install ./magitra-lxqt_*.deb
```

パッケージを入れると、常駐サービス `magitrad`（systemd のシステムサービス）が有効になって起動する。ログインし直すと、LXQt のセッションがトレイを自動で起動する（XDG autostart）。
GitHub Actions の「linux」ワークフローの成果物（`magitra-lxqt-deb`）にも、ビルドした `.deb` がある。

## 操作のキー（LXQt の既定）

| 操作 | 既定 | LXQt での意味 |
|---|---|---|
| 右隣・左隣の仮想デスクトップ | `ctrl+alt+right`・`ctrl+alt+left` | openbox（LXQt の `lxqt-rc.xml`）の `C-A-Right`・`C-A-Left` |
| タスクビュー | lxqt-globalkeys に割り当てたメニューのキー（なければ `alt+f1`） | LXQt にタスクビューはないので、パネルのメインメニューを開く（下の「Lubuntu」） |
| デスクトップの表示 | lxqt-globalkeys に割り当てたキー（なければ `super+d`） | openbox の `W-d`、labwc の `W-d`（`lxqt-qdbus showdesktop`） |
| アプリの切り替え | `alt+tab` | openbox・labwc の `A-Tab` |
| 検索 | `exec:lxqt-runner` | lxqt-runner を開く（すでに動いていれば前に出る） |
| 戻る・進む・再生・拡大率を戻す | Linux 版と同じ | |

KWin など別のウィンドウマネージャーを使っているときは、設定画面の「操作のキー」で合わせる。

### メニューとデスクトップの表示のキーは、利用者の LXQt の設定から取る

メインメニューとデスクトップの表示は LXQt のグローバルショートカット（lxqt-globalkeys）に割り当ててあり、ディストリビューションや利用者によって違う。
そこで、自分で割り当てていなければ、トレイ（`magitra-gui`）が lxqt-globalkeys の設定を読み、実際のキーを常駐サービスに送る。
読むのは `~/.config/lxqt/globalkeyshortcuts.conf`、なければ `$XDG_CONFIG_DIRS` の `lxqt/globalkeyshortcuts.conf` のうち最初の 1 つ（lxqt-globalkeys と同じ）。
LXQt の設定で割り当てを変えたら、トレイを起動し直すと反映される。

### Lubuntu

Lubuntu 26.04 の既定（`lubuntu-default-settings` の `/etc/xdg/xdg-Lubuntu`）は、メインメニュー（fancymenu）が **Super 単独**、デスクトップの表示が `Meta+D`。
上の仕組みでそのまま合う。仮想デスクトップ（openbox の `C-A-Left`・`C-A-Right`）とアプリの切り替え（`A-Tab`）は LXQt と同じ。
Lubuntu 26.04 には `magitra-lxqt` の `.deb` がそのまま入る（CI で `ubuntu:26.04` に入れて確かめている）。

## Wayland（labwc）で仮想デスクトップを切り替える

LXQt の Wayland セッション（labwc）の既定の設定には、仮想デスクトップを切り替えるキーがない。
`~/.config/labwc/rc.xml` の `<keyboard>` に次を足すと、MAGITRA の既定（`ctrl+alt+right`・`ctrl+alt+left`）で切り替わる。

```xml
<keybind key="C-A-Right"><action name="GoToDesktop" to="right" wrap="yes" /></keybind>
<keybind key="C-A-Left"><action name="GoToDesktop" to="left" wrap="yes" /></keybind>
```

足したら `labwc --reconfigure` で読み直す。

## 確かめた資料

- openbox: [lxqt-common-deprecated の `config/openbox/lxqt-rc.xml`](https://github.com/lxqt/lxqt-common-deprecated/blob/master/config/openbox/lxqt-rc.xml)
- labwc: [lxqt-wayland-session の `configurations/labwc/rc.xml`](https://github.com/lxqt/lxqt-wayland-session/blob/master/configurations/labwc/rc.xml)
- メインメニュー: [lxqt-panel の `plugin-mainmenu/lxqtmainmenu.cpp`](https://github.com/lxqt/lxqt-panel/blob/master/plugin-mainmenu/lxqtmainmenu.cpp)（`DEFAULT_SHORTCUT "Alt+F1"`）
- 設定センターの分類: [lxqt-menu-data の `menu/lxqt-config.menu`](https://github.com/lxqt/lxqt-menu-data/blob/master/menu/lxqt-config.menu)
