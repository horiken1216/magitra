#include "settingswindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <cmath>

#include "launcherwindow.h"

static const char *const kFooterText = "設定は変えた瞬間に保存され、常駐アプリにすぐ反映されます。";

static QLabel *textLabel(const QString &text, bool dim = false)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    if (dim) l->setStyleSheet(QStringLiteral("color: gray;"));
    return l;
}

static QString multiplier(int v)
{
    return QStringLiteral("%1 倍").arg(std::pow(2.0, (v - 50) / 25.0), 0, 'f', 2);
}

static QString plain(int v)
{
    return QString::number(v);
}

SettingsWindow::SettingsWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(QStringLiteral("MAGITRA 設定"));
    setWindowIcon(QIcon(QStringLiteral(":/magitra.png")));
    resize(900, 640);
    setMinimumSize(720, 480);

    m_cfg.load(Gui::agentConfPath());
    m_items = Gui::loadLauncherItems(Gui::launcherConfPath(), &m_showBrowser);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(150);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { saveNow(); });

    auto *root = new QHBoxLayout(this);
    auto *side = new QVBoxLayout;
    auto *title = new QLabel(QStringLiteral("MAGITRA"));
    QFont f = title->font();
    f.setPointSize(f.pointSize() + 6);
    f.setBold(true);
    title->setFont(f);
    side->addWidget(title);
    side->addWidget(textLabel(QStringLiteral("Magic Trackpad の設定"), true));
    m_nav = new QListWidget;
    m_nav->setFixedWidth(180);
    side->addWidget(m_nav, 1);
    root->addLayout(side);

    auto *right = new QVBoxLayout;
    m_pages = new QStackedWidget;
    right->addWidget(m_pages, 1);
    m_footer = textLabel(QString::fromUtf8(kFooterText), true);
    right->addWidget(m_footer);
    root->addLayout(right, 1);

    pointerPage();
    clickPage();
    hapticPage();
    gesturePage();
    keysPage();
    launcherPage();
    generalPage();

    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    m_statusTimer.setInterval(2000);
    connect(&m_statusTimer, &QTimer::timeout, this, [this] { updateAgentStatus(); });
    m_statusTimer.start();
    updateAgentStatus();
}

// ---- 部品 ----

QVBoxLayout *SettingsWindow::newPage(const QString &nav, const QString &title, const QString &description)
{
    m_nav->addItem(nav);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *lay = new QVBoxLayout(body);
    auto *t = new QLabel(title);
    QFont f = t->font();
    f.setPointSize(f.pointSize() + 8);
    f.setBold(true);
    t->setFont(f);
    lay->addWidget(t);
    if (!description.isEmpty()) lay->addWidget(textLabel(description, true));
    scroll->setWidget(body);
    m_pages->addWidget(scroll);
    return lay;
}

QWidget *SettingsWindow::card(const QString &title, const QString &description, QLayout *body)
{
    auto *box = new QGroupBox(title);
    auto *lay = new QVBoxLayout(box);
    if (!description.isEmpty()) lay->addWidget(textLabel(description, true));
    lay->addLayout(body);
    return box;
}

// 左右の言葉つきスライダー。動かすたびに保存する。右端の「既定」ボタンで、このスライダーだけを既定値に戻す
QWidget *SettingsWindow::sliderRow(const QString &name, const QString &left, const QString &right, std::function<QString(int)> text)
{
    const Gui::IntSpec *spec = Gui::findIntSpec(name);
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(spec->min, spec->max);
    slider->setValue(m_cfg.getInt(name));
    auto *value = new QLabel;
    value->setMinimumWidth(70);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *reset = new QPushButton(QStringLiteral("既定"));
    auto refresh = [=] {
        const int v = slider->value();
        value->setText(text(v));
        reset->setEnabled(v != spec->def);
        reset->setToolTip(QStringLiteral("既定値（") + text(spec->def) + QStringLiteral("）に戻す"));
    };
    refresh();
    connect(slider, &QSlider::valueChanged, this, [=](int v) {
        refresh();
        m_cfg.setInt(name, v);
        scheduleSave();
    });
    connect(reset, &QPushButton::clicked, slider, [=] { slider->setValue(spec->def); });
    m_resetters[name] = [=] { slider->setValue(spec->def); };
    h->addWidget(new QLabel(left));
    h->addWidget(slider, 1);
    h->addWidget(new QLabel(right));
    h->addWidget(value);
    h->addWidget(reset);
    return w;
}

