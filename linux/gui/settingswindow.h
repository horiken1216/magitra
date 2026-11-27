/*
 * settingswindow.h - 設定画面（settings/MtpSettings.cs と同じ分類・ラベル・範囲）
 *
 * 変えた瞬間に agent.conf へ保存し（150ms まとめて）、常駐サービスへ CONFIG を送る。
 */
#ifndef MAGITRA_SETTINGSWINDOW_H
#define MAGITRA_SETTINGSWINDOW_H

#include <QHash>
#include <QTimer>
#include <QWidget>
#include <functional>

#include "agentclient.h"
#include "guisettings.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QVBoxLayout;

class SettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget *parent = nullptr);

signals:
    void launcherRequested();

private:
    QVBoxLayout *newPage(const QString &nav, const QString &title, const QString &description);
    QWidget *card(const QString &title, const QString &description, QLayout *body);
    QWidget *sliderRow(const QString &name, const QString &left, const QString &right, std::function<QString(int)> text);
    QWidget *toggle(const QString &label, const QString &name);
    QWidget *actionPicker(const QString &name);

    void pointerPage();
    void clickPage();
    void hapticPage();
    void gesturePage();
    void keysPage();
    void launcherPage();
    void generalPage();
    QWidget *launcherEditor();
    void refreshLauncherList(int select);
    void showLauncherDetail();
    void saveLauncher();
    void addLauncherItem(const QString &kind, const QString &target);

    void scheduleSave();
    void saveNow();
    void resetAll();
    void updateAgentStatus();

    Gui::AgentConfig m_cfg;
    AgentClient m_client;
    QTimer m_saveTimer;
    QTimer m_statusTimer;
    QListWidget *m_nav;
    QStackedWidget *m_pages;
    QLabel *m_footer;
    QLabel *m_agentStatus = nullptr;
    QHash<QString, std::function<void()>> m_resetters;
    QList<Gui::LauncherItem> m_items;
    bool m_showBrowser = true;
    bool m_loading = false;
    QListWidget *m_launchList = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_targetEdit = nullptr;
};

#endif
