#include "waypoint_canvas.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>

#include "theme/tokens.h"
#include "widgets/camera_live_view.h"
#include "widgets/canvas_geometry.h"

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

    // 우측 상단에 카메라 라이브 화면 자리를 띄운다 — 실제 자식 위젯이라 지도 확대/패닝과
    // 무관하게 화면에 고정되고, 그 영역의 클릭도 자연스럽게 캔버스가 아닌 이 위젯이
    // 받아서 그 밑에 웨이포인트가 실수로 찍히지 않는다.
    cameraView_ = new CameraLiveView(this);
    cameraView_->move(static_cast<int>(floorBounds().right()) - cameraView_->width() - 8,
                       static_cast<int>(floorBounds().top()) + 8);

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

void WaypointCanvas::setCameraFrame(const QImage& frame) { cameraView_->setFrame(frame); }

void WaypointCanvas::setBackgroundImage(const QImage& image) {
    backgroundImage_ = image;
    update();
}

void WaypointCanvas::resetView() {
    zoomScale_ = 1.0;
    panOffset_ = QPointF(0.0, 0.0);
    update();
}

void WaypointCanvas::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);

    cameraView_->move(static_cast<int>(floorBounds().right()) - cameraView_->width() - 8,
                       static_cast<int>(floorBounds().top()) + 8);

    // showEvent에서 설계 기준 맞춤(hasFitInitialView_)이 끝나기 전의 리사이즈는 무시한다
    // — 그 전에 "이전 크기 비율 보존" 로직을 적용하면 화면에 보이기도 전의 의미 없는
    // 임시 크기를 기준으로 좌표가 어긋난다.
    if (!hasFitInitialView_) return;

    const QSize oldSize = event->oldSize();
    if (oldSize.width() <= 0 || oldSize.height() <= 0 || oldSize == event->size()) return;

    // 창 크기 변경 전/후 그리기 영역을 각각 계산해, 웨이포인트·로봇 위치·테스트 주행
    // 경로를 지도(또는 목업 바닥) 위 상대 위치를 보존한 채로 옮긴다. 안 하면 캔버스
    // 절대 픽셀 좌표라서 창을 최대화했을 때 배경 지도만 다시 배치되고 마커는 옛 자리에
    // 남아 지도에서 어긋나 보인다.
    const QRectF oldBounds = QRectF(QPointF(0, 0), oldSize).adjusted(22, 16, -22, -16);
    const QRectF newBounds = floorBounds();

    for (Waypoint& w : waypoints_) {
        w.pos = CanvasGeometry::remapOnResize(w.pos, oldBounds, newBounds, backgroundImage_);
    }
    if (hasRobotPos_) {
        currentRobotPos_ = CanvasGeometry::remapOnResize(currentRobotPos_, oldBounds, newBounds, backgroundImage_);
    }
    for (QPointF& p : testRunPath_) {
        p = CanvasGeometry::remapOnResize(p, oldBounds, newBounds, backgroundImage_);
    }

    // 확대/패닝 상태는 리사이즈 전 배치 기준이라 그대로 두면 새 배치와 어긋난다.
    zoomScale_ = 1.0;
    panOffset_ = QPointF(0.0, 0.0);

    emit viewRescaled();
}

void WaypointCanvas::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    if (hasFitInitialView_) return;
    hasFitInitialView_ = true;

    const QRectF newBounds = floorBounds();
    if (newBounds.width() <= 0 || newBounds.height() <= 0) return;

    // 웨이포인트 좌표는 와이어프레임 설계 캔버스(380x330, floorBounds()와 같은 22,16
    // 여백) 기준으로 정의돼 있다. 오른쪽 패널이 고정폭이라 실제 지도 캔버스는 그보다
    // 훨씬 크게 남는데, 그대로 두면 맵·경로가 화면 왼쪽 위 구석에 작게 몰려 보인다.
    // 이 페이지가 처음 화면에 보이는 시점(= 첫 실제 크기가 확정된 시점)에 설계 기준에서
    // 지금 크기로 한 번 리매핑해, 손대기 전부터 경로 전체가 화면을 채우도록 확대해 둔다.
    const QRectF designBounds = QRectF(0, 0, 380, 330).adjusted(22, 16, -22, -16);
    for (Waypoint& w : waypoints_) {
        w.pos = CanvasGeometry::remapOnResize(w.pos, designBounds, newBounds, backgroundImage_);
    }
    if (hasRobotPos_) {
        currentRobotPos_ = CanvasGeometry::remapOnResize(currentRobotPos_, designBounds, newBounds, backgroundImage_);
    }
    for (QPointF& p : testRunPath_) {
        p = CanvasGeometry::remapOnResize(p, designBounds, newBounds, backgroundImage_);
    }

    emit viewRescaled();
    update();
}

QPointF WaypointCanvas::toWorld(QPointF screenPt) const {
    return (screenPt - panOffset_) / zoomScale_;
}