// スイッチ（オン・オフ）。切り替えるたびに保存する
QWidget *SettingsWindow::toggle(const QString &label, const QString &name)
{
    const Gui::IntSpec *spec = Gui::findIntSpec(name);
    auto *c = new QCheckBox(label);
    c->setChecked(m_cfg.getInt(name) != 0);
    connect(c, &QCheckBox::toggled, this, [=](bool on) {
        m_cfg.setInt(name, on ? 1 : 0);
        scheduleSave();
    });
    m_resetters[name] = [=] { c->setChecked(spec->def != 0); };
    return c;
}

QWidget *SettingsWindow::actionPicker(const QString &name)
{
    const Gui::IntSpec *spec = Gui::findIntSpec(name);
    auto *box = new QComboBox;
    for (int id = 0; id < Gui::kActionCount; id++) box->addItem(Gui::actionLabel(id), id);
    box->setCurrentIndex(m_cfg.getInt(name));
    connect(box, &QComboBox::currentIndexChanged, this, [=](int index) {
        m_cfg.setInt(name, box->itemData(index).toInt());
        scheduleSave();
    });
    m_resetters[name] = [=] { box->setCurrentIndex(spec->def); };
    return box;
}

// ---- 各ページ ----

void SettingsWindow::pointerPage()
{
    QVBoxLayout *p = newPage(QStringLiteral("ポインター"), QStringLiteral("ポインターとスクロール"),
                             QStringLiteral("指 1 本でのポインター移動と、2 本指でのスクロール・拡大縮小の感じを調整します。"));

    auto *pointer = new QVBoxLayout;
    pointer->addWidget(sliderRow(QStringLiteral("PointerSpeed"), QStringLiteral("遅い"), QStringLiteral("速い"), multiplier));
    pointer->addWidget(new QLabel(QStringLiteral("加速（速く動かすほど遠くへ動く度合い）")));
    pointer->addWidget(sliderRow(QStringLiteral("PointerAccel"), QStringLiteral("なし"), QStringLiteral("強い"),
                                 [](int v) { return QString::number(v / 10.0, 'f', 1); }));
    p->addWidget(card(QStringLiteral("ポインターの速さ"),
                      QStringLiteral("中央が既定の速さです。25 目盛り動かすごとに 2 倍（半分）になります。"), pointer));

    auto *scroll = new QVBoxLayout;
    scroll->addWidget(sliderRow(QStringLiteral("ScrollSpeed"), QStringLiteral("遅い"), QStringLiteral("速い"), multiplier));
    scroll->addWidget(toggle(QStringLiteral("ナチュラルスクロール（Mac と同じ向き。内容が指と同じ向きに動く）"), QStringLiteral("NaturalScroll")));
    p->addWidget(card(QStringLiteral("スクロール"), QString(), scroll));

    auto *inertia = new QVBoxLayout;
    inertia->addWidget(toggle(QStringLiteral("慣性スクロール"), QStringLiteral("Inertia")));
    inertia->addWidget(new QLabel(QStringLiteral("滑る長さ")));
    inertia->addWidget(sliderRow(QStringLiteral("InertiaGlide"), QStringLiteral("短い"), QStringLiteral("長い"),
                                 [](int v) { return QStringLiteral("%1 秒").arg(0.4 * std::pow(2.0, (v - 50) / 50.0), 0, 'f', 2); }));
    p->addWidget(card(QStringLiteral("慣性スクロール"),
                      QStringLiteral("2 本指で払って離すと、その勢いでしばらくスクロールが続きます。どこかに触れると止まります。"), inertia));

    auto *pinch = new QVBoxLayout;
    pinch->addWidget(toggle(QStringLiteral("ピンチで拡大・縮小する"), QStringLiteral("Pinch")));
    pinch->addWidget(new QLabel(QStringLiteral("2 本指でダブルタップ（押し込まずに軽く 2 回たたく）")));
    pinch->addWidget(actionPicker(QStringLiteral("TwoFingerDoubleTap")));
    pinch->addWidget(new QLabel(QStringLiteral("ダブルタップの間隔")));
    pinch->addWidget(sliderRow(QStringLiteral("DoubleTapInterval"), QStringLiteral("速くたたく"), QStringLiteral("ゆっくりでもよい"),
                               [](int v) { return QStringLiteral("%1 秒").arg(v / 1000.0, 0, 'f', 2); }));
    pinch->addWidget(textLabel(QStringLiteral("「拡大率を元に戻す」は Ctrl+0 を送ります。ブラウザーやファイルマネージャーでは元の大きさに戻りますが、"
                                              "アプリによっては別の操作になります。"), true));
    p->addWidget(card(QStringLiteral("ピンチ"),
                      QStringLiteral("2 本指を広げると拡大、つまむと縮小します（Ctrl + ホイールとして送ります。ブラウザーなどで使えます）。"), pinch));
    p->addStretch(1);
}

