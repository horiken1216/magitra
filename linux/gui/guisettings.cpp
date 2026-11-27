#include "guisettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#ifndef MAGITRA_DEFAULT_PROFILE
#define MAGITRA_DEFAULT_PROFILE "generic"
#endif

namespace Gui {

static const QList<IntSpec> kIntSpecs = {
    { "NaturalScroll", 0, 1, 1, false },
    { "ForceTouch", 0, 1, 1, false },
    { "Gestures", 0, 1, 1, false },
    { "PointerSpeed", 1, 100, 50, false },
    { "PointerAccel", 0, 30, 15, false },
    { "ScrollSpeed", 1, 100, 50, false },
    { "ForceThreshold", 170, 250, 200, false },
    { "ForceAction", 0, kActionCount - 1, 6, true },
    { "HapticStrength", 0, 0x30, 0x30, false },
    { "HapticSharpness", 0, 0x20, 0x00, false },
    { "HapticDepth", 0, 0x40, 0x40, false },
    { "Swipe3Left", 0, kActionCount - 1, 1, true },
    { "Swipe3Right", 0, kActionCount - 1, 2, true },
    { "Swipe3Up", 0, kActionCount - 1, 3, true },
    { "Swipe3Down", 0, kActionCount - 1, 4, true },
    { "Swipe4Left", 0, kActionCount - 1, 5, true },
    { "Swipe4Right", 0, kActionCount - 1, 5, true },
    { "Swipe4Up", 0, kActionCount - 1, 12, true },
    { "Swipe4Down", 0, kActionCount - 1, 0, true },
    { "Inertia", 0, 1, 1, false },
    { "InertiaGlide", 1, 100, 50, false },
    { "Pinch", 0, 1, 1, false },
    { "TwoFingerDoubleTap", 0, kActionCount - 1, 11, true },
    { "DoubleTapInterval", 200, 800, 400, false },
};

const QList<IntSpec> &intSpecs()
{
    return kIntSpecs;
}

const IntSpec *findIntSpec(const QString &name)
{
    for (const IntSpec &s : kIntSpecs) {
        if (name == QLatin1String(s.name)) return &s;
    }
    return nullptr;
}

QString actionLabel(int id)
{
    static const char *const labels[kActionCount] = {
        "なし", "右隣の仮想デスクトップ", "左隣の仮想デスクトップ", "タスクビュー", "デスクトップの表示",
        "直前のアプリに切り替え", "中クリック", "戻る", "進む", "再生・一時停止", "検索",
        "拡大率を元に戻す（Ctrl+0）", "ランチャー",
    };
    return (id >= 0 && id < kActionCount) ? QString::fromUtf8(labels[id]) : QString();
}

static const QList<KeyAction> kKeyActions = {
    { "DesktopNext", 1 }, { "DesktopPrev", 2 }, { "TaskView", 3 }, { "ShowDesktop", 4 }, { "AppSwitch", 5 },
    { "BrowserBack", 7 }, { "BrowserForward", 8 }, { "PlayPause", 9 }, { "Search", 10 }, { "ZoomReset", 11 },
};

const QList<KeyAction> &keyActions()
{
    return kKeyActions;
}

/* 既定のキー表。kKeyActions と同じ順。プロファイルを足すときはここに 1 つ足す */
struct Profile {
    const char *name;
    const char *keys[10];
};

static const Profile kProfiles[] = {
    { "generic",
      { "ctrl+alt+right", "ctrl+alt+left", "super+w", "super+d", "alt+tab",
        "alt+left", "alt+right", "playpause", "super", "ctrl+0" } },
    /* LXQt。linux/daemon/settings.c の表と同じにしておく */
    { "lxqt",
      { "ctrl+alt+right", "ctrl+alt+left", "alt+f1", "super+d", "alt+tab",
        "alt+left", "alt+right", "playpause", "exec:lxqt-runner", "ctrl+0" } },
};

static const Profile *findProfile(const QString &name)
{
    for (const Profile &p : kProfiles) {
        if (name == QLatin1String(p.name)) return &p;
    }
    return nullptr;
}

QString fromQtKeySequence(const QString &seq)
{
    QStringList out;
    for (const QString &raw : seq.split(QLatin1Char('+'), Qt::SkipEmptyParts)) {
        const QString t = raw.trimmed().toLower();
        if (t == QLatin1String("meta") || t == QLatin1String("super_l") || t == QLatin1String("super_r")) out << QStringLiteral("super");
        else if (t == QLatin1String("control")) out << QStringLiteral("ctrl");
        else if (t == QLatin1String("esc")) out << QStringLiteral("escape");
        else out << t;
    }
    const QString s = out.join(QLatin1Char('+'));
    return (!s.isEmpty() && isValidBinding(s)) ? s : QString();
}

QString lxqtGlobalShortcut(const QStringList &paths)
{
    QStringList files;
    QString home = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (home.isEmpty()) home = QDir::homePath() + QStringLiteral("/.config");
    files << home + QStringLiteral("/lxqt/globalkeyshortcuts.conf");
    QString dirs = qEnvironmentVariable("XDG_CONFIG_DIRS");
    if (dirs.isEmpty()) dirs = QStringLiteral("/etc/xdg");
    for (const QString &d : dirs.split(QLatin1Char(':'), Qt::SkipEmptyParts)) files << d + QStringLiteral("/lxqt/globalkeyshortcuts.conf");

    for (const QString &name : files) {
        QFile f(name);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        /* 最初に見つかった 1 つだけを使う（lxqt-globalkeys と同じ）。節は [<キー（URL エンコード）>.<番号>] */
        QString key, path;
        bool enabled = true;
        auto take = [&]() -> QString {
            if (!key.isEmpty() && enabled && paths.contains(path)) return fromQtKeySequence(key);
            return QString();
        };
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
                const QString found = take();
                if (!found.isEmpty()) return found;
                QString sec = line.mid(1, line.size() - 2);
                const int dot = sec.lastIndexOf(QLatin1Char('.'));
                key = (dot > 0 && sec != QLatin1String("General")) ? QUrl::fromPercentEncoding(sec.left(dot).toUtf8()) : QString();
                path.clear();
                enabled = true;
            } else if (line.startsWith(QLatin1String("path="))) {
                path = line.mid(5).trimmed();
            } else if (line.startsWith(QLatin1String("Enabled="))) {
                enabled = line.mid(8).trimmed().compare(QLatin1String("false"), Qt::CaseInsensitive) != 0;
            }
        }
        return take();
    }
    return QString();
}

