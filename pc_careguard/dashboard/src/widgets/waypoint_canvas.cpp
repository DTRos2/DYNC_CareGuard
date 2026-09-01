#include "waypoint_canvas.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>

#include "theme/tokens.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
constexpr qreal kHitRadius = 20.0;
constexpr qreal kMarkerRadius = 15.0;
constexpr int kTestRunTickMs = 30;
constexpr qreal kTestRunSpeed = 0.00035;  // t(0..1) 증가량 per tick, 대략 3~4초 완주
constexpr qreal kMinZoom = 0.5;
constexpr qreal kMaxZoom = 4.0;
constexpr qreal kZoomStepFactor = 1.15;
}  // namespace

WaypointCanvas::WaypointCanvas(QWidget* parent)
    : QFrame(parent),
      routeEditMode_(false),
      mode_(InteractionMode::AddWaypoint),
      dragging_(false),
      dragMoved_(false),
      dragIndex_(-1),
      zoomScale_(1.0),
      panOffset_(0.0, 0.0),
      panning_(false),
      panMoved_(false),
      currentRobotHeadingDeg_(0.0),
      hasRobotPos_(false),
      testRunTimer_(new QTimer(this)),
      testRunT_(-1.0) {
    setObjectName("slamCanvas");
    setMinimumHeight(320);
    setMouseTracking(false);

    connect(testRunTimer_, &QTimer::timeout, this, [this] {
        testRunT_ += kTestRunSpeed * kTestRunTickMs;
        if (testRunT_ >= 1.0) {
            testRunT_ = 1.0;
            updateRobotAlongTestPath();
            testRunT_ = -1.0;
            testRunTimer_->stop();
            update();
            emit testRunFinished();
            return;
        }
        updateRobotAlongTestPath();
        update();
    });
}

void WaypointCanvas::setWaypoints(const QVector<Waypoint>& wps) {
    waypoints_ = wps;
    update();
}

const WaypointCanvas::Waypoint* WaypointCanvas::findWaypoint(const QString& id) const {
    for (const Waypoint& w : waypoints_) {
        if (w.id == id) return &w;
    }
    return nullptr;
}

void WaypointCanvas::setRoutePreview(const QVector<QString>& orderedIds) {
    routePreview_ = orderedIds;
    update();
}

void WaypointCanvas::setRouteEditMode(bool on) {
    routeEditMode_ = on;
    if (!on) routeEditSelection_.clear();
    update();
}

void WaypointCanvas::setRouteEditSelection(const QVector<QString>& orderedIds) {
    routeEditSelection_ = orderedIds;
    update();
}

void WaypointCanvas::setInteractionMode(InteractionMode mode) {
    mode_ = mode;
}

void WaypointCanvas::setCurrentRobotPosition(QPointF canvasPos, qreal headingDeg) {
    currentRobotPos_ = canvasPos;
    currentRobotHeadingDeg_ = headingDeg;
    hasRobotPos_ = true;
    update();
}

void WaypointCanvas::setBackgroundImage(const QImage& image) {
    backgroundImage_ = image;
    update();
}

void WaypointCanvas::resetView() {
    zoomScale_ = 1.0;
    panOffset_ = QPointF(0.0, 0.0);
    update();
}

QPointF WaypointCanvas::toWorld(QPointF screenPt) const {
    return (screenPt - panOffset_) / zoomScale_;
}

bool WaypointCanvas::computeImagePlacement(QPointF* outTopLeft, QSize* outScaledSize) const {
    if (backgroundImage_.isNull()) return false;
    const QRectF bounds = floorBounds();
    const QSize scaled =
        backgroundImage_.size().scaled(bounds.size().toSize(), Qt::KeepAspectRatio);
    if (scaled.isEmpty()) return false;
    *outScaledSize = scaled;
    *outTopLeft = QPointF(bounds.center().x() - scaled.width() / 2.0,
                           bounds.center().y() - scaled.height() / 2.0);
    return true;
}

namespace {
// 0(검정)~255(흰색). ROS map_server 관례상 어둡게 칠해진 픽셀 = 점유(벽/장애물).
constexpr int kOccupiedGrayThreshold = 90;
}  // namespace