void SettingsWindow::clickPage()
{
    QVBoxLayout *p = newPage(QStringLiteral("クリック"), QStringLiteral("クリックと Force Touch"),
                             QStringLiteral("1 本指で押し込むと左クリック、2 本指で押し込むと右クリックです。"));

    auto *force = new QVBoxLayout;
    force->addWidget(toggle(QStringLiteral("Force Touch を使う"), QStringLiteral("ForceTouch")));
    force->addWidget(new QLabel(QStringLiteral("発動に必要な押し込みの強さ")));
    force->addWidget(sliderRow(QStringLiteral("ForceThreshold"), QStringLiteral("軽く"), QStringLiteral("強く"), plain));
    auto *assign = new QHBoxLayout;
    assign->addWidget(new QLabel(QStringLiteral("強く押し込んだときの操作")), 1);
    assign->addWidget(actionPicker(QStringLiteral("ForceAction")));
    force->addLayout(assign);
    p->addWidget(card(QStringLiteral("Force Touch"),
                      QStringLiteral("1 本指で押した直後に強く押し込むと、振動を返して割り当てた操作をします。"
                                     "見極めのため、普通のクリックの確定が最大 0.15 秒遅れます。"
                                     "押したまま指を動かすとドラッグになり、Force Touch にはなりません。"), force));
    p->addStretch(1);
}

void SettingsWindow::hapticPage()
{
    QVBoxLayout *p = newPage(QStringLiteral("振動"), QStringLiteral("振動"),
                             QStringLiteral("Force Touch が成立したときに返す振動の手応えです。上限は、公開情報で安全とされる範囲に抑えてあります。"));

    auto *feel = new QVBoxLayout;
    feel->addWidget(new QLabel(QStringLiteral("強さ")));
    feel->addWidget(sliderRow(QStringLiteral("HapticStrength"), QStringLiteral("弱い"), QStringLiteral("強い"), plain));
    feel->addWidget(new QLabel(QStringLiteral("鋭さ（振動の質）")));
    feel->addWidget(sliderRow(QStringLiteral("HapticSharpness"), QStringLiteral("柔らかい"), QStringLiteral("鋭い"), plain));
    feel->addWidget(new QLabel(QStringLiteral("重さ")));
    feel->addWidget(sliderRow(QStringLiteral("HapticDepth"), QStringLiteral("軽い"), QStringLiteral("重い"), plain));

    auto *buttons = new QHBoxLayout;
    auto *test = new QPushButton(QStringLiteral("試す"));
    auto *reset = new QPushButton(QStringLiteral("既定に戻す"));
    auto *result = new QLabel;
    buttons->addWidget(test);
    buttons->addWidget(reset);
    buttons->addWidget(result, 1);
    feel->addLayout(buttons);
    connect(test, &QPushButton::clicked, this, [this, result] {
        const bool ok = m_client.haptic(m_cfg.getInt(QStringLiteral("HapticStrength")), m_cfg.getInt(QStringLiteral("HapticSharpness")),
                                        m_cfg.getInt(QStringLiteral("HapticDepth")));
        result->setText(ok ? QStringLiteral("振動を送りました。")
                           : (m_client.status().running ? QStringLiteral("鳴らせませんでした（接続されているか、少し間をあけて試してください）。")
                                                        : QStringLiteral("常駐アプリが動いていません。")));
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        for (const char *n : { "HapticStrength", "HapticSharpness", "HapticDepth" }) m_resetters[QString::fromLatin1(n)]();
    });
    p->addWidget(card(QStringLiteral("Force Touch の振動"), QStringLiteral("「試す」でトラックパッドが振動します。指を軽く置いて確かめてください。"), feel));
    p->addStretch(1);
}

