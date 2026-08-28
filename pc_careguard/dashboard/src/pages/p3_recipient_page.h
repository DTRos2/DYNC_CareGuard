#pragma once
#include <QWidget>

// P3 대상자 관리 — 담당 팀원이 이 페이지의 메인 뷰를 구현한다.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 2), §7 참고.
class P3RecipientPage : public QWidget {
    Q_OBJECT
public:
    explicit P3RecipientPage(QWidget* parent = nullptr);
};
