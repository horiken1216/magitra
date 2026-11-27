#include "launcherwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

Gui::LauncherItem defaultBrowserItem()
{
    Gui::LauncherItem item;
    QProcess p;
    p.start(QStringLiteral("xdg-settings"), { QStringLiteral("get"), QStringLiteral("default-web-browser") });
    if (!p.waitForStarted(1000) || !p.waitForFinished(2000)) return item;
    const QString id = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    if (id.isEmpty() || id.contains(QLatin1Char('/'))) return item;
    const QString path = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("applications/") + id);
    if (path.isEmpty()) return item;
    const Gui::DesktopEntry e = Gui::readDesktopEntry(path);
    if (!e.valid) return item;
    item.kind = QStringLiteral("app");
    item.name = e.name.isEmpty() ? id : e.name;
    item.target = path;
    item.builtIn = true;
    return item;
}

static QIcon themeOrFile(const QString &name, const QString &fallback)
{
    if (name.startsWith(QLatin1Char('/'))) {
        const QIcon f(name);
        if (!f.isNull()) return f;
    } else if (!name.isEmpty() && QIcon::hasThemeIcon(name)) {
        return QIcon::fromTheme(name);
    }
    return QIcon::fromTheme(fallback);
}

QIcon launcherIcon(const Gui::LauncherItem &item)
{
    if (item.kind == QLatin1String("app")) {
        return themeOrFile(Gui::readDesktopEntry(item.target).icon, QStringLiteral("application-x-executable"));
    }
    if (item.kind == QLatin1String("folder")) return QIcon::fromTheme(QStringLiteral("folder"));
    if (item.kind == QLatin1String("url")) return QIcon::fromTheme(QStringLiteral("text-html"));
    return QIcon::fromTheme(QStringLiteral("utilities-terminal"));
}

QString launchItem(const Gui::LauncherItem &item)
{
    if (item.kind == QLatin1String("command")) {
        return QProcess::startDetached(QStringLiteral("/bin/sh"), { QStringLiteral("-c"), item.target })
                   ? QString() : QStringLiteral("コマンドを実行できません");
    }
    if (item.kind == QLatin1String("url")) {
        return QDesktopServices::openUrl(QUrl(item.target)) ? QString() : QStringLiteral("開けません");
    }
    if (!QFileInfo::exists(item.target)) return QStringLiteral("見つかりません（") + item.target + QStringLiteral("）");
    if (item.kind == QLatin1String("folder")) {
        return QDesktopServices::openUrl(QUrl::fromLocalFile(item.target)) ? QString() : QStringLiteral("開けません");
    }
    /* app: .desktop ファイル。gio があればそれに任せる。なければ Exec を自分で起動する */
    if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty()) {
        return QProcess::startDetached(QStringLiteral("gio"), { QStringLiteral("launch"), item.target })
                   ? QString() : QStringLiteral("起動できません");
    }
    const QStringList args = Gui::execArguments(Gui::readDesktopEntry(item.target).exec);
    if (args.isEmpty()) return QStringLiteral("起動するコマンドがありません");
    return QProcess::startDetached(args.first(), args.mid(1)) ? QString() : QStringLiteral("起動できません");
}

LauncherWindow::LauncherWindow(bool standalone, QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool), m_standalone(standalone)
{
    setWindowTitle(QStringLiteral("MAGITRA ランチャー"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(6 * 112 + 44);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("panel"));
    panel->setStyleSheet(QStringLiteral(
        "#panel { background: rgba(30, 31, 34, 242); border-radius: 16px; }"
        "QLabel { color: #b8bbc0; }"
        "QLineEdit { background: #2c2d31; color: white; border: none; border-radius: 4px; padding: 7px 10px; font-size: 16px; }"
        "QListWidget { background: transparent; color: white; border: none; outline: none; }"
        "QListWidget::item { border-radius: 10px; }"
        "QListWidget::item:hover { background: rgba(255, 255, 255, 36); }"
        "QListWidget::item:selected { background: #1f6feb; color: white; }"));
    outer->addWidget(panel);

    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(22, 20, 22, 20);
    m_search = new QLineEdit(panel);
    m_search->setPlaceholderText(QStringLiteral("名前で絞り込む（矢印キーで選んで Enter）"));
    m_search->installEventFilter(this);
    lay->addWidget(m_search);

    m_list = new QListWidget(panel);
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(48, 48));
    m_list->setGridSize(QSize(112, 104));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setWordWrap(true);
    m_list->setUniformItemSizes(true);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setMinimumHeight(110);
    m_list->setMaximumHeight(4 * 104 + 4);
    lay->addWidget(m_list);

    m_message = new QLabel(panel);
    m_message->setWordWrap(true);
    lay->addWidget(m_message);

    m_settings = new QPushButton(QStringLiteral("設定を開く"), panel);
    lay->addWidget(m_settings, 0, Qt::AlignLeft);

    connect(m_search, &QLineEdit::textChanged, this, [this] { filter(); });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { launchRow(m_list->row(it)); });
    connect(m_settings, &QPushButton::clicked, this, [this] {
        emit settingsRequested();
        hideLauncher();
    });
}

