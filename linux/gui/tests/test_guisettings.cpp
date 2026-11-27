/* guisettings の単体テスト（QtTest は使わない。失敗したら 1 を返す） */
#include "../guisettings.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstring>

/* 検索の既定はビルドの既定プロファイルで違う（generic: super、lxqt: lxqt-runner） */
#define EXPECT_SEARCH (std::strcmp(MAGITRA_DEFAULT_PROFILE, "lxqt") == 0 ? "exec:lxqt-runner" : "super")

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(data);
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int main()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString conf = dir.filePath("agent.conf");
    /* 手元の LXQt の設定に左右されないよう、XDG の設定の場所をテスト用にする */
    qputenv("XDG_CONFIG_HOME", dir.filePath("home").toUtf8());
    qputenv("XDG_CONFIG_DIRS", (dir.filePath("sys1") + ":" + dir.filePath("sys2")).toUtf8());

    /* 既定値（ファイルなし） */
    Gui::AgentConfig c;
    CHECK(!c.load(conf));
    CHECK(c.getInt("PointerSpeed") == 50);
    CHECK(c.getInt("ForceThreshold") == 200);
    CHECK(c.getInt("HapticStrength") == 0x30);
    CHECK(c.getInt("Swipe4Up") == 12);
    CHECK(c.getInt("TwoFingerDoubleTap") == 11);
    CHECK(c.binding("DesktopNext") == "ctrl+alt+right");
    CHECK(c.binding("Search") == QLatin1String(EXPECT_SEARCH));

    /* 範囲の丸め・不正な値 */
    writeFile(conf, "# コメント\nPointerSpeed=500\nPointerAccel=-3\nForceThreshold=abc\nSwipe3Left=99\n"
                    "Unknown=keep me\nKey.TaskView=ctrl+shift+f5\nKey.Search=bogus+key\n");
    CHECK(c.load(conf));
    CHECK(c.getInt("PointerSpeed") == 100);
    CHECK(c.getInt("PointerAccel") == 0);
    CHECK(c.getInt("ForceThreshold") == 200);
    CHECK(c.getInt("Swipe3Left") == 0);
    CHECK(c.binding("TaskView") == "ctrl+shift+f5");
    CHECK(c.binding("Search") == QLatin1String(EXPECT_SEARCH)); /* 不正な束縛は既定に戻る */

    /* 書き戻し: 知らない行とコメントを残す */
    c.setInt("PointerSpeed", 30);
    c.setBinding("TaskView", ""); /* 消す */
    c.setBinding("AppSwitch", "exec:rofi -show window");
    c.setBinding("ZoomReset", "ctrl+0"); /* 既定と同じなので書かない */
    CHECK(c.save(conf));
    const QByteArray out = readFile(conf);
    CHECK(out.contains("# コメント"));
    CHECK(out.contains("Unknown=keep me"));
    CHECK(out.contains("PointerSpeed=30"));
    CHECK(!out.contains("PointerSpeed=500"));
    CHECK(!out.contains("Key.TaskView"));
    CHECK(!out.contains("Key.ZoomReset"));
    CHECK(out.contains("Key.AppSwitch=exec:rofi -show window"));
    Gui::AgentConfig c2;
    CHECK(c2.load(conf));
    CHECK(c2.getInt("PointerSpeed") == 30);
    CHECK(c2.binding("AppSwitch") == "exec:rofi -show window");

    c2.resetAll();
    CHECK(c2.getInt("PointerSpeed") == 50);
    CHECK(c2.save(conf));
    CHECK(readFile(conf).contains("Unknown=keep me"));

    /* 束縛の検証 */
    CHECK(Gui::isValidBinding("ctrl+alt+right"));
    CHECK(Gui::isValidBinding("CTRL+Alt+F12"));
    CHECK(Gui::isValidBinding("super"));
    CHECK(Gui::isValidBinding("playpause"));
    CHECK(Gui::isValidBinding("exec:notify-send hi"));
    CHECK(!Gui::isValidBinding(""));
    CHECK(!Gui::isValidBinding("exec:"));
    CHECK(!Gui::isValidBinding("ctrl+"));
    CHECK(!Gui::isValidBinding("ctrl+f13"));
    CHECK(!Gui::isValidBinding("ctrl+a+b"));
    CHECK(!Gui::isValidBinding("ctrl+foo"));

    /* ランチャーの項目 */
    const QString lconf = dir.filePath("launcher.conf");
    bool browser = true;
    CHECK(Gui::loadLauncherItems(lconf, &browser).isEmpty());
    CHECK(browser);
    QList<Gui::LauncherItem> items;
    Gui::LauncherItem a;
    a.kind = "url";
    a.name = "例\tタブ入り";
    a.target = "https://www.example.com/x";
    items << a;
    CHECK(Gui::saveLauncherItems(lconf, items, false));
    const QList<Gui::LauncherItem> back = Gui::loadLauncherItems(lconf, &browser);
    CHECK(!browser);
    CHECK(back.size() == 1);
    CHECK(back.value(0).kind == "url");
    CHECK(back.value(0).name == "例 タブ入り");
    CHECK(Gui::defaultItemName("url", a.target) == "example.com");
    CHECK(Gui::defaultItemName("folder", "/home/x/Documents/") == "Documents");
    CHECK(Gui::looksLikeUrl("https://a.b"));
    CHECK(!Gui::looksLikeUrl("/usr/bin/x"));

    /* Exec 行 */
    CHECK(Gui::execArguments("firefox %u") == QStringList({ "firefox" }));
    CHECK(Gui::execArguments("env A=1 \"/opt/my app/run\" --f %F 100%%") == QStringList({ "env", "A=1", "/opt/my app/run", "--f", "100%" }));

    /* lxqt-globalkeys のキーの書き方 */
    CHECK(Gui::fromQtKeySequence("Meta+D") == "super+d");
    CHECK(Gui::fromQtKeySequence("Super_L") == "super");
    CHECK(Gui::fromQtKeySequence("Control+Alt+Left") == "ctrl+alt+left");
    CHECK(Gui::fromQtKeySequence("Alt+F1") == "alt+f1");
    CHECK(Gui::fromQtKeySequence("XF86Search").isEmpty()); /* 送れないキー */

    /* lxqt-globalkeys の設定がないときは表の既定 */
    CHECK(Gui::defaultBinding("lxqt", "TaskView") == "alt+f1");
    CHECK(Gui::defaultBinding("lxqt", "ShowDesktop") == "super+d");

    /* Lubuntu 26.04（/etc/xdg/xdg-Lubuntu/lxqt/globalkeyshortcuts.conf と同じ形）: メニューは Super 単独。2 番目の場所でも見つける */
    QDir().mkpath(dir.filePath("sys2/lxqt"));
    writeFile(dir.filePath("sys2/lxqt/globalkeyshortcuts.conf"),
              "[General]\nversion=1\n\n[Meta%2BD.15]\nComment=Show desktop\nEnabled=true\npath=/panel/showdesktop/show_hide\n\n"
              "[Super_L.32]\nComment=Show/hide main menu\nEnabled=true\npath=/panel/fancymenu/show_hide\n");
    CHECK(Gui::defaultBinding("lxqt", "TaskView") == "super");
    CHECK(Gui::defaultBinding("lxqt", "ShowDesktop") == "super+d");
    CHECK(Gui::defaultBinding("generic", "TaskView") == "super+w"); /* generic は読まない */

    /* 利用者の設定があれば、それだけを使う（無効にした割り当ては使わない） */
    QDir().mkpath(dir.filePath("home/lxqt"));
    writeFile(dir.filePath("home/lxqt/globalkeyshortcuts.conf"),
              "[Alt%2BF1.1]\nEnabled=false\npath=/panel/mainmenu/show_hide\n\n[Control%2BEscape.2]\nEnabled=true\npath=/panel/mainmenu/show_hide\n");
    CHECK(Gui::defaultBinding("lxqt", "TaskView") == "ctrl+escape");
    CHECK(Gui::defaultBinding("lxqt", "ShowDesktop") == "super+d"); /* 利用者の設定にないので表の既定 */

    if (g_failures == 0) std::puts("OK");
    return g_failures == 0 ? 0 : 1;
}
