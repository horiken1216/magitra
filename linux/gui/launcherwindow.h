/*
 * launcherwindow.h - ランチャー（launcher/MtpLauncher.cs と同じ動き）と、項目の起動・アイコン
 */
#ifndef MAGITRA_LAUNCHERWINDOW_H
#define MAGITRA_LAUNCHERWINDOW_H

#include <QIcon>
#include <QList>
#include <QWidget>

#include "guisettings.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

/* https を開くアプリ（既定のブラウザー）。見つからなければ target が空 */
Gui::LauncherItem defaultBrowserItem();
QIcon launcherIcon(const Gui::LauncherItem &item);
/* 起動する。失敗したら理由を返す（成功なら空） */
QString launchItem(const Gui::LauncherItem &item);

class LauncherWindow : public QWidget {
    Q_OBJECT
public:
    /* standalone: トレイ版が動いていないときの単独起動。隠れたら終了する */
    explicit LauncherWindow(bool standalone, QWidget *parent = nullptr);

    void showLauncher();
    void toggle();

signals:
    void settingsRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void hideLauncher();
    void filter();
    void launchCurrent();
    void launchRow(int row);

    bool m_standalone;
    int m_registeredCount = 0;
    bool m_showingError = false;
    QList<Gui::LauncherItem> m_items;
    QList<int> m_filtered; /* 表示中の行 → m_items の番号 */
    QLineEdit *m_search;
    QListWidget *m_list;
    QLabel *m_message;
    QPushButton *m_settings;
};

#endif
