/*
 * magitra-gui - トレイ・設定画面・ランチャー（1 つの実行ファイル）
 *
 *   引数なし   トレイ（常駐）。ランチャーと設定画面の持ち主にもなる
 *   --settings 設定画面（トレイが動いていればそちらに任せる）
 *   --launcher ランチャーの表示・非表示（トレイが動いていればそちらに任せる）
 */
#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QPalette>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QTimer>
#include <cstdio>
#include <unistd.h>

#include "agentclient.h"
#include "guisettings.h"
#include "launcherwindow.h"
#include "settingswindow.h"

namespace {

QString instanceName()
{
    return QStringLiteral("magitra-gui-") + QString::number(static_cast<unsigned>(getuid()));
}

// 動いているトレイに用件を伝える。つながらなければ false
bool forward(const QByteArray &command)
{
    QLocalSocket s;
    s.connectToServer(instanceName());
    if (!s.waitForConnected(300)) return false;
    s.write(command + '\n');
    s.waitForBytesWritten(300);
    s.disconnectFromServer();
    if (s.state() != QLocalSocket::UnconnectedState) s.waitForDisconnected(300);
    return true;
}

class Tray {
public:
    Tray()
    {
        QAction *settings = m_menu.addAction(QStringLiteral("設定…"));
        QAction *launcher = m_menu.addAction(QStringLiteral("ランチャーを開く"));
        m_pause = m_menu.addAction(QStringLiteral("一時停止"));
        QAction *haptic = m_menu.addAction(QStringLiteral("振動を試す"));
        m_menu.addSeparator();
        QAction *quit = m_menu.addAction(QStringLiteral("終了（一時停止して閉じる）"));

        QObject::connect(settings, &QAction::triggered, qApp, [this] { showSettings(); });
        QObject::connect(launcher, &QAction::triggered, qApp, [this] { showLauncher(); });
        QObject::connect(m_pause, &QAction::triggered, qApp, [this] {
            m_client.request(m_paused ? QStringLiteral("RESUME") : QStringLiteral("PAUSE"));
            update();
        });
        QObject::connect(haptic, &QAction::triggered, qApp, [this] {
            Gui::AgentConfig cfg;
            cfg.load(Gui::agentConfPath());
            m_client.haptic(cfg.getInt(QStringLiteral("HapticStrength")), cfg.getInt(QStringLiteral("HapticSharpness")),
                            cfg.getInt(QStringLiteral("HapticDepth")));
        });
        QObject::connect(quit, &QAction::triggered, qApp, [this] {
            m_client.request(QStringLiteral("PAUSE")); // サービスは動いたまま止める（カーネルのドライバーの動きに戻る）
            QCoreApplication::quit();
        });

        m_icon.setContextMenu(&m_menu);
        QObject::connect(&m_icon, &QSystemTrayIcon::activated, qApp, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger) showSettings();
        });
        QObject::connect(&m_link, &AgentLink::actionReceived, qApp, [this](int n) { onAction(n); });
        QObject::connect(&m_timer, &QTimer::timeout, qApp, [this] { update(); });
        m_timer.start(2000);
        update();
        m_icon.show();
    }

    void showSettings()
    {
        if (!m_settings) {
            m_settings = new SettingsWindow;
            QObject::connect(m_settings, &SettingsWindow::launcherRequested, qApp, [this] { showLauncher(); });
        }
        m_settings->show();
        m_settings->raise();
        m_settings->activateWindow();
    }

    void toggleLauncher() { launcherWindow()->toggle(); }

    // サービスは exec: とランチャーを実行しない。ユーザー側のここで実行する
    void onAction(int n)
    {
        Gui::AgentConfig cfg;
        cfg.load(Gui::agentConfPath());
        QString command;
        if (n == 12) {
            command = cfg.launcherCommand();
            if (command == QLatin1String("magitra-gui --launcher")) {
                toggleLauncher();
                return;
            }
        } else {
            for (const Gui::KeyAction &a : Gui::keyActions()) {
                if (a.id != n) continue;
                const QString b = cfg.binding(QString::fromLatin1(a.name));
                if (b.startsWith(QLatin1String("exec:"))) command = b.mid(5).trimmed();
            }
        }
        if (!command.isEmpty()) QProcess::startDetached(QStringLiteral("/bin/sh"), { QStringLiteral("-c"), command });
    }
    void showLauncher() { launcherWindow()->showLauncher(); }

