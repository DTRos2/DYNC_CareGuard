#include "careguard_date_edit.h"

#include <QPainter>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"

CareGuardDateEdit::CareGuardDateEdit(const QDate& date, QWidget* parent) : QDateEdit(date, parent) {}

void CareGuardDateEdit::paintEvent(QPaintEvent* event) {
    QDateEdit::paintEvent(event);

    const QColor color = isReadOnly() ? Tokens::BORDER_STRONG : Tokens::TEXT_SECONDARY;
    constexpr int kIconSize = 10;
    constexpr int kDropDownWidth = 22;
    const QRect iconRect(width() - kDropDownWidth + (kDropDownWidth - kIconSize) / 2,
                          (height() - kIconSize) / 2, kIconSize, kIconSize);

    QPainter painter(this);
    SimpleIcons::chevronDown(color, kIconSize).paint(&painter, iconRect);
}
