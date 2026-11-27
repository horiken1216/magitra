/* guisettings の単体テスト（QtTest は使わない。失敗したら 1 を返す） */
#include "../guisettings.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

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

    /* 既定値（ファイルなし） */
    Gui::AgentConfig c;
    CHECK(!c.load(conf));
    CHECK(c.getInt("PointerSpeed") == 50);
    CHECK(c.getInt("ForceThreshold") == 200);
    CHECK(c.getInt("HapticStrength") == 0x30);
    CHECK(c.getInt("Swipe4Up") == 12);
    CHECK(c.getInt("TwoFingerDoubleTap") == 11);
    CHECK(c.binding("DesktopNext") == "ctrl+alt+right");
    CHECK(c.binding("Search") == "super");

    /* 範囲の丸め・不正な値 */
    writeFile(conf, "# コメント\nPointerSpeed=500\nPointerAccel=-3\nForceThreshold=abc\nSwipe3Left=99\n"
                    "Unknown=keep me\nKey.TaskView=ctrl+shift+f5\nKey.Search=bogus+key\n");
    CHECK(c.load(conf));
    CHECK(c.getInt("PointerSpeed") == 100);
    CHECK(c.getInt("PointerAccel") == 0);
    CHECK(c.getInt("ForceThreshold") == 200);
    CHECK(c.getInt("Swipe3Left") == 0);
    CHECK(c.binding("TaskView") == "ctrl+shift+f5");
    CHECK(c.binding("Search") == "super"); /* 不正な束縛は既定に戻る */

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

    if (g_failures == 0) std::puts("OK");
    return g_failures == 0 ? 0 : 1;
}