private:
    LauncherWindow *launcherWindow()
    {
        if (!m_launcher) {
            m_launcher = new LauncherWindow(false);
            QObject::connect(m_launcher, &LauncherWindow::settingsRequested, qApp, [this] { showSettings(); });
        }
        return m_launcher;
    }

    void update()
    {
        m_link.ensureConnected();
        const AgentClient::Status st = m_client.status();
        QString text;
        if (!st.running) text = QStringLiteral("常駐サービスに接続できません");
        else if (st.paused) text = QStringLiteral("一時停止中");
        else if (st.connected == QLatin1String("usb")) text = QStringLiteral("USB で接続中");
        else if (st.connected == QLatin1String("bt")) text = QStringLiteral("Bluetooth で接続中");
        else text = QStringLiteral("未接続");
        m_paused = st.running && st.paused;
        m_pause->setText(m_paused ? QStringLiteral("再開") : QStringLiteral("一時停止"));
        m_pause->setEnabled(st.running);
        m_icon.setToolTip(QStringLiteral("MAGITRA: ") + text);

        // 明るいパネルには黒い虎（light）、暗いパネルには白い虎（dark）。つながっているとき以外は薄い版
        const bool light = QApplication::palette().color(QPalette::Window).lightness() > 127;
        const bool active = st.running && !st.paused && st.connected != QLatin1String("none");
        const QString name = QStringLiteral(":/tray-") + (light ? QStringLiteral("light") : QStringLiteral("dark"))
                             + (active ? QString() : QStringLiteral("-idle")) + QStringLiteral(".png");
        if (name != m_iconName) {
            m_iconName = name;
            m_icon.setIcon(QIcon(name));
        }
    }

    AgentClient m_client;
    AgentLink m_link;
    QMenu m_menu;
    QSystemTrayIcon m_icon;
    QAction *m_pause = nullptr;
    QTimer m_timer;
    QString m_iconName;
    bool m_paused = false;
    SettingsWindow *m_settings = nullptr;
    LauncherWindow *m_launcher = nullptr;
};

} // namespace

int main(int argc, char *argv[])
{
    /*
     * 常駐サービスは、ログイン中のユーザーからの接続しか受け付けない（root は弾かれる）。
     * root で起動すると、何も知らされないまま動かないので、起動しない
     */
    if (geteuid() == 0) {
        std::fputs("magitra-gui: root では起動しません。ログインしているユーザーのまま（sudo を付けずに）起動してください\n", stderr);
        return 1;
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("magitra-gui"));
    QApplication::setDesktopFileName(QStringLiteral("magitra-gui"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/magitra.png")));

    enum { ModeTray, ModeSettings, ModeLauncher } mode = ModeTray;
    for (const QString &a : app.arguments().mid(1)) {
        if (a == QLatin1String("--settings")) mode = ModeSettings;
        else if (a == QLatin1String("--launcher")) mode = ModeLauncher;
    }

    if (mode == ModeSettings) {
        if (forward("settings")) return 0;
        SettingsWindow w;
        QObject::connect(&w, &SettingsWindow::launcherRequested, &w, [] {
            QProcess::startDetached(QCoreApplication::applicationFilePath(), { QStringLiteral("--launcher") });
        });
        w.show();
        return app.exec();
    }
    if (mode == ModeLauncher) {
        if (forward("launcher")) return 0;
        LauncherWindow w(true);
        QObject::connect(&w, &LauncherWindow::settingsRequested, &w, [] {
            QProcess::startDetached(QCoreApplication::applicationFilePath(), { QStringLiteral("--settings") });
        });
        w.showLauncher();
        return app.exec();
    }

    // トレイ: すでに動いていれば何もしない
    if (forward("ping")) return 0;
    app.setQuitOnLastWindowClosed(false);
    QLocalServer::removeServer(instanceName());
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    Tray tray;
    if (server.listen(instanceName())) {
        QObject::connect(&server, &QLocalServer::newConnection, &server, [&server, &tray] {
            while (QLocalSocket *s = server.nextPendingConnection()) {
                QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QLocalSocket::readyRead, s, [s, &tray] {
                    while (s->canReadLine()) {
                        const QByteArray cmd = s->readLine().trimmed();
                        if (cmd == "settings") tray.showSettings();
                        else if (cmd == "launcher") tray.toggleLauncher();
                    }
                });
            }
        });
    }
    return app.exec();
}
