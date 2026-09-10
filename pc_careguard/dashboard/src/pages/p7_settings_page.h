#pragma once
#include <QVector>
#include <QWidget>

#include <vector>

#include "dialogs/p7_account_dialog.h"
#include "dialogs/p7_robot_dialog.h"
#include "theme/tokens.h"

class QTableWidget;
class QLabel;
class QPushButton;
class QNetworkAccessManager;
class QNetworkReply;
class QDateEdit;
class QEvent;
class LiveSearchLineEdit;
class WsClient;
class QJsonObject;
class QLineEdit;
class QComboBox;

// P7 시스템 설정 — 로봇 관리 / 계정 관리 / 임계값·알림 / 감사 로그 4탭.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 6), §7 참고.
class P7SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit P7SettingsPage(QWidget* parent = nullptr);

    // 로그인 성공 시 main.cpp -> MainWindow를 거쳐 실제 로그인 ID를 전달받는다.
    // 계정 삭제 체크박스/버튼 노출(isCurrentUserAdmin()) 판단의 기준이 된다.
    void setCurrentUserLoginId(const QString& loginId);

protected:
    // 감사 로그 탭 기간 세그먼트가 "직접"이 아닐 때 날짜 칸은 읽기 전용으로 계산된
    // 범위만 보여준다. 그 상태에서 칸을 클릭하면 "직접"로 전환해야 하는데, 읽기 전용
    // 위젯은 클릭을 그대로 통과시키므로(=편집 불가만 막힘) 여기서 잡아서 처리한다.
    // (P2EventPage::eventFilter와 동일한 패턴)
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct RobotRow {
        int dbId = -1;  // robot.id — 등록 해제(DELETE /robots/{id}) API 호출에 필요
        QString id;
        QString ip;
        QString ns;
        QString mapRecipient;
        Tokens::Level level;
        QString statusDetail;  // 온라인/오프라인 상세 텍스트
        // --- 로봇 등록/수정 모달용 (와이어프레임 대응) ---
        QString name;               // 표시 이름, 예: "3호기"
        QString model = QStringLiteral("limo");
        int rosDomainId = 0;
        QString recipientLabel = QStringLiteral("미배정");  // "이말순 (C-003)" 형태
        QString wsHost = QStringLiteral("192.168.0.9");      // 서버 WebSocket 호스트 (데모용 고정값)
        QString connectionDetail;   // "최근 ping 응답 0.4s 전 · 세션 3d 2h"
    };

    struct AccountRow {
        int dbId = -1;   // careguard_user.id — 담당 배정 API 호출에 필요
        QString id;
        QString name;
        QString role;    // 관리자/관제/보호자
        QString charge;  // 담당 대상자·로봇
        QString lastLogin;
        bool active;
    };

    // /accounts/recipients 응답 한 줄. 계정 수정 모달의 담당 배정 체크리스트 원본.
    struct RecipientRow {
        int id = -1;
        QString code;         // "C-001" — 담당 대상자 입력창에서 타이핑으로 지정하는 값
        QString name;          // "김순자" — 재배정/비활성화 경고 메시지 조립용
        QString label;         // "김순자 (C-001) · R-01" 형태
        int operatorId = -1;  // 현재 담당 계정 id. 미배정이면 -1
    };

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

    // GET /patrol-runs 응답 한 줄.
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

    QWidget* buildRobotTab();
    QWidget* buildAccountTab();
    QWidget* buildThresholdTab();
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
    void fetchPatrolRunLog();  // GET /patrol-runs?start=&end=&q=
    void handlePatrolRunLogReply(QNetworkReply* reply);
    void refreshPatrolRunTable();

    void openRobotDialog(int rowIndex);  // -1이면 신규 등록
    void populateRobotRow(int r);
    void refreshRobotTable();
    void fetchRobots();  // GET /robots
    void handleRobotsReply(QNetworkReply* reply);
    void registerRobot(const RobotDialog::RobotData& data);  // POST /robots
    void assignRobotRecipient(int robotDbId, int recipientId);  // PUT /robots/{id}/recipient
    void deregisterRobot(int dbId);  // DELETE /robots/{id}
    void handleWsMessage(const QJsonObject& message);  // WsClient::messageReceived — type으로 분기

    void openAccountDialog(int rowIndex);  // -1이면 신규 등록
    void populateAccountRow(int r);
    void refreshAccountTable();
    bool isLastActiveAdmin(int rowIndex) const;
    bool isCurrentUserAdmin() const;  // 계정 삭제 체크박스/버튼 노출 여부 판단용
    void handleDeleteAccountsClicked();

    void fetchAccounts();  // GET /accounts — 계정 관리 탭 진입 시 DB에서 불러온다
    void handleAccountsReply(QNetworkReply* reply);
    void registerAccount(const AccountDialog::AccountData& data);  // POST /accounts
    void updateAccount(int rowIndex, const AccountDialog::AccountData& data);  // PATCH /accounts/{id}
    void changePassword(int userId, const QString& newPassword);  // PATCH /accounts/{id}/password
    void activateAccount(int rowIndex);  // PATCH /accounts/{id} — 비활성 계정의 "활성화" 링크용
    void deleteAccounts(const QVector<int>& userIds);  // POST /accounts/delete

    void fetchRecipients();  // GET /accounts/recipients — 담당 배정 체크리스트 원본
    void handleRecipientsReply(QNetworkReply* reply);
    void assignRecipients(int userId, const QVector<int>& recipientIds);  // PUT /accounts/{id}/recipients

    void refreshThresholdRobotCombo();  // robots_ 갱신될 때마다 콤보 항목도 다시 채운다
    void fetchRobotThresholds(int robotDbId);  // GET /robots/{id}/thresholds — 콤보 선택 바뀔 때
    void saveThresholdsForCurrentRobot();  // PUT /robots/{id}/thresholds — "이 로봇에 저장"
    void applyThresholdsToAllRobots();  // PUT /robots/thresholds/apply-all — "전체 로봇에 적용"

    QTableWidget* robotTable_ = nullptr;
    QLabel* robotCountLabel_ = nullptr;
    QTableWidget* accountTable_ = nullptr;
    QLabel* accountCountLabel_ = nullptr;
    QPushButton* accountRegisterBtn_ = nullptr;
    QPushButton* deleteAccountBtn_ = nullptr;
    WsClient* wsClient_ = nullptr;

    QTableWidget* auditTable_ = nullptr;
    QDateEdit* auditStartDateEdit_ = nullptr;
    QDateEdit* auditEndDateEdit_ = nullptr;
    QPushButton* auditPeriodCustomBtn_ = nullptr;
    LiveSearchLineEdit* auditSearchEdit_ = nullptr;
    QString auditSearchText_;
    // setCurrentUserLoginId()로 실제 로그인 ID가 들어오기 전까지의 기본값.
    QString currentUserLoginId_ = QStringLiteral("admin");

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

    QComboBox* thresholdRobotCombo_ = nullptr;
    QLineEdit* batteryAttentionEdit_ = nullptr;
    QLineEdit* batteryReturnEdit_ = nullptr;
    QLineEdit* voiceWaitEdit_ = nullptr;
    QLineEdit* reconnectRetryEdit_ = nullptr;
    QLineEdit* medMissedWaitEdit_ = nullptr;
    QLineEdit* medReannounceEdit_ = nullptr;

    QNetworkAccessManager* netManager_ = nullptr;
    QString serverUrl_;

    std::vector<RobotRow> robots_;
    std::vector<AccountRow> accounts_;
    std::vector<RecipientRow> recipients_;
    std::vector<AuditRow> auditLog_;
    std::vector<SystemEventLogRow> systemEventLog_;
    std::vector<PatrolRunLogRow> patrolRunLog_;
};
