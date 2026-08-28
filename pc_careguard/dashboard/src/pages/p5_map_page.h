#pragma once
#include <QWidget>

// P5 맵·로봇 매핑 — 담당 팀원이 이 페이지의 메인 뷰를 구현한다.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 4), §7 참고.
class P5MapPage : public QWidget {
    Q_OBJECT
public:
    explicit P5MapPage(QWidget* parent = nullptr);
};
