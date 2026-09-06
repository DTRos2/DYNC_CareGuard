#include "net/ws_client.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

namespace {
constexpr int kReconnectIntervalMs = 3000;
}

WsClient::WsClient(QObject* parent) : QObject(parent) {
    QString serverUrl = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_URL"), QStringLiteral("http://127.0.0.1:8000"));
    // http(s):// -> ws(s):// 로 바꿔서 /ws/dashboard에 붙는다.
    wsUrl_ = serverUrl.replace(QStringLiteral("https://"), QStringLiteral("wss://"))
                 .replace(QStringLiteral("http://"), QStringLiteral("ws://")) +
             QStringLiteral("/ws/dashboard");

    socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    connect(socket_, &QWebSocket::connected, this, [this] { emit connectionStateChanged(true); });
    connect(socket_, &QWebSocket::disconnected, this, [this] {
        emit connectionStateChanged(false);
        scheduleReconnect();
    });
    connect(socket_, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
        const QJsonObject obj = QJsonDocument::fromJson(text.toUtf8()).object();
        if (!obj.isEmpty()) emit messageReceived(obj);
    });
    // 연결 자체가 안 되는 경우(서버 미기동 등)도 재연결 루프로 이어지게 한다.
    connect(socket_, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { scheduleReconnect(); });

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, &WsClient::connectToServer);
}

void WsClient::connectToServer() {
    if (socket_->state() == QAbstractSocket::ConnectedState ||
        socket_->state() == QAbstractSocket::ConnectingState) {
        return;
    }
    socket_->open(QUrl(wsUrl_));
}

void WsClient::scheduleReconnect() { reconnectTimer_->start(kReconnectIntervalMs); }
