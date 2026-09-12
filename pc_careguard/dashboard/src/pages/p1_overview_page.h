#pragma once
#include <QMap>
#include <QVector>
#include <QWidget>

#include "net/api_client.h"
#include "theme/tokens.h"

class QTableWidget;
class QListWidget;
class QJsonObject;
class QTimer;
class WsClient;

// P1 통합 관제 — 페이지 헤더 / KPI 요약 / 로봇·대상자 현황 테이블 / 실시간 알림 피드.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 0), §7 참고.
// 2026-09-12까지는 이 페이지 전체가 하드코딩된 목데이터였다 — GET /robots, GET
// /recipients, GET /events와 /ws/dashboard(battery_state/robot_status/nav_status)로
// 실제 서버 데이터에 연동했다. 배터리는 서버가 그동안 대시보드로 중계를 안 하고 있어서
// (robot_ws.py::_handle_message의 battery_state 분기, 저배터리 자동 복귀 판단에만
// 썼음) 서버 쪽에도 dashboard_ws.broadcast를 추가해야 했다. 통신 지연(ms)은 서버에
// 그 자체를 재는 로직이 없어 여전히 표시할 수 없고("-"로 둠), 웨어러블 관련 KPI는
// 이 프로젝트에 웨어러블 데이터 자체가 없어 카드를 아예 뺐다.
class P1OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit P1OverviewPage(QWidget* parent = nullptr);

signals:
    // 로봇 상세 모달의 "이벤트로 이동" → MainWindow가 P2로 전환하고 그 이벤트를 연다
    // (P4VideoPage::eventDetailRequested와 같은 패턴).
    void eventNavigationRequested(const QString& eventCode);

private:
    enum class SortMode { Risk, RobotId, Name };

    struct RobotRow {
        QString id;
        Tokens::Level level;
        QString address;
        QString recipient;
        int battery;    // %, -1 = 알 수 없음
        int medSteps;   // 0~3, -1 = 알 수 없음(오늘 복약 스케줄 없음)
        QString mission;
        // RobotControlModal(상세 모달)이 취소·충전 복귀 명령에 그대로 쓸 이벤트 코드 —
        // 이 로봇에 연결된 미해결 이벤트가 없으면 비어있다.
        QString eventCode;
        QString eventText;
    };

    struct AlertRow {
        QString time;
        const char* level;  // Tokens::Prop::EMERGENCY / ATTENTION / INFO
        QString text;
    };

    QWidget* buildHeaderRow();
    QWidget* buildKpiRow();
    QWidget* buildTableSection();
    QWidget* buildAlertSection();

    void loadFromServer();
    void handleWsMessage(const QJsonObject& message);
    void rebuildRowsFromData();
    void populateKpiRow();
    void populateAlertFeed();

    void sortRows(SortMode mode);
    void populateTable();

    // 헤더의 "전체 비상정지"/"순찰 취소"/"충전 복귀" — 셋 다 로봇 하나를 고르는 UI가
    // 없는 페이지 레벨 버튼이라, 대상 조건에 맞는 모든 로봇에 각각 명령을 보낸다.
    void handleEstopAllClicked();
    void handleCancelAllPatrolsClicked();
    void handleReturnAllToChargeClicked();
    void sendRobotToCharge(const QString& robotCode);

    ApiClient* apiClient_ = nullptr;
    WsClient* wsClient_ = nullptr;

    // battery_state는 로봇에서 초당 여러 번 들어온다(uplink_node가 /battery_state
    // 토픽을 받는 대로 그대로 올림, 쓰로틀 없음). 올 때마다 테이블 전체를
    // rebuildRowsFromData()+populateTable()로 다시 그리면 셀 위젯이 매번 파괴·재생성돼
    // 배터리 막대가 눈에 띄게 떨린다 — 그래서 값만 즉시 갱신해두고 실제 다시 그리기는
    // 이 타이머로 묶어서(디바운스) 최대 초당 한 번만 하게 한다.
    QTimer* batteryRefreshTimer_ = nullptr;

    QTableWidget* table_ = nullptr;
    QListWidget* alertList_ = nullptr;
    QWidget* kpiRow_ = nullptr;
    std::vector<RobotRow> rows_;
    std::vector<AlertRow> alerts_;
    SortMode currentSortMode_ = SortMode::Risk;

    QVector<ApiClient::RobotInfo> robots_;
    QVector<ApiClient::RecipientInfo> recipients_;
    QVector<ApiClient::EventInfo> events_;
    // robot_code -> 배터리 %. 서버(robot_ws.py::_smooth_battery_percent)가 이미 EMA +
    // 히스테리시스로 다듬어서 내려주므로 여기서는 그대로 저장만 한다(2026-09-12,
    // 원래는 여기서 스무딩했는데 화면마다 중복 구현해야 해서 서버로 옮겼다).
    QMap<QString, int> liveBattery_;
};
