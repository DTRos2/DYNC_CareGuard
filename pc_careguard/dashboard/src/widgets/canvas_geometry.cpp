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

}  // namespace CanvasGeometry