void SettingsWindow::gesturePage()
{
    QVBoxLayout *p = newPage(QStringLiteral("ジェスチャー"), QStringLiteral("ジェスチャー"),
                             QStringLiteral("3 本・4 本の指で払う操作に、好きな操作を割り当てます。約 1cm 払うと 1 回発動します。"));

    auto *onoff = new QVBoxLayout;
    onoff->addWidget(toggle(QStringLiteral("3・4本指ジェスチャーを使う"), QStringLiteral("Gestures")));
    p->addWidget(card(QStringLiteral("ジェスチャー"), QString(), onoff));

    const QStringList dirs = { QStringLiteral("Left"), QStringLiteral("Right"), QStringLiteral("Up"), QStringLiteral("Down") };
    const QStringList dirNames = { QStringLiteral("← 左へ払う"), QStringLiteral("→ 右へ払う"), QStringLiteral("↑ 上（奥）へ払う"),
                                   QStringLiteral("↓ 下（手前）へ払う") };
    for (int fingers = 3; fingers <= 4; fingers++) {
        auto *g = new QGridLayout;
        g->setColumnStretch(0, 1);
        for (int d = 0; d < 4; d++) {
            g->addWidget(new QLabel(dirNames[d]), d, 0);
            g->addWidget(actionPicker(QStringLiteral("Swipe%1%2").arg(fingers).arg(dirs[d])), d, 1);
        }
        p->addWidget(card(QStringLiteral("%1 本指").arg(fingers),
                          fingers == 3 ? QStringLiteral("既定: 左右で仮想デスクトップ、上でタスクビュー、下でデスクトップの表示（Mac に近い割り当て）")
                                       : QStringLiteral("既定: 左右で直前のアプリに切り替え、上でランチャー。下は空けてあります。"),
                          g));
    }
    p->addStretch(1);
}

// 操作ごとに送るキーを変える（Linux だけ）。空にすると既定（プロファイルの表）に戻る
void SettingsWindow::keysPage()
{
    QVBoxLayout *p = newPage(QStringLiteral("操作のキー"), QStringLiteral("操作のキー"),
                             QStringLiteral("操作を実行するときに送るキーを変えます。空にすると既定に戻ります。"
                                            "書き方は ctrl+alt+right のように + でつなぐか、exec:コマンド で外部のコマンドを実行します。"));
    auto *g = new QGridLayout;
    g->setColumnStretch(1, 1);
    int row = 0;
    for (const Gui::KeyAction &a : Gui::keyActions()) {
        const QString name = QString::fromLatin1(a.name);
        const QString def = Gui::defaultBinding(m_cfg.profile(), name);
        auto *edit = new QLineEdit(m_cfg.binding(name));
        edit->setPlaceholderText(def);
        connect(edit, &QLineEdit::textChanged, this, [=](const QString &text) {
            const bool valid = text.trimmed().isEmpty() || Gui::isValidBinding(text);
            edit->setStyleSheet(valid ? QString() : QStringLiteral("color: #c5221f;"));
            edit->setToolTip(valid ? QString() : QStringLiteral("書き方が正しくありません（例: ctrl+alt+right、exec:notify-send hi）"));
            if (!valid) return; // 正しくなるまで保存しない
            m_cfg.setBinding(name, text);
            scheduleSave();
        });
        m_resetters[QStringLiteral("Key.") + name] = [=] { edit->setText(def); };
        g->addWidget(new QLabel(Gui::actionLabel(a.id)), row, 0);
        g->addWidget(edit, row, 1);
        row++;
    }
    p->addWidget(card(QStringLiteral("キーの割り当て"),
                      QStringLiteral("使えるキー: ctrl alt shift super、a-z、0-9、f1-f12、left right up down、tab escape return space、"
                                     "pageup pagedown home end、playpause back forward"), g));
    p->addStretch(1);
}

