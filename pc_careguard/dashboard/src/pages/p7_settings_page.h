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
class WsClient;
class QJsonObject;
class QLineEdit;
class QComboBox;

// P7 시스템 설정 — 로봇 관리 / 계정 관리 / 임계값·알림 3탭.
// 사용자 로그·임계값·알림 로그·경로순찰 로그 3탭은 2026-09-11 사이드바 개편으로
// 별도 "로그" 항목(P8LogsPage)으로 분리했다.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 6), §7 참고.
class P7SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit P7SettingsPage(QWidget* parent = nullptr);

    // 로그인 성공 시 main.cpp -> MainWindow를 거쳐 실제 로그인 ID를 전달받는다.
    // 계정 삭제 체크박스/버튼 노출(isCurrentUserAdmin()) 판단의 기준이 된다.
    void setCurrentUserLoginId(const QString& loginId);

    // 로그인 성공 시 받은 세션 토큰. /robots, /accounts 등 요청에 Authorization 헤더로 실어 보낸다.
    void setAuthToken(const QString& token) { authToken_ = token; }

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
        int guardianUserId = -1;
        int id = -1;
        QString code;         // "C-001" — 담당 대상자 입력창에서 타이핑으로 지정하는 값
        QString name;          // "김순자" — 재배정/비활성화 경고 메시지 조립용
        QString label;         // "김순자 (C-001) · R-01" 형태
        int operatorId = -1;  // 현재 담당 계정 id. 미배정이면 -1
    };

    QWidget* buildRobotTab();
    QWidget* buildAccountTab();
    QWidget* buildThresholdTab();

    void openRobotDialog(int rowIndex);  // -1이면 신규 등록
    void populateRobotRow(int r);
    void refreshRobotTable();
    void fetchRobots();  // GET /robots
    void handleRobotsReply(QNetworkReply* reply);
    void registerRobot(const RobotDialog::RobotData& data);  // POST /robots
    void updateRobot(int robotDbId, const RobotDialog::RobotData& data);  // PUT /robots/{id}
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
    void assignRecipients(int userId, const QVector<int>& recipientIds, AccountDialog::Role role);  // PUT /accounts/{id}/recipients

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

    // setCurrentUserLoginId()로 실제 로그인 ID가 들어오기 전까지의 기본값.
    QString currentUserLoginId_ = QStringLiteral("admin");

    QComboBox* thresholdRobotCombo_ = nullptr;
    QLineEdit* batteryAttentionEdit_ = nullptr;
    QLineEdit* batteryReturnEdit_ = nullptr;
    QLineEdit* voiceWaitEdit_ = nullptr;
    QLineEdit* reconnectRetryEdit_ = nullptr;
    QLineEdit* medMissedWaitEdit_ = nullptr;
    QLineEdit* medReannounceEdit_ = nullptr;

    QNetworkAccessManager* netManager_ = nullptr;
    QString serverUrl_;
    QString authToken_;

    std::vector<RobotRow> robots_;
    std::vector<AccountRow> accounts_;
    std::vector<RecipientRow> recipients_;
};
