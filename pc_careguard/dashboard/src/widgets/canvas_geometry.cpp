#include "widgets/canvas_geometry.h"

namespace CanvasGeometry {

bool imagePlacement(const QRectF& bounds, const QImage& image, QPointF* outTopLeft, QSize* outScaledSize) {
    if (image.isNull()) return false;
    const QSize scaled = image.size().scaled(bounds.size().toSize(), Qt::KeepAspectRatio);
    if (scaled.isEmpty()) return false;
    *outScaledSize = scaled;
    *outTopLeft = QPointF(bounds.center().x() - scaled.width() / 2.0,
                           bounds.center().y() - scaled.height() / 2.0);
    return true;
}

QPointF remapOnResize(const QPointF& p, const QRectF& oldBounds, const QRectF& newBounds, const QImage& image) {
    QPointF oldTopLeft, newTopLeft;
    QSize oldScaled, newScaled;
    if (imagePlacement(oldBounds, image, &oldTopLeft, &oldScaled) &&
        imagePlacement(newBounds, image, &newTopLeft, &newScaled) && oldScaled.width() > 0 &&
        oldScaled.height() > 0) {
        const QPointF frac((p.x() - oldTopLeft.x()) / oldScaled.width(),
                            (p.y() - oldTopLeft.y()) / oldScaled.height());
        return QPointF(newTopLeft.x() + frac.x() * newScaled.width(),
                        newTopLeft.y() + frac.y() * newScaled.height());
    }
    if (oldBounds.width() <= 0 || oldBounds.height() <= 0) return p;
    const QPointF frac((p.x() - oldBounds.left()) / oldBounds.width(),
                        (p.y() - oldBounds.top()) / oldBounds.height());
    return QPointF(newBounds.left() + frac.x() * newBounds.width(),
                    newBounds.top() + frac.y() * newBounds.height());
}

QPointF metersToCanvas(const QPointF& meters, qreal resolution, const QPointF& origin, const QRectF& bounds,
                       const QImage& image) {
    QPointF topLeft;
    QSize scaledSize;
    if (resolution <= 0.0 || !imagePlacement(bounds, image, &topLeft, &scaledSize)) return QPointF(0, 0);

    const qreal imgCol = (meters.x() - origin.x()) / resolution;
    const qreal imgRow = image.height() - (meters.y() - origin.y()) / resolution;
    const qreal sx = static_cast<qreal>(scaledSize.width()) / image.width();
    const qreal sy = static_cast<qreal>(scaledSize.height()) / image.height();
    return topLeft + QPointF(imgCol * sx, imgRow * sy);
}

QPointF canvasToMeters(const QPointF& canvasPos, qreal resolution, const QPointF& origin, const QRectF& bounds,
                       const QImage& image) {
    QPointF topLeft;
    QSize scaledSize;
    if (resolution <= 0.0 || !imagePlacement(bounds, image, &topLeft, &scaledSize)) return QPointF(0, 0);

    const QPointF local = canvasPos - topLeft;
    const qreal sx = static_cast<qreal>(image.width()) / scaledSize.width();
    const qreal sy = static_cast<qreal>(image.height()) / scaledSize.height();
    const qreal imgCol = local.x() * sx;
    const qreal imgRow = local.y() * sy;
    return QPointF(origin.x() + imgCol * resolution, origin.y() + (image.height() - imgRow) * resolution);
}

}  // namespace CanvasGeometry