bool WaypointCanvas::isOccupiedAt(QPointF canvasPos) const {
    QPointF topLeft;
    QSize scaledSize;
    if (!computeImagePlacement(&topLeft, &scaledSize)) return false;

    const QPointF local = canvasPos - topLeft;
    if (local.x() < 0 || local.y() < 0 || local.x() >= scaledSize.width() ||
        local.y() >= scaledSize.height()) {
        return false;  // 이미지 바깥은 점유 판정하지 않는다.
    }

    const qreal sx = static_cast<qreal>(backgroundImage_.width()) / scaledSize.width();
    const qreal sy = static_cast<qreal>(backgroundImage_.height()) / scaledSize.height();
    const int ix = qBound(0, static_cast<int>(local.x() * sx), backgroundImage_.width() - 1);
    const int iy = qBound(0, static_cast<int>(local.y() * sy), backgroundImage_.height() - 1);

    return qGray(backgroundImage_.pixel(ix, iy)) < kOccupiedGrayThreshold;
}

QPointF WaypointCanvas::nearestFreePoint(QPointF canvasPos) const {
    if (!isOccupiedAt(canvasPos)) return canvasPos;

    // 캔버스 좌표계에서 바깥으로 확장되는 사각 링을 훑어 가장 가까운 자유 공간을 찾는다.
    constexpr int kMaxRadius = 140;
    constexpr int kStep = 6;
    for (int r = kStep; r <= kMaxRadius; r += kStep) {
        for (int dx = -r; dx <= r; dx += kStep) {
            for (int dy = -r; dy <= r; dy += kStep) {
                if (qAbs(dx) != r && qAbs(dy) != r) continue;  // 링 위 점만
                const QPointF candidate = canvasPos + QPointF(dx, dy);
                if (!isOccupiedAt(candidate)) return candidate;
            }
        }
    }
    return canvasPos;  // 못 찾으면 원래 위치 유지 (최선의 노력).
}

void WaypointCanvas::startTestRun(const QVector<QString>& orderedIds, const QString& chargingId,
                                   bool returnToCharging) {
    testRunPath_.clear();
    if (const Waypoint* c = findWaypoint(chargingId)) testRunPath_.append(c->pos);
    for (const QString& id : orderedIds) {
        if (const Waypoint* w = findWaypoint(id)) testRunPath_.append(w->pos);
    }
    if (returnToCharging) {
        if (const Waypoint* c = findWaypoint(chargingId)) testRunPath_.append(c->pos);
    }

    if (testRunPath_.size() < 2) return;
    testRunT_ = 0.0;
    testRunTimer_->start(kTestRunTickMs);
}

void WaypointCanvas::updateRobotAlongTestPath() {
    if (testRunPath_.size() < 2) return;
    const qreal segCount = testRunPath_.size() - 1;
    const qreal scaled = qBound(0.0, testRunT_, 1.0) * segCount;
    int seg = qMin(static_cast<int>(scaled), static_cast<int>(segCount) - 1);
    const qreal localT = scaled - seg;
    const QPointF a = testRunPath_[seg];
    const QPointF b = testRunPath_[seg + 1];
    const QPointF pt = a + (b - a) * localT;
    const qreal headingRad = std::atan2(b.y() - a.y(), b.x() - a.x());
    currentRobotPos_ = pt;
    currentRobotHeadingDeg_ = headingRad * 180.0 / M_PI;
    hasRobotPos_ = true;
}

QRectF WaypointCanvas::floorBounds() const { return rect().adjusted(22, 16, -22, -16); }

int WaypointCanvas::hitTest(const QPointF& pt) const {
    for (int i = 0; i < waypoints_.size(); ++i) {
        const qreal d = std::hypot(pt.x() - waypoints_[i].pos.x(), pt.y() - waypoints_[i].pos.y());
        if (d <= kHitRadius) return i;
    }
    return -1;
}

