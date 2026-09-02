#include "image_zoom_view.h"

#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QPixmap>
#include <QShowEvent>
#include <QWheelEvent>
#include <cmath>

#include "theme/tokens.h"

namespace {
constexpr qreal kMinZoom = 1.0;  // "전체 보기" 배율보다 더 축소하지는 않는다.
constexpr qreal kMaxZoom = 8.0;
constexpr qreal kZoomStepFactor = 1.15;
}  // namespace

ImageZoomView::ImageZoomView(QWidget* parent) : QGraphicsView(parent), pixmapItem_(nullptr) {
    scene_ = new QGraphicsScene(this);
    setScene(scene_);

    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(Tokens::BG_SUNKEN);
}

void ImageZoomView::setImage(const QImage& image) {
    scene_->clear();
    pixmapItem_ = nullptr;
    if (image.isNull()) return;

    pixmapItem_ = scene_->addPixmap(QPixmap::fromImage(image));
    scene_->setSceneRect(pixmapItem_->boundingRect());
    // 새 이미지가 들어오면 배율을 다시 맞춰야 한다. 이 시점엔 다이얼로그가 아직 화면에
    // 보이기 전이라 뷰 크기가 최종값이 아닐 수 있으므로, 우선 지금 크기 기준으로 한 번
    // 맞추고(레이아웃이 이미 끝난 뒤 호출된 경우 이걸로 충분하다) showEvent에서 실제
    // 최종 크기가 확정된 뒤 다시 한 번 확실하게 맞춘다.
    hasFitOnce_ = false;
    fitToView();
}

void ImageZoomView::fitToView() {
    if (!pixmapItem_) return;
    fitInView(pixmapItem_, Qt::KeepAspectRatio);
    fitScale_ = transform().m11();
}

void ImageZoomView::resetZoom() { fitToView(); }

void ImageZoomView::wheelEvent(QWheelEvent* event) {
    if (!pixmapItem_) return;

    const qreal steps = event->angleDelta().y() / 120.0;
    const qreal factor = std::pow(kZoomStepFactor, steps);

    const qreal currentScale = transform().m11();
    const qreal newScale = qBound(fitScale_ * kMinZoom, currentScale * factor, fitScale_ * kMaxZoom);
    const qreal appliedFactor = newScale / currentScale;

    scale(appliedFactor, appliedFactor);
    event->accept();
}

void ImageZoomView::showEvent(QShowEvent* event) {
    QGraphicsView::showEvent(event);
    // 처음 화면에 보일 때(= 뷰 크기가 최종적으로 확정된 시점) 딱 한 번만 "전체 보기"로
    // 맞춘다. 매 resizeEvent마다 다시 맞추면 사용자가 방금 확대한 배율이 플랫폼이 보내는
    // 사소한 리사이즈 하나에도 조용히 원래대로 되돌아가 버린다 — 실제로 겪었던 버그다.
    if (!hasFitOnce_) {
        fitToView();
        hasFitOnce_ = true;
    }
}

void ImageZoomView::mouseDoubleClickEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    fitToView();
}
