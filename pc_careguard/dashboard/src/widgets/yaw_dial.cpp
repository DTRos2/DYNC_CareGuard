#include "yaw_dial.h"

#include <QMouseEvent>
#include <QPainter>
#include <cmath>

#include "theme/tokens.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

YawDial::YawDial(QWidget* parent) : QWidget(parent), angleDeg_(0) {
    setFixedSize(36, 36);
    setCursor(Qt::PointingHandCursor);
}

void YawDial::setAngleDeg(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg == angleDeg_) return;
    angleDeg_ = deg;
    update();
}

void YawDial::updateFromPos(const QPointF& pos) {
    const QPointF center(width() / 2.0, height() / 2.0);
    const qreal dx = pos.x() - center.x();
    const qreal dy = pos.y() - center.y();
    // 화면 y축은 아래로 증가하므로 -dy로 뒤집어 "위=0도, 시계방향 증가"가 아니라
    // 표준 수학 좌표(오른쪽=0도, 반시계 증가)의 화면 버전인 "오른쪽=0도, 시계 증가"를 사용.
    const qreal rad = std::atan2(dy, dx);
    int deg = static_cast<int>(std::round(rad * 180.0 / M_PI));
    if (deg < 0) deg += 360;
    setAngleDeg(deg);
    emit angleChanged(angleDeg_);
}

void YawDial::mousePressEvent(QMouseEvent* event) {
    updateFromPos(event->position());
}

void YawDial::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) updateFromPos(event->position());
}

void YawDial::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QPointF center(width() / 2.0, height() / 2.0);
    const qreal r = qMin(width(), height()) / 2.0 - 2.0;

    p.setPen(QPen(Tokens::BORDER_STRONG, 1.0));
    p.setBrush(Tokens::BG_BASE);
    p.drawEllipse(center, r, r);

    const qreal rad = angleDeg_ * M_PI / 180.0;
    const QPointF tip(center.x() + std::cos(rad) * (r - 3), center.y() + std::sin(rad) * (r - 3));
    p.setPen(QPen(Tokens::ACCENT, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(center, tip);
    p.setPen(QPen(Tokens::ACCENT, 1.5));
    p.setBrush(Tokens::BG_SURFACE);
    p.drawEllipse(tip, 3.0, 3.0);
}
