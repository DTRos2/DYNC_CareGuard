#pragma once
#include <QWidget>

#include <vector>

#include "theme/tokens.h"

class QTableWidget;

// P7 시스템 설정 — 로봇 관리 / 계정 관리 / 임계값·알림 / 감사 로그 4탭.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 6), §7 참고.
class P7SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit P7SettingsPage(QWidget* parent = nullptr);

private:
    struct RobotRow {
        QString id;
        QString ip;
        QString ns;
        QString mapRecipient;
        Tokens::Level level;
        QString statusDetail;  // 온라인/오프라인 상세 텍스트
        QString nodeSummary;   // "6/6 정상" 등
        // --- 로봇 등록/수정 모달용 (와이어프레임 대응) ---
        QString name;               // 표시 이름, 예: "3호기"
        QString model = QStringLiteral("limo");
        int rosDomainId = 0;
        QString recipientLabel = QStringLiteral("미배정");  // "이말순 (C-003)" 형태
        QString wsHost = QStringLiteral("192.168.0.9");      // 서버 WebSocket 호스트 (데모용 고정값)
        QString connectionDetail;   // "최근 ping 응답 0.4s 전 · 세션 3d 2h"
    };

    struct NodeDetail {
        QString name;
        bool warn;
        QString detail;  // "up 3d 2h" 또는 "응답 지연 2.1s"
    };

    struct AccountRow {
        QString id;
        QString name;
        QString role;    // 관리자/관제/보호자
        QString charge;  // 담당 대상자·로봇
        QString lastLogin;
        bool active;
    };

    struct AuditRow {
        QString time;
        QString account;
        QString action;
        QString detail;
        bool highlight;  // 영상 열람·이벤트 완료 등 책임 추적 대상
    };

    QWidget* buildRobotTab();
    QWidget* buildAccountTab();
    QWidget* buildThresholdTab();
    QWidget* buildAuditTab();

    void openRobotDialog(int rowIndex);  // -1이면 신규 등록
    void populateRobotRow(int r);
    void refreshRobotTable();

    QTableWidget* robotTable_ = nullptr;

    std::vector<RobotRow> robots_;
    std::vector<NodeDetail> r03Nodes_;
    std::vector<AccountRow> accounts_;
    std::vector<AuditRow> auditLog_;
};
