#pragma once
#include <QWidget>

#include <vector>

class QTableWidget;
class QLabel;
class QPushButton;
class QNetworkAccessManager;
class QNetworkReply;
class QDateEdit;
class QEvent;
class QJsonObject;
class LiveSearchLineEdit;
class WsClient;

// P8 로그 조회 — 사용자 로그 / 임계값·알림 로그 / 경로순찰 로그 3탭.
// 원래는 P7SettingsPage 안의 탭 3개였으나, 2026-09-11 사이드바 개편으로 "설정" 그룹
// 아래 별도 "로그" 항목으로 분리했다. 로봇/계정/임계값 관리(P7SettingsPage)와 상태를
// 공유하지 않는 완전히 독립된 하위 시스템이라 분리에 구조적 장애물이 없었다 — 이
// 페이지의 각 탭은 자기 자신의 멤버(테이블/날짜필터/검색창/데이터)만 사용한다.
class P8LogsPage : public QWidget {
    Q_OBJECT
public:
    explicit P8LogsPage(QWidget* parent = nullptr);

protected:
    // 기간 세그먼트가 "직접"이 아닐 때 읽기 전용 날짜 칸을 클릭하면 "직접"로 전환한다.
    // (P2EventPage::eventFilter, 분리 전 P7SettingsPage::eventFilter와 동일한 패턴)
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct AuditRow {
        QString time;
        QString account;
        QString action;
        QString detail;
        bool highlight;  // 영상 열람·이벤트 완료 등 책임 추적 대상
    };

    // GET /system-events 응답 한 줄 — 감사 로그(AuditRow)와 같은 모양으로 맞춰서
    // buildAuditTab()과 같은 방식으로 그린다.
    struct SystemEventLogRow {
        QString time;
        QString robotCode;
        QString eventType;
        QString message;
        bool highlight;  // severity == error(복귀 필요) 등
    };

    // GET /patrol-runs/log 응답 한 줄.
    struct PatrolRunLogRow {
        QString time;
        QString robotCode;
        QString recipient;
        QString route;
        QString trigger;
        QString status;
        QString progress;  // "3/5" 형태
        bool highlight;     // 중단·실패
    };

    QWidget* buildAuditTab();
    QWidget* buildSystemEventLogTab();
    QWidget* buildPatrolRunLogTab();

    void onAuditDateRangeChanged();
    void applyAuditPeriodPreset(int spanDays);  // -1 = 직접, 0/6/29 = 오늘/7일/30일
    void setAuditDateEditsReadOnly(bool readOnly);
    void fetchAuditLog();  // GET /audit-log?start=&end=&q= — 기간/검색 바뀔 때마다 다시 호출
    void handleAuditLogReply(QNetworkReply* reply);
    void refreshAuditTable();  // auditLog_ 내용을 auditTable_에 그린다

    // 아래 두 탭은 buildAuditTab()과 완전히 같은 필터 UI(기간 세그먼트+날짜+검색) 관례를
    // 따른다 — 위 buildAuditTab() 구현의 주석 참고, 여기서는 반복하지 않는다.
    void onSystemEventDateRangeChanged();
    void applySystemEventPeriodPreset(int spanDays);
    void setSystemEventDateEditsReadOnly(bool readOnly);
    void fetchSystemEventLog();  // GET /system-events?start=&end=&q=
    void handleSystemEventLogReply(QNetworkReply* reply);
    void refreshSystemEventTable();

    void onPatrolRunDateRangeChanged();
    void applyPatrolRunPeriodPreset(int spanDays);
    void setPatrolRunDateEditsReadOnly(bool readOnly);
    void fetchPatrolRunLog();  // GET /patrol-runs/log?start=&end=&q=
    void handlePatrolRunLogReply(QNetworkReply* reply);
    void refreshPatrolRunTable();

    // 2026-09-12 추가 — 세 탭 다 원래는 페이지를 열거나 필터를 바꿀 때만 다시 불렀다
    // (진짜 "실시간"은 아니었음). /ws/dashboard의 audit_log_created/system_event_created/
    // patrol_run_created(및 이미 있던 patrol_progress/patrol_done)를 받으면 해당 탭의
    // fetch*Log()를 다시 불러 지금 적용 중인 필터 그대로 최신 상태로 갱신한다.
    void handleWsMessage(const QJsonObject& message);

    WsClient* wsClient_ = nullptr;

    QTableWidget* auditTable_ = nullptr;
    QDateEdit* auditStartDateEdit_ = nullptr;
    QDateEdit* auditEndDateEdit_ = nullptr;
    QPushButton* auditPeriodCustomBtn_ = nullptr;
    LiveSearchLineEdit* auditSearchEdit_ = nullptr;
    QString auditSearchText_;

    QTableWidget* systemEventTable_ = nullptr;
    QDateEdit* systemEventStartDateEdit_ = nullptr;
    QDateEdit* systemEventEndDateEdit_ = nullptr;
    QPushButton* systemEventPeriodCustomBtn_ = nullptr;
    LiveSearchLineEdit* systemEventSearchEdit_ = nullptr;
    QString systemEventSearchText_;

    QTableWidget* patrolRunTable_ = nullptr;
    QDateEdit* patrolRunStartDateEdit_ = nullptr;
    QDateEdit* patrolRunEndDateEdit_ = nullptr;
    QPushButton* patrolRunPeriodCustomBtn_ = nullptr;
    LiveSearchLineEdit* patrolRunSearchEdit_ = nullptr;
    QString patrolRunSearchText_;

    QNetworkAccessManager* netManager_ = nullptr;
    QString serverUrl_;

    std::vector<AuditRow> auditLog_;
    std::vector<SystemEventLogRow> systemEventLog_;
    std::vector<PatrolRunLogRow> patrolRunLog_;
};
