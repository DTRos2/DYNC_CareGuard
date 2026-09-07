#include "simple_icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
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

QIcon play(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal /*w*/) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF triangle;
        triangle << QPointF(7, 4) << QPointF(20, 12) << QPointF(7, 20);
        p.drawPolygon(triangle);
    });
}

QIcon pause(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal /*w*/) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRect(QRectF(6, 4, 4, 16));
        p.drawRect(QRectF(14, 4, 4, 16));
    });
}

QIcon list(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        for (int row = 0; row < 3; ++row) {
            const qreal y = 6 + row * 6;
            p.drawLine(QPointF(4, y), QPointF(6, y));
            p.drawLine(QPointF(9, y), QPointF(20, y));
        }
    });
}

QIcon close(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(6, 6), QPointF(18, 18));
        p.drawLine(QPointF(18, 6), QPointF(6, 18));
    });
}

QIcon robot(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12, 3), QPointF(12, 6));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 2.4), 1.1, 1.1);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(4, 6, 16, 12), 3, 3);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(9, 12), 1.4, 1.4);
        p.drawEllipse(QPointF(15, 12), 1.4, 1.4);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(1.5, 11), QPointF(4, 11));
        p.drawLine(QPointF(20, 11), QPointF(22.5, 11));
        p.drawLine(QPointF(9, 21), QPointF(9, 18));
        p.drawLine(QPointF(15, 21), QPointF(15, 18));
    });
}

QIcon chevronDown(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath v;
        v.moveTo(5, 9);
        v.lineTo(12, 16);
        v.lineTo(19, 9);
        p.drawPath(v);
    });
}

QIcon chevronRight(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath v;
        v.moveTo(9, 5);
        v.lineTo(16, 12);
        v.lineTo(9, 19);
        p.drawPath(v);
    });
}

QIcon upload(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12, 3.5), QPointF(12, 14));
        QPainterPath head;
        head.moveTo(7.5, 8);
        head.lineTo(12, 3.5);
        head.lineTo(16.5, 8);
        p.drawPath(head);
        p.drawLine(QPointF(4, 17), QPointF(4, 20.5));
        p.drawLine(QPointF(4, 20.5), QPointF(20, 20.5));
        p.drawLine(QPointF(20, 20.5), QPointF(20, 17));
    });
}

QIcon radar(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 12), 9, 9);
        p.drawEllipse(QPointF(12, 12), 4.5, 4.5);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 12), 1.3, 1.3);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(12, 12), QPointF(19, 6.5));
    });
}

namespace {
void drawArrow(QPainter& p, const QColor& color, qreal w, qreal fromX, qreal fromY, qreal toX, qreal toY,
               qreal headDx1, qreal headDy1, qreal headDx2, qreal headDy2) {
    p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(fromX, fromY), QPointF(toX, toY));
    QPainterPath head;
    head.moveTo(toX + headDx1, toY + headDy1);
    head.lineTo(toX, toY);
    head.lineTo(toX + headDx2, toY + headDy2);
    p.drawPath(head);
}
}  // namespace

QIcon arrowUp(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        drawArrow(p, color, w, 12, 20, 12, 4, -4.5, 4.5, 4.5, 4.5);
    });
}

QIcon arrowDown(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        drawArrow(p, color, w, 12, 4, 12, 20, -4.5, -4.5, 4.5, -4.5);
    });
}

QIcon arrowLeft(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        drawArrow(p, color, w, 20, 12, 4, 12, 4.5, -4.5, 4.5, 4.5);
    });
}

QIcon arrowRight(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        drawArrow(p, color, w, 4, 12, 20, 12, -4.5, -4.5, -4.5, 4.5);
    });
}

QIcon deviceFloppy(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath body;
        body.moveTo(4.5, 3.5);
        body.lineTo(16.5, 3.5);
        body.lineTo(20.5, 7.5);
        body.lineTo(20.5, 20.5);
        body.lineTo(4.5, 20.5);
        body.closeSubpath();
        p.drawPath(body);
        p.drawRoundedRect(QRectF(8, 3.5, 8, 6), 0.8, 0.8);
        p.drawRoundedRect(QRectF(7, 13, 10, 7.5), 1, 1);
    });
}