void WaypointCanvas::mousePressEvent(QMouseEvent* event) {
    const QPointF screenPt = event->position();
    const QPointF pt = toWorld(screenPt);
    const int idx = hitTest(pt);

    if (event->button() == Qt::RightButton) {
        if (idx >= 0) emit waypointDeleteRequested(waypoints_[idx].id);
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    if (routeEditMode_) {
        if (idx >= 0) {
            if (waypoints_[idx].charging) {
                emit chargingClickBlocked();
            } else {
                emit routeWaypointClicked(waypoints_[idx].id);
            }
            return;
        }
        // 빈 곳을 누르면 경로 편집 중에도 지도를 둘러볼 수 있게 패닝한다.
        panning_ = true;
        panMoved_ = false;
        panStartMouse_ = screenPt;
        panStartOffset_ = panOffset_;
        return;
    }

    if (idx >= 0) {
        // 마커를 누른 경우: "WP 추가" 모드에서만 드래그 이동을 허용한다(드래그 없이
        // 클릭만 하면 편집 팝오버). "이동" 모드에서는 드래그가 무시되고, 뗄 때 항상
        // 그 웨이포인트로 주행한다.
        dragging_ = (mode_ == InteractionMode::AddWaypoint);
        dragMoved_ = false;
        dragIndex_ = idx;
        return;
    }

    // 빈 공간을 눌렀다: 드래그하면 패닝, 드래그 없이 떼면(WP 추가 모드에서만) 그
    // 지점에 웨이포인트를 추가한다.
    panning_ = true;
    panMoved_ = false;
    panStartMouse_ = screenPt;
    panStartOffset_ = panOffset_;
}

void WaypointCanvas::mouseMoveEvent(QMouseEvent* event) {
    const QPointF screenPt = event->position();

    if (dragging_ && dragIndex_ >= 0) {
        dragMoved_ = true;
        waypoints_[dragIndex_].pos = toWorld(screenPt);
        update();
        return;
    }

    if (panning_) {
        const QPointF delta = screenPt - panStartMouse_;
        if (QPoint(delta.toPoint()).manhattanLength() > 3) panMoved_ = true;
        panOffset_ = panStartOffset_ + delta;
        update();
    }
}

void WaypointCanvas::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);

    if (dragging_ && dragIndex_ >= 0) {
        const QString id = waypoints_[dragIndex_].id;
        if (dragMoved_) {
            // 벽 위에 놓았으면 가장 가까운 자유 공간으로 스냅해서 확정한다.
            const QPointF freePos = nearestFreePoint(waypoints_[dragIndex_].pos);
            waypoints_[dragIndex_].pos = freePos;
            emit waypointMoved(id, freePos);
        } else if (mode_ == InteractionMode::MoveWaypoint) {
            // "이동" 모드에서 드래그 없이 그냥 클릭 = 그 웨이포인트로 주행.
            emit waypointDriveRequested(id);
        } else {
            emit waypointEditRequested(id);
        }
        dragging_ = false;
        dragMoved_ = false;
        dragIndex_ = -1;
        return;
    }

    if (panning_) {
        if (!panMoved_ && !routeEditMode_ && mode_ == InteractionMode::AddWaypoint) {
            // 드래그 없이 그냥 클릭한 것과 같다 — "WP 추가" 모드면 그 자리에 추가.
            // 벽/장애물(점유 픽셀) 위였으면 가장 가까운 자유 공간으로 스냅한다.
            emit waypointAddRequested(nearestFreePoint(toWorld(panStartMouse_)));
        }
        // "이동" 모드나 경로 편집 모드에서 드래그 없이 빈 곳을 클릭하면 아무 일도 없다.
        panning_ = false;
        panMoved_ = false;
    }
}

void WaypointCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (hitTest(toWorld(event->position())) >= 0) return;  // 마커 위 더블클릭은 무시
    resetView();
}

void WaypointCanvas::wheelEvent(QWheelEvent* event) {
    const QPointF screenPt = event->position();
    const QPointF worldBefore = toWorld(screenPt);

    const qreal steps = event->angleDelta().y() / 120.0;
    const qreal newZoom = qBound(kMinZoom, zoomScale_ * std::pow(kZoomStepFactor, steps), kMaxZoom);
    zoomScale_ = newZoom;

    // 커서 아래의 월드 지점이 화면상 같은 자리에 남도록 팬 오프셋을 재계산한다.
    panOffset_ = screenPt - worldBefore * zoomScale_;
    update();
    event->accept();
}

