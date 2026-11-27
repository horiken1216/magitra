/*
 * agentclient.h - 常駐サービス（magitrad）との通信（/run/magitra/agent.sock の 1 行プロトコル）
 */
#ifndef MAGITRA_AGENTCLIENT_H
#define MAGITRA_AGENTCLIENT_H

#include <QLocalSocket>
#include <QObject>
#include <QString>

class AgentClient {
public:
    struct Status {
        bool running = false;           /* 常駐サービスに届いたか（許可されたか） */
        QString connected = "none";     /* none / usb / bt */
        bool paused = false;
    };

    static QString socketPath();

    /* 1 行送って 1 行の返事を返す。届かなければ空 */
    QString request(const QString &line, int timeoutMs = 500) const;
    bool ok(const QString &line) const;

    Status status() const;
    /* agent.conf の中身を CONFIG として送る（ファイルがなければ空 = 既定値） */
    bool sendConfig() const;
    bool haptic(int strength, int sharpness, int depth) const;
};

/* トレイが持つ常時接続。SUBSCRIBE と CONFIG を送り、サービスからの ACTION <n> を受ける */
class AgentLink : public QObject {
    Q_OBJECT
public:
    explicit AgentLink(QObject *parent = nullptr);
    /* つながっていなければつなぎ直す（呼び出し側が 2 秒ごとに呼ぶ） */
    void ensureConnected();

signals:
    void actionReceived(int action);

private:
    void onReadyRead();
    QLocalSocket m_socket;
};

#endif
