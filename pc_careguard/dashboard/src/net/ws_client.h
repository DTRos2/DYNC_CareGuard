#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>

class QWebSocket;
class QTimer;

// /ws/dashboard 실시간 연결. 서버가 로봇 위치·SLAM 진행률·순찰 진행률·새 영상 클립 등을
// push하면 messageReceived로 그대로 넘겨준다. 끊기면 일정 간격으로 자동 재연결을 시도한다
// (체크리스트 ⑩ — WS 연결 자체의 안정성).
class WsClient : public QObject {
    Q_OBJECT
public:
    explicit WsClient(QObject* parent = nullptr);

    void connectToServer();

signals:
    // 서버가 보낸 메시지 하나. "type" 필드로 종류를 구분한다:
    // robot_position / slam_progress / slam_done / patrol_progress / patrol_done / robot_status /
    // video_event_created.
    void messageReceived(const QJsonObject& message);
    void connectionStateChanged(bool connected);

private:
    void scheduleReconnect();

    QWebSocket* socket_;
    QString wsUrl_;
    QTimer* reconnectTimer_;
};
