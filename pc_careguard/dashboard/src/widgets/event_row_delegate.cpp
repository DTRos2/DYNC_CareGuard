#include "event_row_delegate.h"

#include <QPainter>

#include "event_table_model.h"
#include "theme/tokens.h"

EventRowDelegate::EventRowDelegate(const bool* pulseOn, const int* hoveredRow, QObject* parent)
    : QStyledItemDelegate(parent), pulseOn_(pulseOn), hoveredRow_(hoveredRow) {}

void EventRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    const int kind = index.data(EventTableModel::RowKindRole).toInt();
    const bool hovered = index.row() == *hoveredRow_;
    const bool checked =
        index.sibling(index.row(), EventTableModel::ColCheck).data(Qt::CheckStateRole).toInt() ==
        Qt::Checked;

    QColor bg;
    QColor barColor;
    switch (static_cast<EventTableModel::RowKind>(kind)) {
        case EventTableModel::RowKind::Emergency:
            bg = *pulseOn_ ? Tokens::EMERGENCY_PULSE_A : Tokens::EMERGENCY_PULSE_B;
            barColor = Tokens::EMERGENCY.main;
            break;
        case EventTableModel::RowKind::Attention:
            bg = Tokens::ATTENTION.soft;
            barColor = Tokens::ATTENTION.main;
            break;
        case EventTableModel::RowKind::Normal:
        default:
            bg = hovered ? Tokens::BG_HOVER : Tokens::BG_SURFACE;
            barColor = QColor();
            break;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->fillRect(option.rect, bg);

    if (barColor.isValid() && index.column() == EventTableModel::ColCheck) {
        QRect barRect(option.rect.left(), option.rect.top(), Tokens::Layout::STATUS_BAR_W,
                      option.rect.height());
        painter->fillRect(barRect, barColor);
    }

    if (checked) {
        // 선택 외곽선은 drawLine(펜)이 아니라 fillRect로 그린다. 호버로 셀 하나만
        // 부분 리페인트될 때 펜 굵기(1.5px)의 반올림이 리페인트 범위에 따라 들쭉날쭉해져
        // 선이 얇아 보이는 문제가 있었다 — fillRect는 항상 같은 정수 픽셀 두께로 나온다.
        constexpr int kOutline = 2;
        painter->fillRect(QRect(option.rect.left(), option.rect.top(), option.rect.width(), kOutline),
                           Tokens::ACCENT);
        painter->fillRect(QRect(option.rect.left(), option.rect.bottom() - kOutline + 1,
                                 option.rect.width(), kOutline),
                           Tokens::ACCENT);
        if (index.column() == EventTableModel::ColCheck) {
            painter->fillRect(
                QRect(option.rect.left(), option.rect.top(), kOutline, option.rect.height()),
                Tokens::ACCENT);
        }
        if (index.column() == index.model()->columnCount() - 1) {
            painter->fillRect(QRect(option.rect.right() - kOutline + 1, option.rect.top(), kOutline,
                                     option.rect.height()),
                               Tokens::ACCENT);
        }
    }
    painter->restore();

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    // State_HasFocus를 지우지 않으면 클릭(=현재 인덱스 지정)한 셀에 스타일 기본
    // 포커스 사각형이 남아, 모달을 닫아도 클릭했던 칸에 테두리 박스가 계속 보인다.
    opt.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus);
    opt.showDecorationSelected = false;
    opt.backgroundBrush = Qt::NoBrush;
    QStyledItemDelegate::paint(painter, opt, index);
}

QSize EventRowDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    s.setHeight(Tokens::Layout::TABLE_ROW_H);
    return s;
}
