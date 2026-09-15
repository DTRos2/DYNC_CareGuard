#pragma once
#include <QColor>
#include <QDialog>
#include <QVector>

#include "net/api_client.h"
#include "theme/tokens.h"

class QWidget;
class QLabel;
class QPushButton;
class QTimer;
class QJsonObject;
class MiniMapView;
class WsClient;

// P1 통합관제 로봇 카드 "상세" 버튼이 여는 모달 — 배터리·상태·진행 중
// 이벤트·최근 미션을 한 화면에서 보고, 그 자리에서 순찰 취소/비상정지/이벤트 이동을
// 할 수 있게 한다. 2026-09-12 디자인 확정본에는 스피커 통화·웨어러블 상태·충전 복귀가
// 없다 — 웨어러블은 이 카드가 다룰 정보가 아니라는 판단, 충전 복귀는 "순찰 취소" 안에
// 흡수됐다(취소 5초 뒤 자동으로 충전소 웨이포인트까지 이동 명령을 보낸다).
class RobotControlModal : public QDialog {
    Q_OBJECT
public:
    struct RecentMission {
        QString time;
        QString label;
        QString status;  // 예: "완료"
    };

    struct Data {
        QString robotCode;      // 예: "R-01" — 서버 호출(취소·이동 명령)에 그대로 쓴다.
        QString headerTitle;    // 예: "R-01  행복빌라 302 · 김순자"
        QString urgencyBadge;   // 예: "긴급 · VOICE_CHECK". 비어있으면 배지를 숨긴다.
        QString mapLabel;       // 예: "행복빌라 302 맵"
        QString positionLabel;  // 예: "현재 위치  WP_01 거실 · 헤딩 42°". 비어있으면 숨긴다.
        int batteryPct = -1;    // -1 = 알 수 없음
        bool cameraOk = false;
        bool micOk = false;
        bool nav2Ok = false;
        QString eventCode;   // 비어있으면 "진행 중 이벤트" 박스를 숨긴다.
        QString eventText;
        QVector<RecentMission> recentMissions;
    };

    // wsClient가 null이 아니면 열려있는 동안 맵/웨이포인트/로봇 위치가 바뀔 때마다
    // /ws/dashboard 브로드캐스트를 받아 그 자리에서 다시 그린다(2026-09-15, 모달을
    // 닫았다 다시 열어야만 최신값이 보이던 문제 수정).
    explicit RobotControlModal(const Data& data, WsClient* wsClient, QWidget* parent = nullptr);

signals:
    // "이벤트로 이동" 클릭 — 페이지 전환은 이 모달의 책임이 아니라 호출부(P1OverviewPage
    // -> MainWindow)에 맡긴다. EventDetailModal::videoViewRequested와 같은 패턴.
    void eventNavigationRequested(const QString& eventCode);

private:
    QWidget* buildHeader();
    QWidget* buildBody();
    QWidget* buildLeftColumn();
    QWidget* buildRightColumn();
    QWidget* buildStatCard(const QString& label, const QString& value, const QColor& valueColor = Tokens::TEXT_PRIMARY);
    QWidget* buildStatusDotsRow();
    QWidget* buildEventBox();
    QWidget* buildMissionList();
    QWidget* buildFooter();

    void loadChargeWaypoint();
    void handleWsMessage(const QJsonObject& message);
    void handleCancelPatrolClicked();
    void beginReturnToChargeCountdown();
    void tickCountdown();
    void sendReturnToCharge();
    void resetCancelButton();

    Data data_;
    ApiClient* apiClient_;
    WsClient* wsClient_ = nullptr;

    MiniMapView* mapView_ = nullptr;
    QLabel* positionLabel_ = nullptr;
    QPushButton* cancelPatrolBtn_ = nullptr;

    int mapId_ = 0;
    bool chargeWaypointLoaded_ = false;
    ApiClient::WaypointInfo chargeWaypoint_;

    QTimer* countdownTimer_ = nullptr;
    int countdownRemaining_ = 0;
};
