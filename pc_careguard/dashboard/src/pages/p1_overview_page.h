#pragma once
#include <QWidget>

#include <vector>

#include "theme/tokens.h"

class QTableWidget;
class QListWidget;

// P1 통합 관제 — 페이지 헤더 / KPI 요약 / 로봇·대상자 현황 테이블 / 실시간 알림 피드.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 0), §7 참고.
class P1OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit P1OverviewPage(QWidget* parent = nullptr);

private:
    enum class SortMode { Risk, RobotId, Name };

    struct RobotRow {
        QString id;
        Tokens::Level level;
        QString address;
        QString recipient;
        int battery;    // %, -1 = 알 수 없음
        int medSteps;   // 0~3, -1 = 알 수 없음
        QString mission;
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

    void sortRows(SortMode mode);
    void populateTable();

    QTableWidget* table_ = nullptr;
    QListWidget* alertList_ = nullptr;
    std::vector<RobotRow> rows_;
    std::vector<AlertRow> alerts_;
};