// ---- ランチャー ----

void SettingsWindow::launcherPage()
{
    QVBoxLayout *p = newPage(QStringLiteral("ランチャー"), QStringLiteral("ランチャー"),
                             QStringLiteral("4 本指で上（奥）へ払うと、画面の中央に一覧が開きます。クリックするか、文字を打って絞り込み、"
                                            "矢印キーと Enter で起動します。払う向きはジェスチャーのページで変えられます。"));

    auto *browser = new QVBoxLayout;
    const Gui::LauncherItem found = defaultBrowserItem();
    auto *check = new QCheckBox(!found.target.isEmpty() ? QStringLiteral("既定のブラウザー（") + found.name + QStringLiteral("）を先頭に出す")
                                                        : QStringLiteral("既定のブラウザーを先頭に出す"));
    check->setChecked(m_showBrowser);
    connect(check, &QCheckBox::toggled, this, [this](bool on) {
        m_showBrowser = on;
        saveLauncher();
    });
    m_resetters[QStringLiteral("LauncherBrowser")] = [check] { check->setChecked(true); };
    browser->addWidget(check);
    browser->addWidget(textLabel(!found.target.isEmpty()
                                     ? QStringLiteral("登録したものとは別に、いちばん前に出します。デスクトップの既定のブラウザーを変えると、ここも変わります。")
                                     : QStringLiteral("既定のブラウザーが見つかりません（デスクトップの設定の「既定のアプリ」で選べます）。"), true));
    auto *tryIt = new QPushButton(QStringLiteral("ランチャーを開いてみる"));
    connect(tryIt, &QPushButton::clicked, this, [this] { emit launcherRequested(); });
    browser->addWidget(tryIt, 0, Qt::AlignLeft);
    p->addWidget(card(QStringLiteral("既定のブラウザー"), QString(), browser));

    auto *items = new QVBoxLayout;
    items->addWidget(launcherEditor());
    p->addWidget(card(QStringLiteral("並べるもの"), QStringLiteral("既定のブラウザーの後に、上から順に並びます。名前と対象は下の欄で変えられます。"), items));
    p->addStretch(1);
}

void SettingsWindow::saveLauncher()
{
    if (!Gui::saveLauncherItems(Gui::launcherConfPath(), m_items, m_showBrowser)) {
        m_footer->setText(QStringLiteral("ランチャーの設定を保存できませんでした。"));
    }
}

void SettingsWindow::refreshLauncherList(int select)
{
    m_loading = true;
    m_launchList->clear();
    for (const Gui::LauncherItem &it : m_items) {
        auto *w = new QListWidgetItem(launcherIcon(it), (it.name.isEmpty() ? QStringLiteral("（名前なし）") : it.name) + QLatin1Char('\n') + it.target);
        m_launchList->addItem(w);
    }
    m_launchList->setCurrentRow(qMin(select, m_items.size() - 1));
    m_loading = false;
    showLauncherDetail();
}

void SettingsWindow::showLauncherDetail()
{
    const int i = m_launchList->currentRow();
    const bool has = i >= 0 && i < m_items.size();
    m_nameEdit->setText(has ? m_items[i].name : QString());
    m_targetEdit->setText(has ? m_items[i].target : QString());
    m_nameEdit->setEnabled(has);
    m_targetEdit->setEnabled(has);
}

