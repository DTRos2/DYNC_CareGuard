#include "icon_line_edit.h"

#include <QMouseEvent>
#include <QPainter>

IconLineEdit::IconLineEdit(QWidget* parent) : QLineEdit(parent) {}

void IconLineEdit::setLeadingIcon(const QIcon& icon) {
    leadingIcon_ = icon;
    updateTextMargins();
    update();
}

void IconLineEdit::setTrailingIcon(const QIcon& icon) {
    trailingIcon_ = icon;
    updateTextMargins();
    update();
}

void IconLineEdit::updateTextMargins() {
    const int left = leadingIcon_.isNull() ? 0 : kIconSize + 6;
    const int right = trailingIcon_.isNull() ? 0 : kIconSize + 6;
    setTextMargins(left, 0, right, 0);
}

QRect IconLineEdit::leadingRect() const {
    return QRect(kInset, (height() - kIconSize) / 2, kIconSize, kIconSize);
}

QRect IconLineEdit::trailingRect() const {
    return QRect(width() - kInset - kIconSize, (height() - kIconSize) / 2, kIconSize, kIconSize);
}

void IconLineEdit::paintEvent(QPaintEvent* event) {
    QLineEdit::paintEvent(event);

    QPainter painter(this);
    if (!leadingIcon_.isNull()) {
        leadingIcon_.paint(&painter, leadingRect());
    }
    if (!trailingIcon_.isNull()) {
        trailingIcon_.paint(&painter, trailingRect());
    }
}

void IconLineEdit::mousePressEvent(QMouseEvent* event) {
    if (!trailingIcon_.isNull() && trailingRect().adjusted(-4, -4, 4, 4).contains(event->pos())) {
        emit trailingIconClicked();
        return;
    }
    QLineEdit::mousePressEvent(event);
}