QString defaultBinding(const QString &profile, const QString &actionName)
{
    if (profile == QLatin1String("lxqt")) {
        /* LXQt はパネルのメニュー（標準のメインメニューか、Lubuntu などの fancymenu）とデスクトップの表示を lxqt-globalkeys に持つ */
        QString found;
        if (actionName == QLatin1String("TaskView")) {
            found = lxqtGlobalShortcut({ QStringLiteral("/panel/mainmenu/show_hide"), QStringLiteral("/panel/fancymenu/show_hide") });
        } else if (actionName == QLatin1String("ShowDesktop")) {
            found = lxqtGlobalShortcut({ QStringLiteral("/panel/showdesktop/show_hide") });
        }
        if (!found.isEmpty()) return found;
    }
    const Profile *p = findProfile(profile);
    if (!p) p = findProfile(QString::fromLatin1(MAGITRA_DEFAULT_PROFILE));
    if (!p) p = &kProfiles[0];
    for (int i = 0; i < kKeyActions.size(); i++) {
        if (actionName == QLatin1String(kKeyActions[i].name)) return QString::fromLatin1(p->keys[i]);
    }
    return QString();
}

static bool isModifierToken(const QString &t)
{
    return t == QLatin1String("ctrl") || t == QLatin1String("alt") || t == QLatin1String("shift") || t == QLatin1String("super");
}

static bool isKeyToken(const QString &t)
{
    static const QSet<QString> names = {
        "left", "right", "up", "down", "tab", "escape", "return", "space", "pageup", "pagedown",
        "home", "end", "playpause", "back", "forward",
    };
    if (names.contains(t)) return true;
    if (t.size() == 1) {
        const QChar c = t.at(0);
        return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9'));
    }
    if (t.size() >= 2 && t.at(0) == QLatin1Char('f')) {
        bool ok = false;
        const int n = t.mid(1).toInt(&ok);
        return ok && n >= 1 && n <= 12 && t.mid(1) == QString::number(n);
    }
    return false;
}