void SettingsWindow::addLauncherItem(const QString &kind, const QString &target)
{
    Gui::LauncherItem it;
    it.kind = kind;
    it.target = target;
    it.name = Gui::defaultItemName(kind, target);
    m_items << it;
    saveLauncher();
    refreshLauncherList(m_items.size() - 1);
}

QWidget *SettingsWindow::launcherEditor()
{
    auto *w = new QWidget;
    auto *editor = new QVBoxLayout(w);
    editor->setContentsMargins(0, 0, 0, 0);

    m_launchList = new QListWidget;
    m_launchList->setIconSize(QSize(32, 32));
    m_launchList->setMinimumHeight(200);
    editor->addWidget(m_launchList);

    auto *add = new QHBoxLayout;
    auto *order = new QHBoxLayout;
    auto button = [](QHBoxLayout *row, const QString &text) {
        auto *b = new QPushButton(text);
        row->addWidget(b);
        return b;
    };
    QPushButton *addApp = button(add, QStringLiteral("アプリを追加…"));
    QPushButton *addFolder = button(add, QStringLiteral("フォルダーを追加…"));
    QPushButton *addUrl = button(add, QStringLiteral("URL を追加…"));
    QPushButton *addCommand = button(add, QStringLiteral("コマンドを追加…"));
    add->addStretch(1);
    QPushButton *up = button(order, QStringLiteral("↑ 上へ"));
    QPushButton *down = button(order, QStringLiteral("↓ 下へ"));
    QPushButton *del = button(order, QStringLiteral("削除"));
    order->addStretch(1);
    editor->addLayout(add);
    editor->addLayout(order);

    auto *detail = new QGridLayout;
    m_nameEdit = new QLineEdit;
    m_targetEdit = new QLineEdit;
    detail->addWidget(new QLabel(QStringLiteral("名前")), 0, 0);
    detail->addWidget(m_nameEdit, 0, 1);
    detail->addWidget(new QLabel(QStringLiteral("対象")), 1, 0);
    detail->addWidget(m_targetEdit, 1, 1);
    detail->setColumnStretch(1, 1);
    editor->addLayout(detail);

    auto updateRow = [this](int i) {
        QListWidgetItem *w = m_launchList->item(i);
        if (!w) return;
        w->setText((m_items[i].name.isEmpty() ? QStringLiteral("（名前なし）") : m_items[i].name) + QLatin1Char('\n') + m_items[i].target);
        w->setIcon(launcherIcon(m_items[i]));
    };
    connect(m_launchList, &QListWidget::currentRowChanged, this, [this] {
        if (!m_loading) showLauncherDetail();
    });
    connect(m_nameEdit, &QLineEdit::textEdited, this, [this, updateRow](const QString &t) {
        const int i = m_launchList->currentRow();
        if (i < 0 || i >= m_items.size()) return;
        m_items[i].name = t;
        updateRow(i);
        saveLauncher();
    });
    connect(m_targetEdit, &QLineEdit::textEdited, this, [this, updateRow](const QString &t) {
        const int i = m_launchList->currentRow();
        const QString target = t.trimmed();
        if (i < 0 || i >= m_items.size() || target.isEmpty()) return; // 空のままでは保存しない
        m_items[i].target = target;
        updateRow(i);
        saveLauncher();
    });

    connect(addApp, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("ランチャーに追加するアプリ"),
                                                                QStringLiteral("/usr/share/applications"),
                                                                QStringLiteral("アプリ (*.desktop);;すべてのファイル (*)"));
        for (const QString &f : files) addLauncherItem(QStringLiteral("app"), f);
    });
    connect(addFolder, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("ランチャーに追加するフォルダー"));
        if (!d.isEmpty()) addLauncherItem(QStringLiteral("folder"), d);
    });
    connect(addUrl, &QPushButton::clicked, this, [this] {
        QString u = QInputDialog::getText(this, QStringLiteral("URL を追加"), QStringLiteral("URL")).trimmed();
        if (u.isEmpty()) return;
        if (!Gui::looksLikeUrl(u) && u.contains(QLatin1Char('.')) && !u.contains(QLatin1Char(' '))) u = QStringLiteral("https://") + u;
        if (!Gui::looksLikeUrl(u)) {
            QMessageBox::warning(this, QStringLiteral("MAGITRA 設定"), QStringLiteral("https:// で始まる URL を入れてください"));
            return;
        }
        addLauncherItem(QStringLiteral("url"), u);
    });
    connect(addCommand, &QPushButton::clicked, this, [this] {
        const QString c = QInputDialog::getText(this, QStringLiteral("コマンドを追加"), QStringLiteral("実行するコマンド")).trimmed();
        if (!c.isEmpty()) addLauncherItem(QStringLiteral("command"), c);
    });
    connect(up, &QPushButton::clicked, this, [this] {
        const int i = m_launchList->currentRow();
        if (i <= 0 || i >= m_items.size()) return;
        m_items.swapItemsAt(i, i - 1);
        saveLauncher();
        refreshLauncherList(i - 1);
    });
    connect(down, &QPushButton::clicked, this, [this] {
        const int i = m_launchList->currentRow();
        if (i < 0 || i >= m_items.size() - 1) return;
        m_items.swapItemsAt(i, i + 1);
        saveLauncher();
        refreshLauncherList(i + 1);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const int i = m_launchList->currentRow();
        if (i < 0 || i >= m_items.size()) return;
        m_items.removeAt(i);
        saveLauncher();
        refreshLauncherList(i);
    });

    refreshLauncherList(0);
    return w;
}

