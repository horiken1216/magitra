/*
 * agentclient.h - 常駐アプリ（magitrad）との通信（$XDG_RUNTIME_DIR/magitra/agent.sock の 1 行プロトコル）
 */
#ifndef MAGITRA_AGENTCLIENT_H
#define MAGITRA_AGENTCLIENT_H

#include <QString>

class AgentClient {
public:
    struct Status {
        bool running = false;           /* 常駐アプリに届いたか */
        QString connected = "none";     /* none / usb / bt */
        bool paused = false;
    };

    static QString socketPath();

    /* 1 行送って 1 行の返事を返す。届かなければ空 */
    QString request(const QString &line, int timeoutMs = 500) const;
    bool ok(const QString &line) const;

    Status status() const;
    bool reload() const { return ok(QStringLiteral("RELOAD")); }
    bool haptic(int strength, int sharpness, int depth) const;
};

#endif