bool isValidBinding(const QString &text)
{
    const QString s = text.trimmed();
    if (s.toUtf8().size() > 3500) return false; // デーモンへ送る 1 行（4096 バイトまで）に収める
    if (s.startsWith(QLatin1String("exec:"))) return !s.mid(5).trimmed().isEmpty();
    const QStringList tokens = s.toLower().split(QLatin1Char('+'));
    int keys = 0;
    for (const QString &raw : tokens) {
        const QString t = raw.trimmed();
        if (isModifierToken(t)) continue;
        if (!isKeyToken(t)) return false;
        keys++;
    }
    return keys <= 1;
}

QString configDir()
{
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    return base + QStringLiteral("/magitra");
}

QString agentConfPath()
{
    return configDir() + QStringLiteral("/agent.conf");
}

QString launcherConfPath()
{
    return configDir() + QStringLiteral("/launcher.conf");
}

/* "Name=value" の 1 行を分ける。コメント・空行・= のない行は false */
static bool splitLine(const QString &line, QString *key, QString *value)
{
    const QString t = line.trimmed();
    if (t.isEmpty() || t.startsWith(QLatin1Char('#'))) return false;
    const int eq = t.indexOf(QLatin1Char('='));
    if (eq <= 0) return false;
    *key = t.left(eq).trimmed();
    *value = t.mid(eq + 1).trimmed();
    return true;
}

static bool isManagedKey(const QString &key)
{
    return findIntSpec(key) || key.startsWith(QLatin1String("Key."));
}

bool AgentConfig::load(const QString &path)
{
    m_values.clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    while (!f.atEnd()) {
        QString k, v;
        if (splitLine(QString::fromUtf8(f.readLine()), &k, &v)) m_values[k] = v;
    }
    return true;
}

bool AgentConfig::save(const QString &path) const
{
    QStringList lines;
    QFile in(path);
    if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!in.atEnd()) {
            QString line = QString::fromUtf8(in.readLine());
            while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
            lines << line;
        }
        in.close();
    }
    QSet<QString> written;
    QStringList out;
    for (const QString &line : lines) {
        QString k, v;
        if (!splitLine(line, &k, &v)) {
            out << line;
        } else if (m_values.contains(k)) {
            if (!written.contains(k)) out << k + QLatin1Char('=') + m_values.value(k);
            written.insert(k);
        } else if (!isManagedKey(k)) {
            out << line;
        } /* 設定から消した行は書かない */
    }
    for (auto it = m_values.constBegin(); it != m_values.constEnd(); ++it) {
        if (!written.contains(it.key())) out << it.key() + QLatin1Char('=') + it.value();
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(out.join(QLatin1Char('\n')).toUtf8());
    if (!out.isEmpty()) f.write("\n");
    return f.commit();
}

int AgentConfig::getInt(const QString &name) const
{
    const IntSpec *s = findIntSpec(name);
    if (!s) return 0;
    bool ok = false;
    const int v = m_values.value(name).toInt(&ok, 10);
    if (!ok) return s->def;
    if (v < s->min || v > s->max) return s->isAction ? 0 : qBound(s->min, v, s->max);
    return v;
}

void AgentConfig::setInt(const QString &name, int value)
{
    const IntSpec *s = findIntSpec(name);
    if (!s) return;
    m_values[name] = QString::number(qBound(s->min, value, s->max));
}

QString AgentConfig::profile() const
{
    const QString p = m_values.value(QStringLiteral("Profile")).trimmed().toLower();
    return findProfile(p) ? p : QString::fromLatin1(MAGITRA_DEFAULT_PROFILE);
}

QString AgentConfig::launcherCommand() const
{
    const QString v = m_values.value(QStringLiteral("LauncherCommand")).trimmed();
    return v.isEmpty() ? QStringLiteral("magitra-gui --launcher") : v;
}

QString AgentConfig::binding(const QString &actionName) const
{
    const QString v = m_values.value(QStringLiteral("Key.") + actionName).trimmed();
    return (!v.isEmpty() && isValidBinding(v)) ? v : defaultBinding(profile(), actionName);
}

void AgentConfig::setBinding(const QString &actionName, const QString &text)
{
    const QString key = QStringLiteral("Key.") + actionName;
    const QString v = text.trimmed();
    if (v.isEmpty() || v == defaultBinding(profile(), actionName)) {
        m_values.remove(key);
    } else {
        m_values[key] = v;
    }
}

