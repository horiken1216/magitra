#include "agentclient.h"

#include <QByteArray>
#include <QLocalSocket>
#include <QStringList>

QString AgentClient::socketPath()
{
    return qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/magitra/agent.sock");
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

bool AgentClient::haptic(int strength, int sharpness, int depth) const
{
    return ok(QStringLiteral("HAPTIC %1 %2 %3").arg(strength).arg(sharpness).arg(depth));
}