void LauncherWindow::showLauncher()
{
    bool showBrowser = true;
    const QList<Gui::LauncherItem> registered = Gui::loadLauncherItems(Gui::launcherConfPath(), &showBrowser);
    m_registeredCount = registered.size();
    m_items.clear();
    if (showBrowser) {
        const Gui::LauncherItem b = defaultBrowserItem();
        if (!b.target.isEmpty()) m_items << b;
    }
    m_items << registered;
    m_settings->setVisible(m_registeredCount == 0);

    m_showingError = false;
    m_search->clear();
    filter();

    adjustSize();
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect r = screen->availableGeometry();
        move(r.center() - QPoint(width() / 2, height() / 2));
    }
    show();
    raise();
    activateWindow();
    m_search->setFocus();
}

void LauncherWindow::toggle()
{
    if (isVisible()) hideLauncher();
    else showLauncher();
}

void LauncherWindow::hideLauncher()
{
    hide();
    if (m_standalone) QCoreApplication::quit();
}

void LauncherWindow::filter()
{
    const QString q = m_search->text().trimmed();
    m_list->clear();
    m_filtered.clear();
    for (int i = 0; i < m_items.size(); i++) {
        const Gui::LauncherItem &it = m_items[i];
        if (!q.isEmpty() && !it.name.contains(q, Qt::CaseInsensitive)) continue;
        auto *w = new QListWidgetItem(launcherIcon(it), it.name, m_list);
        w->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        w->setSizeHint(QSize(104, 100));
        w->setToolTip(it.builtIn ? QStringLiteral("既定のブラウザー（") + it.target + QStringLiteral("）") : it.target);
        m_filtered << i;
    }
    if (m_list->count() > 0) m_list->setCurrentRow(0);
    if (!m_showingError) {
        QString text;
        if (m_registeredCount == 0 && q.isEmpty()) {
            text = QStringLiteral("設定画面の「ランチャー」で、よく使うアプリ・フォルダー・URL を追加できます。");
        } else if (m_filtered.isEmpty()) {
            text = QStringLiteral("「") + q + QStringLiteral("」に合うものはありません。");
        }
        m_message->setText(text);
        m_message->setVisible(!text.isEmpty());
    }
}

void LauncherWindow::launchRow(int row)
{
    if (row < 0 || row >= m_filtered.size()) return;
    const Gui::LauncherItem &item = m_items[m_filtered[row]];
    const QString error = launchItem(item);
    if (error.isEmpty()) {
        hideLauncher();
        return;
    }
    m_showingError = true;
    m_message->setText(QStringLiteral("「") + item.name + QStringLiteral("」を起動できませんでした: ") + error);
    m_message->setVisible(true);
}

void LauncherWindow::launchCurrent()
{
    launchRow(m_list->currentRow());
}

bool LauncherWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_search && event->type() == QEvent::KeyPress) {
        switch (static_cast<QKeyEvent *>(event)->key()) {
        case Qt::Key_Escape:
            hideLauncher();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            launchCurrent();
            return true;
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
            QCoreApplication::sendEvent(m_list, event);
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, event);
}

bool LauncherWindow::event(QEvent *event)
{
    /* 別のところを押す・別のウィンドウへ移ると隠れる */
    if (event->type() == QEvent::WindowDeactivate && isVisible()) hideLauncher();
    return QWidget::event(event);
}

void LauncherWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) hideLauncher();
    else QWidget::keyPressEvent(event);
}

void LauncherWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hideLauncher();
}