QIcon x(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(6, 6), QPointF(18, 18));
        p.drawLine(QPointF(18, 6), QPointF(6, 18));
    });
}

QIcon trackPrev(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal /*w*/) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRect(QRectF(3, 5, 2, 14));
        QPolygonF triangle;
        triangle << QPointF(19, 5) << QPointF(19, 19) << QPointF(7, 12);
        p.drawPolygon(triangle);
    });
}

QIcon trackNext(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal /*w*/) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRect(QRectF(19, 5, 2, 14));
        QPolygonF triangle;
        triangle << QPointF(5, 5) << QPointF(5, 19) << QPointF(17, 12);
        p.drawPolygon(triangle);
    });
}

QIcon download(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12, 3), QPointF(12, 14));
        QPainterPath arrow;
        arrow.moveTo(7, 10);
        arrow.lineTo(12, 15);
        arrow.lineTo(17, 10);
        p.drawPath(arrow);
        p.drawLine(QPointF(4, 19), QPointF(20, 19));
    });
}

QIcon share(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(6, 12), 3, 3);
        p.drawEllipse(QPointF(18, 5.5), 3, 3);
        p.drawEllipse(QPointF(18, 18.5), 3, 3);
        p.drawLine(QPointF(8.6, 10.6), QPointF(15.4, 7));
        p.drawLine(QPointF(8.6, 13.4), QPointF(15.4, 17));
    });
}

QIcon fileCheck(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath body;
        body.moveTo(6, 2.5);
        body.lineTo(14, 2.5);
        body.lineTo(18, 6.5);
        body.lineTo(18, 21.5);
        body.lineTo(6, 21.5);
        body.closeSubpath();
        p.drawPath(body);
        QPainterPath fold;
        fold.moveTo(14, 2.5);
        fold.lineTo(14, 6.5);
        fold.lineTo(18, 6.5);
        p.drawPath(fold);
        QPainterPath check;
        check.moveTo(8.5, 14.5);
        check.lineTo(11, 17);
        check.lineTo(15.5, 11.5);
        p.drawPath(check);
    });
}

QIcon fileUpload(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath body;
        body.moveTo(6, 2.5);
        body.lineTo(14, 2.5);
        body.lineTo(18, 6.5);
        body.lineTo(18, 21.5);
        body.lineTo(6, 21.5);
        body.closeSubpath();
        p.drawPath(body);
        QPainterPath fold;
        fold.moveTo(14, 2.5);
        fold.lineTo(14, 6.5);
        fold.lineTo(18, 6.5);
        p.drawPath(fold);
        p.drawLine(QPointF(12, 18.5), QPointF(12, 11.5));
        QPainterPath head;
        head.moveTo(9, 14.5);
        head.lineTo(12, 11.5);
        head.lineTo(15, 14.5);
        p.drawPath(head);
    });
}

QIcon alertTriangle(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath tri;
        tri.moveTo(12, 3.5);
        tri.lineTo(21.5, 20.2);
        tri.lineTo(2.5, 20.2);
        tri.closeSubpath();
        p.drawPath(tri);
        p.drawLine(QPointF(12, 9.5), QPointF(12, 14.5));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 17.3), 1.1, 1.1);
    });
}

QIcon infoCircle(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(2.5, 2.5, 19, 19));
        p.drawLine(QPointF(12, 11), QPointF(12, 16.5));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 7.6), 1.1, 1.1);
    });
}

QIcon trash(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(4, 7), QPointF(20, 7));
        p.drawLine(QPointF(9, 7), QPointF(9, 4.5));
        p.drawLine(QPointF(15, 7), QPointF(15, 4.5));
        p.drawLine(QPointF(9, 4.5), QPointF(15, 4.5));
        p.drawRoundedRect(QRectF(6, 7, 12, 13), 1.5, 1.5);
        p.drawLine(QPointF(10, 10.5), QPointF(10, 17));
        p.drawLine(QPointF(14, 10.5), QPointF(14, 17));
    });
}