void WaypointCanvas::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = floorBounds();

    p.setPen(QPen(routeEditMode_ ? Tokens::INFO.main : Tokens::BORDER_STRONG, routeEditMode_ ? 1.5 : 1.0));
    p.setBrush(Tokens::BG_SURFACE);
    p.drawRoundedRect(bounds, 8, 8);

    // 아래 맵 콘텐츠(배경 이미지·경로·웨이포인트·로봇 위치)만 휠 확대/드래그 패닝의
    // 대상이다. 카드 테두리와 하단 안내 문구는 항상 고정 크기로 남긴다.
    p.save();
    p.setClipRect(bounds);
    p.translate(panOffset_);
    p.scale(zoomScale_, zoomScale_);

    QPointF imageTopLeft;
    QSize imageScaledSize;
    if (computeImagePlacement(&imageTopLeft, &imageScaledSize)) {
        // P5에서 파일로 등록한 실제 pgm 맵. computeImagePlacement()가 캔버스↔이미지 좌표
        // 변환 기준을 정하므로, isOccupiedAt()의 판정과 항상 같은 자리에 그려진다.
        const QImage scaled =
            backgroundImage_.scaled(imageScaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p.drawImage(imageTopLeft, scaled);
    } else {
        // 목업 벽 라인 (L자형 방 구조) — 실제 맵이 없을 때만 그린다.
        p.setPen(QPen(Tokens::TEXT_MUTED, 1.0));
        p.drawLine(QPointF(bounds.left() + bounds.width() * 0.42, bounds.top()),
                   QPointF(bounds.left() + bounds.width() * 0.42, bounds.top() + bounds.height() * 0.34));
        p.drawLine(QPointF(bounds.left() + bounds.width() * 0.68, bounds.bottom()),
                   QPointF(bounds.left() + bounds.width() * 0.68, bounds.top() + bounds.height() * 0.58));
    }

    // 경로 미리보기 (점선 + 화살표)
    QVector<QPointF> previewPts;
    for (const QString& id : std::as_const(routePreview_)) {
        if (const Waypoint* w = findWaypoint(id)) previewPts.append(w->pos);
    }
    if (previewPts.size() > 1) {
        QPainterPath path;
        path.moveTo(previewPts.first());
        for (int i = 1; i < previewPts.size(); ++i) path.lineTo(previewPts.at(i));
        p.setPen(QPen(Tokens::ACCENT, 1.5, Qt::DashLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        for (int i = 0; i + 1 < previewPts.size(); ++i) {
            const QPointF a = previewPts[i], b = previewPts[i + 1];
            const QPointF mid = (a + b) / 2.0;
            const qreal ang = std::atan2(b.y() - a.y(), b.x() - a.x());
            QPainterPath arrow;
            arrow.moveTo(mid + QPointF(std::cos(ang) * 5, std::sin(ang) * 5));
            arrow.lineTo(mid + QPointF(std::cos(ang + 2.4) * 5, std::sin(ang + 2.4) * 5));
            arrow.lineTo(mid + QPointF(std::cos(ang - 2.4) * 5, std::sin(ang - 2.4) * 5));
            arrow.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(Tokens::ACCENT);
            p.drawPath(arrow);
        }
    }

    // 웨이포인트 마커
    QFont labelFont = p.font();
    labelFont.setPixelSize(11);
    labelFont.setBold(true);
    QFont captionFont = p.font();
    captionFont.setPixelSize(10);

    for (const Waypoint& w : std::as_const(waypoints_)) {
        const int editOrder = routeEditMode_ ? routeEditSelection_.indexOf(w.id) : -1;
        const bool selected = editOrder >= 0;

        QColor ring = Tokens::TEXT_SECONDARY;
        QColor fill = Tokens::BG_SURFACE;
        QColor textColor = Tokens::TEXT_PRIMARY;

        if (w.charging) {
            ring = Tokens::ACCENT;
            fill = Tokens::ACCENT_SOFT;
            textColor = Tokens::ACCENT_HOVER;
        } else if (routeEditMode_) {
            if (selected) {
                ring = Tokens::INFO.main;
                fill = Tokens::INFO.main;
                textColor = Tokens::BG_SURFACE;
            } else {
                ring = Tokens::BORDER_STRONG;
                fill = Tokens::BG_SURFACE;
                textColor = Tokens::TEXT_MUTED;
            }
        } else if (routePreview_.contains(w.id)) {
            ring = Tokens::ACCENT;
            fill = Tokens::BG_SURFACE;
            textColor = Tokens::ACCENT_HOVER;
        }

        p.setPen(QPen(ring, selected ? 2.5 : 2.0));
        p.setBrush(fill);
        const qreal r = kMarkerRadius;
        p.drawEllipse(w.pos, r, r);

        p.setFont(labelFont);
        p.setPen(textColor);
        const QString marker = w.charging ? QStringLiteral("C")
                                : editOrder >= 0 ? QString::number(editOrder + 1)
                                                  : QString();
        if (!marker.isEmpty()) p.drawText(QRectF(w.pos.x() - r, w.pos.y() - r, r * 2, r * 2), Qt::AlignCenter, marker);

        p.setFont(captionFont);
        p.setPen(routeEditMode_ && !selected && !w.charging ? Tokens::TEXT_MUTED : Tokens::TEXT_SECONDARY);
        const QString caption = w.charging
            ? QStringLiteral("%1 (초기 위치)").arg(w.name)
            : (routeEditMode_ && !selected ? QStringLiteral("%1 — 미선택").arg(w.name) : w.name);
        // 원 바로 아래, 가운데 정렬로 딱 붙여서 그린다 (AlignVCenter+큰 박스 조합은
        // 여백이 생겨 원과 이름이 멀어 보였다).
        p.drawText(QRectF(w.pos.x() - 75, w.pos.y() + r + 2, 150, 14), Qt::AlignHCenter | Qt::AlignTop, caption);
    }

    // 로봇 현재 위치 (지속 표시 — 테스트 실행 중에는 경로를 따라 움직인다)
    if (hasRobotPos_) {
        QColor halo = Tokens::ACCENT;
        halo.setAlphaF(0.2);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(currentRobotPos_, 15, 15);
        p.setBrush(Tokens::ACCENT);
        p.drawEllipse(currentRobotPos_, 8, 8);
        const qreal headingRad = currentRobotHeadingDeg_ * M_PI / 180.0;
        const QPointF tip(currentRobotPos_.x() + std::cos(headingRad) * 10,
                           currentRobotPos_.y() + std::sin(headingRad) * 10);
        p.setPen(QPen(Tokens::BG_SURFACE, 2.0));
        p.drawLine(currentRobotPos_, tip);

        p.setFont(captionFont);
        p.setPen(Tokens::ACCENT_HOVER);
        p.drawText(QRectF(currentRobotPos_.x() - 60, currentRobotPos_.y() - 32, 150, 14),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("● 현재 위치"));
    }

    p.restore();  // 확대/패닝 종료 — 이 아래는 항상 고정 크기로 그린다.

    if (routeEditMode_) {
        p.setFont(captionFont);
        p.setPen(Tokens::INFO.main);
        p.drawText(QRectF(bounds.left() + 4, bounds.bottom() - 16, bounds.width() - 8, 16),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("경로 편집 중 — 웨이포인트를 순서대로 클릭하세요 (재클릭 = 제거)"));
    } else {
        p.setFont(captionFont);
        p.setPen(Tokens::TEXT_MUTED);
        p.drawText(QRectF(bounds.left() + 4, bounds.bottom() - 16, bounds.width() - 8, 16),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   mode_ == InteractionMode::AddWaypoint
                       ? QStringLiteral("맵 클릭 추가 · 드래그 이동 · 클릭하여 편집 · 우클릭 삭제")
                       : QStringLiteral("클릭하여 주행 · 우클릭 삭제"));
    }
}
