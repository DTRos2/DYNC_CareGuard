#pragma once
#include <QWidget>

// P1 통합 관제 — 담당 팀원이 이 페이지의 메인 뷰를 구현한다.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 0), §7 참고.
class P1OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit P1OverviewPage(QWidget* parent = nullptr);
};
