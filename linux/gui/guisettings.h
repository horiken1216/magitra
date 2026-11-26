/*
 * guisettings.h - agent.conf / launcher.conf の読み書き（QtCore だけ。単体テストできるように分けてある）
 *
 * 名前・範囲・既定値は agent/mtp_settings.h / settings/MtpSettings.cs と同じ。
 * デーモン側の既定値は linux/daemon/settings.c にある（変えるときは両方を直すこと）。
 */
#ifndef MAGITRA_GUISETTINGS_H
#define MAGITRA_GUISETTINGS_H

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace Gui {

struct IntSpec {
    const char *name;
    int min;
    int max;
    int def;
    bool isAction; /* 範囲外は「なし」(0) にする（agent/mtp_settings.c の ClampAction） */
};

const IntSpec *findIntSpec(const QString &name);
const QList<IntSpec> &intSpecs();

/* MTP_ACTION 0..12 の表示名（settings/MtpSettings.cs の ActionItem.All と同じ） */
constexpr int kActionCount = 13;
QString actionLabel(int id);

/* Key.<名前> で束縛できる操作 */
struct KeyAction {
    const char *name;
    int id; /* MTP_ACTION */
};
const QList<KeyAction> &keyActions();

/* 束縛の書き方が正しいか（key combo か exec:コマンド）。デーモンと同じトークン表 */
bool isValidBinding(const QString &text);

QString defaultBinding(const QString &profile, const QString &actionName);

QString configDir(); /* $XDG_CONFIG_HOME/magitra */
QString agentConfPath();
QString launcherConfPath();

class AgentConfig {
public:
    /* 読み直す。ファイルがなければ既定値のまま false */
    bool load(const QString &path);
    /* 変えた値を書く。知らない行・コメントはそのまま残す */
    bool save(const QString &path) const;

    int getInt(const QString &name) const;
    void setInt(const QString &name, int value);

    QString profile() const;
    QString binding(const QString &actionName) const; /* 実際に効く値（未設定なら既定） */
    void setBinding(const QString &actionName, const QString &text); /* 空か既定と同じなら設定を消す */

    void resetAll(); /* 整数の設定と Key.* を消す（知らないキーは残す） */

private:
    QMap<QString, QString> m_values;
};

struct LauncherItem {
    QString kind; /* app / folder / url / command */
    QString name;
    QString target;
    bool builtIn = false; /* 既定のブラウザー（保存しない） */
};

bool looksLikeUrl(const QString &target);
QString defaultItemName(const QString &kind, const QString &target);
QList<LauncherItem> loadLauncherItems(const QString &path, bool *showBrowser);
bool saveLauncherItems(const QString &path, const QList<LauncherItem> &items, bool showBrowser);

/* .desktop の Name / Icon / Exec（[Desktop Entry] だけ） */
struct DesktopEntry {
    QString name;
    QString icon;
    QString exec;
    bool valid = false;
};
DesktopEntry readDesktopEntry(const QString &path);

/* Exec 行を引数に分け、%f %u などのフィールドコードを取り除く */
QStringList execArguments(const QString &exec);

} // namespace Gui

#endif