QIcon expand(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath tl;
        tl.moveTo(3, 9);
        tl.lineTo(3, 3);
        tl.lineTo(9, 3);
        p.drawPath(tl);
        QPainterPath br;
        br.moveTo(15, 21);
        br.lineTo(21, 21);
        br.lineTo(21, 15);
        p.drawPath(br);
    });
}

QIcon collapse(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath tl;
        tl.moveTo(3, 9);
        tl.lineTo(9, 9);
        tl.lineTo(9, 3);
        p.drawPath(tl);
        QPainterPath br;
        br.moveTo(15, 21);
        br.lineTo(15, 15);
        br.lineTo(21, 15);
        p.drawPath(br);
    });
}

QIcon bolt(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal) {
        QPainterPath z;
        z.moveTo(13, 2);
        z.lineTo(3, 14);
        z.lineTo(12, 14);
        z.lineTo(11, 22);
        z.lineTo(21, 10);
        z.lineTo(12, 10);
        z.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(z);
    });
}

QIcon alertOctagon(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath octagon;
        octagon.moveTo(8.5, 3);
        octagon.lineTo(15.5, 3);
        octagon.lineTo(21, 8.5);
        octagon.lineTo(21, 15.5);
        octagon.lineTo(15.5, 21);
        octagon.lineTo(8.5, 21);
        octagon.lineTo(3, 15.5);
        octagon.lineTo(3, 8.5);
        octagon.closeSubpath();
        p.drawPath(octagon);
        p.drawLine(QPointF(12, 7.5), QPointF(12, 13.5));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 16.5), 1.1, 1.1);
    });
}

QIcon chevronLeft(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath v;
        v.moveTo(15, 5);
        v.lineTo(8, 12);
        v.lineTo(15, 19);
        p.drawPath(v);
    });
}

QIcon barChart(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(4, 13, 4, 7), 1, 1);
        p.drawRoundedRect(QRectF(10, 9, 4, 11), 1, 1);
        p.drawRoundedRect(QRectF(16, 5, 4, 15), 1, 1);
    });
}

QIcon file(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath body;
        body.moveTo(6, 2.5);
        body.lineTo(14, 2.5);
        body.lineTo(18, 6.5);
        body.lineTo(18, 21.5);
        body.lineTo(6, 21.5);
        body.closeSubpath();
        p.drawPath(body);
        QPainterPath fold;
        fold.moveTo(14, 2.5);
        fold.lineTo(14, 6.5);
        fold.lineTo(18, 6.5);
        p.drawPath(fold);
        p.drawLine(QPointF(9, 13), QPointF(15, 13));
        p.drawLine(QPointF(9, 17), QPointF(15, 17));
    });
}

QIcon speakerphone(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath body;
        body.moveTo(9, 9);
        body.lineTo(15, 4.5);
        body.lineTo(15, 19.5);
        body.lineTo(9, 15);
        body.closeSubpath();
        p.drawPath(body);
        p.drawRoundedRect(QRectF(3.5, 9, 5.5, 6), 1.2, 1.2);
        p.drawLine(QPointF(6, 15), QPointF(6, 20));
        p.drawArc(QRectF(16, 6.5, 6, 11), -60 * 16, 120 * 16);
    });
}

