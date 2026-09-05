#pragma once
#include <QStyledItemDelegate>

// P2 이벤트 테이블 행 페인팅. CareGuard_DESIGN_SYSTEM.md §4-3.
// 상태별 배경 + 좌측 4px 바(0번 컬럼에서만) + 선택 행 ACCENT 외곽선 + EMERGENCY 배경 펄스.
class EventRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // pulseOn을 상위(P2EventPage)에서 2초 주기로 토글하고 viewport()->update()를 호출한다.
    // hoveredRow는 상위가 뷰포트 마우스 이동을 추적해 갱신하는 (프록시 기준) 행 번호로,
    // Qt 기본 State_MouseOver는 셀 단위라 열 하나만 칠해지는 문제가 있어 행 단위로 직접 판정한다.
    explicit EventRowDelegate(const bool* pulseOn, const int* hoveredRow, QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    const bool* pulseOn_;
    const int* hoveredRow_;
};