bool WaypointCanvas::computeImagePlacement(QPointF* outTopLeft, QSize* outScaledSize) const {
    return CanvasGeometry::imagePlacement(floorBounds(), backgroundImage_, outTopLeft, outScaledSize);
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
        // 마커를 누른 경우 — 모드와 무관하게 뗄 때까지 추적한다. 실제로 옮기는 동작은
        // mouseMoveEvent에서 "WP 추가" 모드일 때만 허용한다("이동" 모드에서는 드래그를
        // 무시하고, 뗄 때 항상 그 웨이포인트로 주행 요청을 보낸다 — mouseReleaseEvent).
        dragging_ = true;
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
        // "이동" 모드에서는 드래그 자체를 무시한다 — dragMoved_가 계속 false로 남아야
        // mouseReleaseEvent에서 "그냥 클릭"으로 처리되어 주행 요청으로 이어진다.
        if (mode_ == InteractionMode::AddWaypoint) {
            dragMoved_ = true;
            waypoints_[dragIndex_].pos = toWorld(screenPt);
            update();
        }
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

    // 편집 중임을 알리는 것은 캔버스 위 배너(P6PatrolPage)의 몫이다 — 캔버스 테두리는
    // p6_route_edit_schedule_wireframe.html처럼 항상 같은 회색을 유지한다.
    p.setPen(QPen(Tokens::BORDER_STRONG, 1.0));
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
    }
    // 실제 맵 이미지가 없을 때 목업 벽 라인(회색 두 줄)을 그리던 자리 — 요청에 따라 제거.
    // 흰 바닥(위 drawRoundedRect)만 남는다.

    // 경로 선 — 평소엔 저장된 경로 미리보기(민트 점선), 경로 편집 모드에선 지금까지
    // 순서대로 클릭한 웨이포인트를 잇는 선(파랑 점선)을 그린다.
    // p6_route_edit_schedule_wireframe.html의 파란 실시간 경로선에 대응.
    auto drawRoutePath = [&p](const QVector<QPointF>& pts, const QColor& color) {
        if (pts.size() < 2) return;
        QPainterPath path;
        path.moveTo(pts.first());
        for (int i = 1; i < pts.size(); ++i) path.lineTo(pts.at(i));
        p.setPen(QPen(color, 1.5, Qt::DashLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        for (int i = 0; i + 1 < pts.size(); ++i) {
            const QPointF a = pts[i], b = pts[i + 1];
            const QPointF mid = (a + b) / 2.0;
            const qreal ang = std::atan2(b.y() - a.y(), b.x() - a.x());
            QPainterPath arrow;
            arrow.moveTo(mid + QPointF(std::cos(ang) * 5, std::sin(ang) * 5));
            arrow.lineTo(mid + QPointF(std::cos(ang + 2.4) * 5, std::sin(ang + 2.4) * 5));
            arrow.lineTo(mid + QPointF(std::cos(ang - 2.4) * 5, std::sin(ang - 2.4) * 5));
            arrow.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawPath(arrow);
        }
    };

    if (routeEditMode_) {
        QVector<QPointF> editPts;
        for (const QString& id : std::as_const(routeEditSelection_)) {
            if (const Waypoint* w = findWaypoint(id)) editPts.append(w->pos);
        }
        drawRoutePath(editPts, Tokens::INFO.main);
    } else {
        QVector<QPointF> previewPts;
        for (const QString& id : std::as_const(routePreview_)) {
            if (const Waypoint* w = findWaypoint(id)) previewPts.append(w->pos);
        }
        drawRoutePath(previewPts, Tokens::ACCENT);
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
            if (routeEditMode_) {
                // 편집 중엔 경로 순서에 포함할 수 없다는 것(자동 출발·복귀)을 색으로도
                // 드러낸다 — p6_route_edit_schedule_wireframe.html의 회색 C 마커.
                ring = Tokens::BORDER_STRONG;
                fill = Tokens::BG_SUNKEN;
                textColor = Tokens::TEXT_MUTED;
            } else {
                ring = Tokens::ACCENT;
                fill = Tokens::ACCENT_SOFT;
                textColor = Tokens::ACCENT_HOVER;
            }
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
            ? (routeEditMode_ ? QStringLiteral("%1 — 클릭 불가 (자동 출발·복귀)").arg(w.name)
                               : QStringLiteral("%1 (초기 위치)").arg(w.name))
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

    // 편집 모드 안내는 캔버스 위 배너(P6PatrolPage::routeEditBanner_)가 맡는다 — 여기서는
    // 일반 모드(WP 추가/이동)일 때만 캔버스 하단에 조작 힌트를 그린다.
    if (!routeEditMode_) {
        p.setFont(captionFont);
        p.setPen(Tokens::TEXT_MUTED);
        p.drawText(QRectF(bounds.left() + 4, bounds.bottom() - 16, bounds.width() - 8, 16),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   mode_ == InteractionMode::AddWaypoint
                       ? QStringLiteral("맵 클릭 추가 · 드래그 이동 · 클릭하여 편집 · 우클릭 삭제")
                       : QStringLiteral("클릭하여 주행 · 우클릭 삭제"));
    }
}
