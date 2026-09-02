#include "toggle_switch.h"

#include <QPainter>

#include "theme/tokens.h"

ToggleSwitch::ToggleSwitch(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(26, 14);
}

QSize ToggleSwitch::sizeHint() const { return QSize(26, 14); }

void ToggleSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(isChecked() ? Tokens::ACCENT : Tokens::BORDER_STRONG);
    p.drawRoundedRect(rect(), height() / 2.0, height() / 2.0);

    const qreal knobD = height() - 4.0;
    const qreal knobX = isChecked() ? width() - knobD - 2.0 : 2.0;
    p.setBrush(Tokens::BG_SURFACE);
    p.drawEllipse(QRectF(knobX, 2.0, knobD, knobD));
}