// ---- 全般 ----

void SettingsWindow::generalPage()
{
    QVBoxLayout *p = newPage(QStringLiteral("全般"), QStringLiteral("全般"), QString());

    auto *agent = new QVBoxLayout;
    m_agentStatus = new QLabel;
    QFont f = m_agentStatus->font();
    f.setBold(true);
    m_agentStatus->setFont(f);
    agent->addWidget(m_agentStatus);
    p->addWidget(card(QStringLiteral("常駐アプリ"), QStringLiteral("トラックパッドの入力を受け取って動かすプログラム（magitrad）です。"), agent));

    auto *reset = new QVBoxLayout;
    auto *resetAllButton = new QPushButton(QStringLiteral("すべての設定を既定に戻す"));
    reset->addWidget(resetAllButton, 0, Qt::AlignLeft);
    connect(resetAllButton, &QPushButton::clicked, this, [this] { resetAll(); });
    p->addWidget(card(QStringLiteral("リセット"), QString(), reset));
    p->addStretch(1);
}

void SettingsWindow::updateAgentStatus()
{
    m_agentStatus->setText(m_client.status().running ? QStringLiteral("● 動作中") : QStringLiteral("○ 停止中"));
}

// ---- 保存 ----

void SettingsWindow::scheduleSave()
{
    m_saveTimer.start();
}

void SettingsWindow::saveNow()
{
    if (m_cfg.save(Gui::agentConfPath())) {
        m_footer->setText(QString::fromUtf8(kFooterText));
        m_client.sendConfig();
    } else {
        m_footer->setText(QStringLiteral("設定を保存できませんでした。"));
    }
}

void SettingsWindow::resetAll()
{
    if (QMessageBox::question(this, QStringLiteral("MAGITRA 設定"),
                              QStringLiteral("ポインター・クリック・振動・ジェスチャーの設定をすべて既定に戻します。よろしいですか？"),
                              QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok) {
        return;
    }
    for (auto it = m_resetters.constBegin(); it != m_resetters.constEnd(); ++it) it.value()();
    m_cfg.resetAll(); // 画面を戻したときに書き込まれた既定値も消す
    scheduleSave();
    m_footer->setText(QStringLiteral("すべての設定を既定に戻しました。"));
}
