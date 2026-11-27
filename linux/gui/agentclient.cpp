#include "agentclient.h"

#include "guisettings.h"

#include <QByteArray>
#include <QFile>
#include <QLocalSocket>
#include <QStringList>

QString AgentClient::socketPath()
{
    return QStringLiteral("/run/magitra/agent.sock");
}

/* agent.conf を CONFIG ブロックにする。本文に END の行があると途中で終わるので除く */
static QByteArray configBlock()
{
    QByteArray body;
    QFile f(Gui::agentConfPath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            const QByteArray line = f.readLine();
            if (line.trimmed() != "END") body += line.endsWith('\n') ? line : line + '\n';
        }
    }
    /* デーモンはホームの設定を読めないので、GUI が見つけた既定（lxqt-globalkeys のキー）を先に送る。本文に同じキーがあればそちらが勝つ */
    QByteArray found;
    Gui::AgentConfig cfg;
    cfg.load(Gui::agentConfPath());
    for (const char *name : { "TaskView", "ShowDesktop" }) {
        const QString key = QString::fromLatin1(name);
        const QString def = Gui::defaultBinding(cfg.profile(), key);
        if (def != cfg.binding(key) || def.isEmpty()) continue; /* 自分で割り当てている */
        found += "Key." + key.toLatin1() + "=" + def.toUtf8() + "\n";
    }
    return QByteArray("CONFIG\n") + found + body + "END\n";
}

QString AgentClient::request(const QString &line, int timeoutMs) const
{
    QLocalSocket s;
    s.connectToServer(socketPath());
    if (!s.waitForConnected(timeoutMs)) return QString();
    s.write(line.toUtf8() + '\n');
    if (!s.waitForBytesWritten(timeoutMs)) return QString();
    while (!s.canReadLine()) {
        if (!s.waitForReadyRead(timeoutMs)) return QString();
    }
    return QString::fromUtf8(s.readLine()).trimmed();
}

bool AgentClient::ok(const QString &line) const
{
    return request(line).startsWith(QLatin1String("OK"));
}

AgentClient::Status AgentClient::status() const
{
    Status st;
    const QString r = request(QStringLiteral("STATUS"));
    if (!r.startsWith(QLatin1String("OK"))) return st;
    st.running = true;
    const QStringList parts = r.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        if (p.startsWith(QLatin1String("connected="))) st.connected = p.mid(10);
        else if (p.startsWith(QLatin1String("paused="))) st.paused = p.mid(7) != QLatin1String("0");
    }
    return st;
}

bool AgentClient::sendConfig() const
{
    QLocalSocket s;
    s.connectToServer(socketPath());
    if (!s.waitForConnected(500)) return false;
    s.write(configBlock());
    if (!s.waitForBytesWritten(500)) return false;
    while (!s.canReadLine()) {
        if (!s.waitForReadyRead(500)) return false;
    }
    return QString::fromUtf8(s.readLine()).startsWith(QLatin1String("OK"));
}

bool AgentClient::haptic(int strength, int sharpness, int depth) const
{
    return ok(QStringLiteral("HAPTIC %1 %2 %3").arg(strength).arg(sharpness).arg(depth));
}

AgentLink::AgentLink(QObject *parent) : QObject(parent)
{
    connect(&m_socket, &QLocalSocket::readyRead, this, [this] { onReadyRead(); });
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_socket.write("SUBSCRIBE\n");
        m_socket.write(configBlock());
    });
}

void AgentLink::ensureConnected()
{
    if (m_socket.state() == QLocalSocket::UnconnectedState) m_socket.connectToServer(AgentClient::socketPath());
}

// 返事（OK/ERR で始まる行）は読み捨て、ACTION <n> だけ取り出す
void AgentLink::onReadyRead()
{
    while (m_socket.canReadLine()) {
        const QString line = QString::fromUtf8(m_socket.readLine()).trimmed();
        if (!line.startsWith(QLatin1String("ACTION "))) continue;
        bool ok = false;
        const int n = line.mid(7).toInt(&ok);
        if (ok) emit actionReceived(n);
    }
}
