#pragma once
#include <QWidget>

// P6 순찰·경로 — 담당 팀원이 이 페이지의 메인 뷰를 구현한다.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 5), §7 참고.
class P6PatrolPage : public QWidget {
    Q_OBJECT
public:
    explicit P6PatrolPage(QWidget* parent = nullptr);
};
