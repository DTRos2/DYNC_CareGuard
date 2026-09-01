#include "simple_icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <cmath>
#include <functional>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// 24x24 논리 좌표계에 그린 뒤 요청 픽셀 크기로 스케일한다 (2x 렌더링으로 선명하게).
QIcon makeIcon(const QColor& color, int px, const std::function<void(QPainter&, qreal)>& draw) {
    const qreal scale = px * 2.0 / 24.0;
    QPixmap pixmap(px * 2, px * 2);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(scale, scale);
    draw(painter, 1.8);
    painter.end();

    // devicePixelRatio는 페인팅이 끝난 뒤에 설정한다 — 페인팅 전에 설정하면 QPainter가
    // 좌표를 자동으로 다시 축소해서 위의 scale()과 이중 적용되어 도형이 잘려나간다.
    pixmap.setDevicePixelRatio(2.0);

    return QIcon(pixmap);
}

}  // namespace

namespace SimpleIcons {

QIcon user(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(8.5, 4, 7, 7));
        QPainterPath shoulders;
        shoulders.moveTo(4.5, 20);
        shoulders.cubicTo(4.5, 15, 8, 13, 12, 13);
        shoulders.cubicTo(16, 13, 19.5, 15, 19.5, 20);
        p.drawPath(shoulders);
    });
}

QIcon lock(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath shackle;
        shackle.moveTo(7.5, 10.5);
        shackle.arcTo(QRectF(7.5, 3.5, 9, 9), 180, 180);
        p.drawPath(shackle);
        p.drawRoundedRect(QRectF(5.5, 10.5, 13, 10), 2.2, 2.2);
    });
}

QIcon eye(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath eyeShape;
        eyeShape.moveTo(3, 12);
        eyeShape.cubicTo(6, 6, 18, 6, 21, 12);
        eyeShape.cubicTo(18, 18, 6, 18, 3, 12);
        p.drawPath(eyeShape);
        p.drawEllipse(QPointF(12, 12), 2.6, 2.6);
    });
}

QIcon eyeOff(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath eyeShape;
        eyeShape.moveTo(3, 12);
        eyeShape.cubicTo(6, 6, 18, 6, 21, 12);
        eyeShape.cubicTo(18, 18, 6, 18, 3, 12);
        p.drawPath(eyeShape);
        p.drawEllipse(QPointF(12, 12), 2.6, 2.6);
        p.drawLine(QPointF(4, 20), QPointF(20, 4));
    });
}

QIcon alertCircle(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(2.5, 2.5, 19, 19));
        p.drawLine(QPointF(12, 7.5), QPointF(12, 13.5));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 17), 1.1, 1.1);
    });
}

QIcon lockExclamation(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath shackle;
        shackle.moveTo(6.5, 9.5);
        shackle.arcTo(QRectF(6.5, 3, 8, 8), 180, 180);
        p.drawPath(shackle);
        p.drawRoundedRect(QRectF(4.5, 9.5, 12, 9), 2, 2);
        p.drawLine(QPointF(19, 15), QPointF(19, 18.2));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(19, 20.6), 1.0, 1.0);
    });
}

QIcon plugOff(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(8.5, 3), QPointF(8.5, 9));
        p.drawLine(QPointF(15.5, 3), QPointF(15.5, 9));
        p.drawRoundedRect(QRectF(6.5, 9, 11, 7), 2, 2);
        p.drawLine(QPointF(12, 16), QPointF(12, 21));
        p.drawLine(QPointF(3, 21), QPointF(21, 3));
    });
}

QIcon refresh(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QRectF box(3.5, 3.5, 17, 17);
        p.drawArc(box, 20 * 16, 300 * 16);
        QPainterPath arrow;
        arrow.moveTo(19.2, 3.8);
        arrow.lineTo(20.6, 8.2);
        arrow.lineTo(16.1, 7.4);
        arrow.closeSubpath();
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPath(arrow);
    });
}

QIcon dashboard(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(3, 3, 8, 8), 1.5, 1.5);
        p.drawRoundedRect(QRectF(13, 3, 8, 8), 1.5, 1.5);
        p.drawRoundedRect(QRectF(3, 13, 8, 8), 1.5, 1.5);
        p.drawRoundedRect(QRectF(13, 13, 8, 8), 1.5, 1.5);
    });
}

QIcon bell(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath body;
        body.moveTo(5, 17);
        body.cubicTo(5, 9, 7, 4, 12, 4);
        body.cubicTo(17, 4, 19, 9, 19, 17);
        p.drawPath(body);
        p.drawLine(QPointF(4, 17), QPointF(20, 17));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 20.3), 1.5, 1.5);
    });
}

QIcon heart(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath h;
        h.moveTo(12, 19.5);
        h.cubicTo(4, 13, 2, 8.3, 6, 5.2);
        h.cubicTo(9, 3, 12, 6, 12, 8.3);
        h.cubicTo(12, 6, 15, 3, 18, 5.2);
        h.cubicTo(22, 8.3, 20, 13, 12, 19.5);
        h.closeSubpath();
        p.drawPath(h);
    });
}

QIcon video(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(3, 7, 12, 10), 2, 2);
        QPainterPath lens;
        lens.moveTo(15.5, 10);
        lens.lineTo(21, 6.5);
        lens.lineTo(21, 17.5);
        lens.lineTo(15.5, 14);
        lens.closeSubpath();
        p.drawPath(lens);
    });
}

QIcon mapPin(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 9), 5, 5);
        QPainterPath tail;
        tail.moveTo(8.3, 12.7);
        tail.lineTo(12, 21);
        tail.lineTo(15.7, 12.7);
        p.drawPath(tail);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 9), 1.6, 1.6);
    });
}

QIcon route(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(5, 19);
        path.cubicTo(5, 13, 19, 13, 19, 7);
        p.drawPath(path);
        p.setPen(QPen(color, w));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(5, 19), 2.4, 2.4);
        p.drawEllipse(QPointF(19, 7), 2.4, 2.4);
    });
}

QIcon settings(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 12), 4.2, 4.2);
        for (int i = 0; i < 8; ++i) {
            const qreal angle = i * (M_PI / 4.0);
            const QPointF inner(12 + 7.0 * std::cos(angle), 12 + 7.0 * std::sin(angle));
            const QPointF outer(12 + 9.6 * std::cos(angle), 12 + 9.6 * std::sin(angle));
            p.drawLine(inner, outer);
        }
    });
}

QIcon logout(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(4, 4, 9, 16), 1.5, 1.5);
        p.drawLine(QPointF(10, 12), QPointF(21, 12));
        QPainterPath arrow;
        arrow.moveTo(17, 8);
        arrow.lineTo(21, 12);
        arrow.lineTo(17, 16);
        p.drawPath(arrow);
    });
}

QIcon power(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QRectF circleRect(4, 5, 16, 16);
        p.drawArc(circleRect, static_cast<int>((90 + 35) * 16), static_cast<int>((360 - 70) * 16));
        p.drawLine(QPointF(12, 3), QPointF(12, 11));
    });
}

}  // namespace SimpleIcons