QIcon phone(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath receiver;
        receiver.moveTo(4.5, 5.5);
        receiver.cubicTo(4.5, 4.3, 5.5, 3.5, 6.8, 3.7);
        receiver.lineTo(9.3, 4.1);
        receiver.cubicTo(10.2, 4.3, 10.6, 5.3, 10.2, 6.1);
        receiver.lineTo(8.8, 8.6);
        receiver.cubicTo(9.6, 10.6, 13.4, 14.4, 15.4, 15.2);
        receiver.lineTo(17.9, 13.8);
        receiver.cubicTo(18.7, 13.4, 19.7, 13.8, 19.9, 14.7);
        receiver.lineTo(20.3, 17.2);
        receiver.cubicTo(20.5, 18.5, 19.7, 19.5, 18.5, 19.5);
        receiver.cubicTo(11.3, 19.5, 4.5, 12.7, 4.5, 5.5);
        receiver.closeSubpath();
        p.drawPath(receiver);
    });
}

QIcon users(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(3.5, 5, 6, 6));
        QPainterPath frontShoulders;
        frontShoulders.moveTo(1.5, 20);
        frontShoulders.cubicTo(1.5, 15.5, 4, 13.3, 6.5, 13.3);
        frontShoulders.cubicTo(9, 13.3, 11.5, 15.5, 11.5, 20);
        p.drawPath(frontShoulders);
        p.drawEllipse(QRectF(13, 4.3, 5.2, 5.2));
        QPainterPath backShoulders;
        backShoulders.moveTo(13.5, 20);
        backShoulders.cubicTo(13.5, 16, 15.7, 13.8, 18, 13.8);
        backShoulders.cubicTo(20.3, 13.8, 22.5, 16, 22.5, 20);
        p.drawPath(backShoulders);
    });
}

QIcon message(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath bubble;
        bubble.moveTo(4, 5.5);
        bubble.lineTo(20, 5.5);
        bubble.lineTo(20, 15.5);
        bubble.lineTo(9.5, 15.5);
        bubble.lineTo(6, 19.5);
        bubble.lineTo(6, 15.5);
        bubble.lineTo(4, 15.5);
        bubble.closeSubpath();
        p.drawPath(bubble);
        p.drawLine(QPointF(7.5, 9.5), QPointF(16.5, 9.5));
        p.drawLine(QPointF(7.5, 12.5), QPointF(14, 12.5));
    });
}

QIcon phoneCall(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath receiver;
        receiver.moveTo(4.5, 6.5);
        receiver.cubicTo(4.5, 5.3, 5.5, 4.5, 6.8, 4.7);
        receiver.lineTo(9.3, 5.1);
        receiver.cubicTo(10.2, 5.3, 10.6, 6.3, 10.2, 7.1);
        receiver.lineTo(8.8, 9.6);
        receiver.cubicTo(9.6, 11.6, 12.4, 14.4, 14.4, 15.2);
        receiver.lineTo(16.9, 13.8);
        receiver.cubicTo(17.7, 13.4, 18.7, 13.8, 18.9, 14.7);
        receiver.lineTo(19.3, 17.2);
        receiver.cubicTo(19.5, 18.5, 18.7, 19.5, 17.5, 19.5);
        receiver.cubicTo(10.3, 19.5, 4.5, 13.7, 4.5, 6.5);
        receiver.closeSubpath();
        p.drawPath(receiver);
        p.drawLine(QPointF(15, 3.5), QPointF(20.5, 9));
        p.drawLine(QPointF(20.5, 3.5), QPointF(15, 9));
    });
}

QIcon plus(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(12, 4.5), QPointF(12, 19.5));
        p.drawLine(QPointF(4.5, 12), QPointF(19.5, 12));
    });
}

QIcon edit(const QColor& color, int px) {
    return makeIcon(color, px, [color](QPainter& p, qreal w) {
        p.setPen(QPen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath pencil;
        pencil.moveTo(4, 20);
        pencil.lineTo(5.2, 15.3);
        pencil.lineTo(15, 5.5);
        pencil.lineTo(18.5, 9);
        pencil.lineTo(8.7, 18.8);
        pencil.closeSubpath();
        p.drawPath(pencil);
        p.drawLine(QPointF(4, 20), QPointF(8.7, 18.8));
        p.drawLine(QPointF(12.5, 8), QPointF(16, 11.5));
    });
}

}  // namespace SimpleIcons