void AgentConfig::resetAll()
{
    const QStringList keys = m_values.keys();
    for (const QString &k : keys) {
        if (isManagedKey(k)) m_values.remove(k);
    }
}

bool looksLikeUrl(const QString &target)
{
    const int colon = target.indexOf(QStringLiteral("://"));
    if (colon <= 1) return target.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive);
    for (int i = 0; i < colon; i++) {
        const QChar c = target.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('+') && c != QLatin1Char('-') && c != QLatin1Char('.')) return false;
    }
    return true;
}

/* 登録したときの既定の名前: アプリは .desktop の名前、フォルダーはフォルダー名、URL はホスト名、コマンドは実行ファイル名 */
QString defaultItemName(const QString &kind, const QString &target)
{
    if (kind == QLatin1String("url")) {
        QString host = QUrl(target).host();
        if (host.startsWith(QLatin1String("www."))) host = host.mid(4);
        return host.isEmpty() ? target : host;
    }
    if (kind == QLatin1String("app")) {
        const DesktopEntry e = readDesktopEntry(target);
        return e.name.isEmpty() ? QFileInfo(target).completeBaseName() : e.name;
    }
    if (kind == QLatin1String("command")) {
        const QStringList parts = QProcess::splitCommand(target);
        return parts.isEmpty() ? target : QFileInfo(parts.first()).fileName();
    }
    QString trimmed = target;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/'))) trimmed.chop(1);
    const QString name = QFileInfo(trimmed).fileName();
    return name.isEmpty() ? trimmed : name;
}

static QString cleanField(QString s)
{
    s.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
    return s.trimmed();
}

QList<LauncherItem> loadLauncherItems(const QString &path, bool *showBrowser)
{
    QList<LauncherItem> list;
    if (showBrowser) *showBrowser = true;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return list;
    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine());
        while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.startsWith(QLatin1String("LauncherBrowser="))) {
            if (showBrowser) *showBrowser = line.mid(16).trimmed() != QLatin1String("0");
        } else if (line.startsWith(QLatin1String("item="))) {
            const QStringList p = line.mid(5).split(QLatin1Char('\t'));
            if (p.size() < 3 || p[2].trimmed().isEmpty()) continue;
            const QString kind = p[0].trimmed();
            if (kind != QLatin1String("app") && kind != QLatin1String("folder") && kind != QLatin1String("url")
                && kind != QLatin1String("command")) continue;
            LauncherItem it;
            it.kind = kind;
            it.name = p[1];
            it.target = p[2];
            list << it;
        }
    }
    return list;
}

bool saveLauncherItems(const QString &path, const QList<LauncherItem> &items, bool showBrowser)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(QByteArray("LauncherBrowser=") + (showBrowser ? "1" : "0") + "\n");
    for (const LauncherItem &it : items) {
        if (it.builtIn) continue;
        f.write(("item=" + cleanField(it.kind) + "\t" + cleanField(it.name) + "\t" + cleanField(it.target) + "\n").toUtf8());
    }
    return f.commit();
}

DesktopEntry readDesktopEntry(const QString &path)
{
    DesktopEntry e;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return e;
    const QString lang = QLocale().name().section(QLatin1Char('_'), 0, 0);
    QString localized;
    bool inEntry = false;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        if (!inEntry) continue;
        QString k, v;
        if (!splitLine(line, &k, &v)) continue;
        if (k == QLatin1String("Name")) e.name = v;
        else if (k == QStringLiteral("Name[") + lang + QLatin1Char(']')) localized = v;
        else if (k == QLatin1String("Icon")) e.icon = v;
        else if (k == QLatin1String("Exec")) e.exec = v;
    }
    if (!localized.isEmpty()) e.name = localized;
    e.valid = !e.exec.isEmpty();
    return e;
}

QStringList execArguments(const QString &exec)
{
    QStringList out;
    const QStringList parts = QProcess::splitCommand(exec);
    for (QString a : parts) {
        if (a.size() == 2 && a.at(0) == QLatin1Char('%') && a.at(1) != QLatin1Char('%')) continue;
        a.replace(QStringLiteral("%%"), QStringLiteral("%"));
        out << a;
    }
    return out;
}

} // namespace Gui
