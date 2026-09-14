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
constexpr qreal kMinZoom = 0.5;
constexpr qreal kMaxZoom = 4.0;
constexpr qreal kZoomStepFactor = 1.15;
// 서버가 아직 이 맵의 resolution을 안 줬을 때(로컬 데모 맵 등)만 쓰는 폴백 —
// p6_patrol_page.cpp가 예전에 모든 맵에 무조건 쓰던 고정 배율과 같은 값이다.
// 실제 지도 해상도(대개 0.05m/px=20px/m)와 다를 수 있어 정확하지 않다.
constexpr qreal kLegacyPxPerMeter = 40.0;
}  // namespace

WaypointCanvas::WaypointCanvas(QWidget* parent)
    : QFrame(parent),
      routeEditMode_(false),
      dragging_(false),
      dragMoved_(false),
      dragIndex_(-1),
      addAiming_(false),
      zoomScale_(1.0),
      panOffset_(0.0, 0.0),
      panning_(false),
      panMoved_(false),
      currentRobotHeadingDeg_(0.0),
      hasRobotPos_(false) {
    setObjectName("slamCanvas");
    setMinimumHeight(320);
    setMouseTracking(false);

    // 우측 상단에 카메라 라이브 화면 자리를 띄운다 — 실제 자식 위젯이라 지도 확대/패닝과
    // 무관하게 화면에 고정되고, 그 영역의 클릭도 자연스럽게 캔버스가 아닌 이 위젯이
    // 받아서 그 밑에 웨이포인트가 실수로 찍히지 않는다.
    cameraView_ = new CameraLiveView(this);
    cameraView_->move(static_cast<int>(floorBounds().right()) - cameraView_->width() - 8,
                       static_cast<int>(floorBounds().top()) + 8);
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

void WaypointCanvas::setAddWaypointMode(bool on) {
    addWaypointMode_ = on;
    update();
}

void WaypointCanvas::setPoseCaptureMode(bool on) {
    poseCaptureMode_ = on;
    if (!on) poseCapturing_ = false;
    update();
}

void WaypointCanvas::setMapMeta(qreal resolutionMPerPx, QPointF originMeters) {
    mapResolution_ = resolutionMPerPx;
    mapOrigin_ = originMeters;
}

QPointF WaypointCanvas::canvasToMapMeters(QPointF canvasPos) const {
    if (mapResolution_ <= 0.0 || backgroundImage_.isNull()) {
        return QPointF(canvasPos.x() / kLegacyPxPerMeter, canvasPos.y() / kLegacyPxPerMeter);
    }
    return CanvasGeometry::canvasToMeters(canvasPos, mapResolution_, mapOrigin_, floorBounds(), backgroundImage_);
}

QPointF WaypointCanvas::mapMetersToCanvas(QPointF meters) const {
    if (mapResolution_ <= 0.0 || backgroundImage_.isNull()) {
        return QPointF(meters.x() * kLegacyPxPerMeter, meters.y() * kLegacyPxPerMeter);
    }
    return CanvasGeometry::metersToCanvas(meters, mapResolution_, mapOrigin_, floorBounds(), backgroundImage_);
}

void WaypointCanvas::setPlannedPath(const QVector<QPointF>& pathMeters) {
    plannedPathMeters_ = pathMeters;
    update();
}

void WaypointCanvas::setCurrentRobotPosition(QPointF canvasPos, qreal headingDeg) {
    currentRobotPos_ = canvasPos;
    currentRobotHeadingDeg_ = headingDeg;
    hasRobotPos_ = true;
    robotPosTracksChargingStation_ = false;
    update();
}

void WaypointCanvas::setCurrentRobotPositionToChargingStation(QPointF canvasPos) {
    currentRobotPos_ = canvasPos;
    currentRobotHeadingDeg_ = 0.0;
    hasRobotPos_ = true;
    robotPosTracksChargingStation_ = true;
    update();
}

void WaypointCanvas::setCameraFrame(const QImage& frame) { cameraView_->setFrame(frame); }

void WaypointCanvas::clearCameraFrame() { cameraView_->clearFrame(); }

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

    // 서버 map.resolution을 받은(진짜 지도가 있는) 맵은 비율 보존 리매핑
    // (remapOnResize) 대신 다음 paintEvent에서 metersPos로부터 다시 계산하게 한다 —
    // remapOnResize는 "oldBounds가 실제로 이 pos를 계산하는 데 쓰인 크기와 정확히
    // 같다"는 전제가 있어야 정확한데, 첫 표시 직후 레이아웃이 완전히 안정되기 전
    // 여러 번의 resizeEvent가 연달아 올 수 있어(각각의 oldSize가 실제 계산 기준과
    // 어긋날 수 있다) 그 전제가 항상 맞다고 보장할 수 없다 — 실제로 겪은 버그: 3개
    // 웨이포인트 중 일부만 엉뚱한 자리에 남아 보임. metersPos 기반 재계산은 이력에
    // 의존하지 않아 언제 리사이즈가 몇 번 일어나든 항상 정확하다.
    if (mapResolution_ > 0.0) {
        requestMetersResync();
        // 확대/패닝 상태는 리사이즈 전 배치 기준이라 그대로 두면 새 배치와 어긋난다
        // (아래 로컬 데모 경로와 같은 이유). viewRescaled()는 여기서 안 낸다 —
        // showEvent와 같은 이유(paintEvent가 실제 보정을 끝낸 직후에만 emit한다).
        zoomScale_ = 1.0;
        panOffset_ = QPointF(0.0, 0.0);
        update();
        return;
    }

    // 창 크기 변경 전/후 그리기 영역을 각각 계산해, 웨이포인트·로봇 위치를 지도(또는
    // 목업 바닥) 위 상대 위치를 보존한 채로 옮긴다. 안 하면 캔버스 절대 픽셀 좌표라서
    // 창을 최대화했을 때 배경 지도만 다시 배치되고 마커는 옛 자리에 남아 지도에서
    // 어긋나 보인다.
    const QRectF oldBounds = QRectF(QPointF(0, 0), oldSize).adjusted(22, 16, -22, -16);
    const QRectF newBounds = floorBounds();

    for (Waypoint& w : waypoints_) {
        w.pos = CanvasGeometry::remapOnResize(w.pos, oldBounds, newBounds, backgroundImage_);
    }
    if (hasRobotPos_) {
        currentRobotPos_ = CanvasGeometry::remapOnResize(currentRobotPos_, oldBounds, newBounds, backgroundImage_);
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

    // 서버 map.resolution을 받은(진짜 지도가 있는) 맵은 좌표 보정을 여기서 하지
    // 않는다 — requestMetersResync()/paintEvent가 "실제로 그려지는 시점"마다
    // metersPos로부터 pos를 다시 계산해주므로 더 안전하다(이 함수는 이 위젯
    // 인스턴스 생애 동안 딱 한 번만 불려서, 이미 한 번 보여준 뒤 hasFitInitialView_가
    // true인 상태로 새 맵 데이터가 들어오면 여기서는 다시 못 잡는다 — 실제로 겪은
    // 버그). 이 경우는 아래 와이어프레임 설계 기준 리매핑도 건너뛴다.
    //
    // viewRescaled()도 여기서 안 낸다 — P6PatrolPage의 핸들러가 그 신호를 받으면
    // canvas_->waypoints()를 그대로 읽어가는데(waypoints_ = canvas_->waypoints()),
    // 이 시점엔 아직 실제 보정(paintEvent의 resync)이 안 끝나 있어서 그대로 읽으면
    // 또 틀린 좌표를 페이지 쪽에 옮겨 담아버린다(실제로 겪은 버그: 캔버스 자체는
    // 다음 그릴 때 맞게 그려지는데, 오른쪽 웨이포인트 목록 패널은 계속 틀린 값을
    // 보여줌). paintEvent가 실제 보정을 끝낸 직후에 딱 한 번 emit한다.
    if (mapResolution_ > 0.0) {
        return;
    }

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

// 예전엔 (22, 16)이었다 — 바깥 카드(buildCanvasCard())가 흰 배경+테두리를 두르고
// 있을 때는 자연스러운 여백이었지만, SlamMapView(P5)와 같은 이유로 그 카드
// 테두리를 없앤 뒤로는 위젯 크기와 무관한 고정 회색 여백만 남아 "박스를 키워도
// 그대로"로 보였다. SlamMapView::exploredBounds()와 같은 값(8px)으로 맞춘다.
QRectF WaypointCanvas::floorBounds() const { return rect().adjusted(8, 8, -8, -8); }

int WaypointCanvas::hitTest(const QPointF& pt) const {
    // 마커 화면 크기가 zoomScale_와 무관하게 고정되도록 바꿨으니(paintEvent), 클릭
    // 판정 반경도 같은 비율(1/zoomScale_)로 맞춰야 "보이는 크기"와 "눌리는 범위"가
    // 확대·축소 어느 배율에서나 일치한다. pt는 이미 월드 좌표(toWorld() 결과)다.
    const qreal hitRadius = kHitRadius / zoomScale_;
    for (int i = 0; i < waypoints_.size(); ++i) {
        const qreal d = std::hypot(pt.x() - waypoints_[i].pos.x(), pt.y() - waypoints_[i].pos.y());
        if (d <= hitRadius) return i;
    }
    return -1;
}

void WaypointCanvas::mousePressEvent(QMouseEvent* event) {
    const QPointF screenPt = event->position();
    const QPointF pt = toWorld(screenPt);

    if (event->button() == Qt::MiddleButton) {
        // 휠 버튼(가운데 클릭) 드래그 = 항상 패닝 — 모드/상태와 무관하게 최우선으로
        // 처리한다. "WP 추가" 모드는 빈 곳 왼쪽 드래그를 웨이포인트 방향 지정에
        // 쓰게 되면서 그 모드에서 지도를 둘러볼 방법이 없어졌다는 요청으로 추가.
        panning_ = true;
        panMoved_ = false;
        panStartMouse_ = screenPt;
        panStartOffset_ = panOffset_;
        return;
    }

    if (poseCaptureMode_) {
        if (event->button() == Qt::RightButton) {
            // 드래그 시작 전 우클릭 = 취소. 이미 드래그 중이면(눌러서 시작한 뒤
            // 오른쪽 버튼도 같이 누르는 것은 일반적이지 않으므로) 여기서 다루지 않는다.
            poseCaptureMode_ = false;
            poseCapturing_ = false;
            update();
            emit initialPoseCaptureCancelled();
            return;
        }
        if (event->button() != Qt::LeftButton) return;
        poseCapturing_ = true;
        poseCaptureAnchor_ = pt;
        poseCaptureCurrent_ = pt;
        update();
        return;
    }

    const int idx = hitTest(pt);

    if (event->button() == Qt::RightButton) {
        if (idx >= 0) emit waypointDeleteRequested(waypoints_[idx].id);
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    if (routeEditMode_) {
        if (idx >= 0) {
            if (waypoints_[idx].kind == QStringLiteral("charge")) {
                emit chargeClickBlocked();
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
        // 마커를 누른 경우 — 뗄 때까지 추적한다. 실제로 옮기는 동작은 mouseMoveEvent가
        // 허용한다. dragStartPos_는 드래그 방향(headingDeg)을 "시작->끝"으로 계산하는
        // 데 쓴다.
        dragging_ = true;
        dragMoved_ = false;
        dragIndex_ = idx;
        dragStartPos_ = waypoints_[idx].pos;
        return;
    }

    if (addWaypointMode_) {
        // 빈 공간을 눌렀다 — "위치 보정"과 같은 클릭+드래그로 방향까지 한 번에 잡는다
        // (mouseReleaseEvent에서 확정).
        addAiming_ = true;
        addAimAnchor_ = pt;
        addAimCurrent_ = pt;
        return;
    }

    // "추가" 모드가 아닐 때 빈 곳을 누르면 패닝(화면 이동)이다 — 왼쪽 드래그로도
    // 지도를 둘러볼 수 있다(휠 버튼 드래그와 동일한 기능, 굳이 안 눌러도 된다).
    panning_ = true;
    panMoved_ = false;
    panStartMouse_ = screenPt;
    panStartOffset_ = panOffset_;
}

void WaypointCanvas::mouseMoveEvent(QMouseEvent* event) {
    const QPointF screenPt = event->position();

    if (poseCapturing_) {
        poseCaptureCurrent_ = toWorld(screenPt);
        update();
        return;
    }

    if (addAiming_) {
        addAimCurrent_ = toWorld(screenPt);
        update();
        return;
    }

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

    if (poseCapturing_) {
        poseCapturing_ = false;
        poseCaptureMode_ = false;  // 단발성 — 한 번 확정하면 모드에서 빠진다.
        const QPointF delta = poseCaptureCurrent_ - poseCaptureAnchor_;
        // 드래그 거리가 너무 짧으면(거의 클릭만) 방향을 신뢰할 수 없으니 0도로 둔다 —
        // ApiClient::driveToPoint 등 다른 경로도 yaw 기본값을 0으로 두는 것과 동일한 규약.
        const qreal headingDeg = std::hypot(delta.x(), delta.y()) < (5.0 / zoomScale_)
            ? 0.0
            : std::atan2(delta.y(), delta.x()) * 180.0 / M_PI;
        const QPointF anchor = poseCaptureAnchor_;
        update();
        emit initialPoseCaptureFinished(anchor, headingDeg);
        return;
    }

    if (addAiming_) {
        addAiming_ = false;
        const QPointF anchor = nearestFreePoint(addAimAnchor_);
        const QPointF delta = addAimCurrent_ - addAimAnchor_;
        // poseCapturing_과 같은 규칙 — 드래그가 너무 짧으면(거의 클릭만) 방향을
        // 신뢰할 수 없으니 0도로 둔다.
        const qreal headingDeg = std::hypot(delta.x(), delta.y()) < (5.0 / zoomScale_)
            ? 0.0
            : std::atan2(delta.y(), delta.x()) * 180.0 / M_PI;
        update();
        emit waypointAddRequested(anchor, headingDeg);
        return;
    }

    if (dragging_ && dragIndex_ >= 0) {
        const QString id = waypoints_[dragIndex_].id;
        if (dragMoved_) {
            // 벽 위에 놓았으면 가장 가까운 자유 공간으로 스냅해서 확정한다.
            const QPointF freePos = nearestFreePoint(waypoints_[dragIndex_].pos);
            waypoints_[dragIndex_].pos = freePos;
            // 드래그 방향(시작->끝)을 그대로 새 yaw로 쓴다 — "위치 보정"과 같은 발상
            // (드래그 한 번으로 위치+방향을 같이 정한다). 너무 짧은 드래그(미세한
            // 떨림 등)는 방향을 신뢰할 수 없으니 기존 yaw를 그대로 둔다.
            const QPointF delta = freePos - dragStartPos_;
            const qreal headingDeg = std::hypot(delta.x(), delta.y()) < (5.0 / zoomScale_)
                ? waypoints_[dragIndex_].yawDeg
                : std::atan2(delta.y(), delta.x()) * 180.0 / M_PI;
            waypoints_[dragIndex_].yawDeg = headingDeg;
            emit waypointMoved(id, freePos, headingDeg);
        } else {
            // 드래그 없이 그냥 클릭 = 편집 팝오버. 그 팝오버의 "경로 이동" 버튼이
            // 실제 주행 요청을 담당한다(WaypointPopover::driveRequested).
            emit waypointEditRequested(id);
        }
        dragging_ = false;
        dragMoved_ = false;
        dragIndex_ = -1;
        return;
    }

    if (panning_) {
        // 빈 곳 클릭/드래그는 이제 addAiming_이 전담한다 — 여기 panning_ 경로는
        // 경로 편집 모드의 지도 둘러보기(빈 곳 드래그)와 휠 버튼 드래그 용도로만 남는다.
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
    // requestMetersResync()로 예약된 좌표 재계산 — paintEvent는 위젯이 실제로 화면에
    // 그려지는 시점에만 불리므로, floorBounds()가 이 시점엔 항상 유효한 최종 크기다
    // (showEvent가 "언젠가 한 번"만 불린다는 것에 기대는 것보다 훨씬 안전하다 —
    // 이 위젯이 이미 다른 맵으로 한 번 보여진 뒤라 hasFitInitialView_가 이미 true인
    // 상태로 새 맵 데이터가 들어와도, 여기서는 매번 확실하게 잡아준다).
    if (pendingMetersResync_) {
        pendingMetersResync_ = false;
        if (mapResolution_ > 0.0) {
            for (Waypoint& w : waypoints_) {
                w.pos = mapMetersToCanvas(w.metersPos);
            }
            // 로봇이 아직 실시간 위치를 안 보내 충전 스테이션을 임시로 대신 보여주고
            // 있었다면(setCurrentRobotPositionToChargingStation), 그 충전 웨이포인트
            // 자체도 방금 막 고쳤으니 로봇 마커도 그 새 위치로 다시 맞춘다 — 안 하면
            // 웨이포인트는 고쳐지는데 로봇 마커만 옛(틀린) 자리에 그대로 남는다.
            if (robotPosTracksChargingStation_) {
                for (const Waypoint& w : waypoints_) {
                    if (w.kind == QStringLiteral("charge")) {
                        currentRobotPos_ = w.pos;
                        break;
                    }
                }
            }
        }
        // 실제 보정이 끝난 뒤에만 알린다 — P6PatrolPage::waypoints_ = canvas_->waypoints()
        // 로 그대로 옮겨 담는 핸들러가 있어서, 보정 전에 emit하면 그 틀린 값을 페이지
        // 쪽 사본(오른쪽 웨이포인트 목록 패널 등)에 그대로 옮겨버린다.
        emit viewRescaled();
    }

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

    // 순찰(Nav2) 중 로봇이 실제로 따라가는 전역 경로 — rviz2의 "가려는 길" 선과
    // 같다. 저장된 경로 미리보기(위, 점선)와 구분되도록 실선으로 그린다. 매 프레임
    // metersPos 그대로 캔버스 좌표로 바꿔 그린다(포인트가 많아야 수십~수백 개라
    // 비용이 작고, 웨이포인트처럼 별도 재계산 예약이 필요 없다).
    if (mapResolution_ > 0.0 && plannedPathMeters_.size() >= 2) {
        QPainterPath livePath;
        livePath.moveTo(mapMetersToCanvas(plannedPathMeters_.first()));
        for (int i = 1; i < plannedPathMeters_.size(); ++i) {
            livePath.lineTo(mapMetersToCanvas(plannedPathMeters_.at(i)));
        }
        p.setPen(QPen(Tokens::OK.main, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(livePath);
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

        const bool isCharge = w.kind == QStringLiteral("charge");
        const bool isEmergency = w.kind == QStringLiteral("emergency");

        QColor ring = Tokens::TEXT_SECONDARY;
        QColor fill = Tokens::BG_SURFACE;
        QColor textColor = Tokens::TEXT_PRIMARY;

        if (isCharge) {
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
            // emergency도 patrol과 똑같이 경로에 포함할 수 있으므로(요청 사양), 편집
            // 모드에서의 선택/미선택 표시는 kind와 무관하게 patrol과 동일하게 그린다 —
            // 평소(비편집)의 빨간 "긴급 버튼" 색은 아래 else if(isEmergency)에서만 쓴다.
            if (selected) {
                ring = Tokens::INFO.main;
                fill = Tokens::INFO.main;
                textColor = Tokens::BG_SURFACE;
            } else {
                ring = Tokens::BORDER_STRONG;
                fill = Tokens::BG_SURFACE;
                textColor = Tokens::TEXT_MUTED;
            }
        } else if (isEmergency) {
            // charge와 확실히 구분되도록 EMERGENCY 상태색(빨강)을 쓴다.
            ring = Tokens::EMERGENCY.main;
            fill = Tokens::EMERGENCY.soft;
            textColor = Tokens::EMERGENCY.main;
        } else if (routePreview_.contains(w.id)) {
            ring = Tokens::ACCENT;
            fill = Tokens::BG_SURFACE;
            textColor = Tokens::ACCENT_HOVER;
        }

        // 마커는 화면상 크기가 확대/축소와 무관하게 일정해야 한다(예: 지도 핀) — 바깥
        // p.scale(zoomScale_)의 영향을 이 마커 하나에서만 1/zoomScale_로 상쇄한다.
        // 그래서 좌표는 w.pos로 이동해둔 로컬 원점(0,0) 기준으로 그린다.
        p.save();
        p.translate(w.pos);
        p.scale(1.0 / zoomScale_, 1.0 / zoomScale_);

        p.setPen(QPen(ring, selected ? 2.5 : 2.0));
        p.setBrush(fill);
        const qreal r = kMarkerRadius;
        p.drawEllipse(QPointF(0, 0), r, r);

        // 방향(yaw) 표시 — 원 테두리 밖으로 짧은 화살표를 그려 이 웨이포인트가 어느
        // 방향을 보고 도착할지 한눈에 보이게 한다("동그라미만 봐서는 방향을 모르겠다"
        // 는 요청으로 추가). 충전 스테이션도 초기 위치의 방향을 나타내므로 똑같이 그린다.
        {
            const qreal headingRad = w.yawDeg * M_PI / 180.0;
            const QPointF tip(std::cos(headingRad) * (r + 8), std::sin(headingRad) * (r + 8));
            constexpr qreal kWingAngle = 2.6;
            QPainterPath arrow;
            arrow.moveTo(tip);
            arrow.lineTo(QPointF(std::cos(headingRad + kWingAngle), std::sin(headingRad + kWingAngle)) * r);
            arrow.lineTo(QPointF(std::cos(headingRad - kWingAngle), std::sin(headingRad - kWingAngle)) * r);
            arrow.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(ring);
            p.drawPath(arrow);
        }

        p.setFont(labelFont);
        p.setPen(textColor);
        // routeEditMode_에서는 kind와 무관하게(charge 제외) 선택 순번이 최우선 —
        // emergency의 "!"는 편집 모드가 아닐 때만 보인다.
        const QString marker = isCharge ? QStringLiteral("C")
                                : routeEditMode_ ? (editOrder >= 0 ? QString::number(editOrder + 1) : QString())
                                : isEmergency ? QStringLiteral("!")
                                              : QString();
        if (!marker.isEmpty()) p.drawText(QRectF(-r, -r, r * 2, r * 2), Qt::AlignCenter, marker);

        p.setFont(captionFont);
        p.setPen(routeEditMode_ && !selected && !isCharge ? Tokens::TEXT_MUTED : Tokens::TEXT_SECONDARY);
        const QString caption = isCharge
            ? (routeEditMode_ ? QStringLiteral("%1 — 클릭 불가 (자동 출발·복귀)").arg(w.name)
                               : QStringLiteral("%1 (초기 위치)").arg(w.name))
            : routeEditMode_
            ? (selected ? w.name : QStringLiteral("%1 — 미선택").arg(w.name))
            : isEmergency
            ? QStringLiteral("%1 (긴급 버튼)").arg(w.name)
            : w.name;
        // 원 바로 아래, 가운데 정렬로 딱 붙여서 그린다 (AlignVCenter+큰 박스 조합은
        // 여백이 생겨 원과 이름이 멀어 보였다).
        p.drawText(QRectF(-75, r + 2, 150, 14), Qt::AlignHCenter | Qt::AlignTop, caption);
        p.restore();
    }

    // 로봇 현재 위치(다이얼 모양 — halo + 중심점 + 방향 바늘). 이 마커가 곧 "현재 위치"
    // 표시 전부다 — 예전엔 별도로 "● 현재 위치" 텍스트 라벨을 옆에 더 그렸는데, 확대한
    // 상태에서는 라벨의 고정 오프셋(-60,-32)도 함께 확대되어 마커와 멀리 떨어져
    // 보이면서 마치 서로 다른 두 마커처럼 보이는 문제가 있어 라벨은 제거했다(요청).
    // 웨이포인트 마커와 같은 이유로 화면상 크기를 zoomScale_와 무관하게 고정한다.
    if (hasRobotPos_) {
        p.save();
        p.translate(currentRobotPos_);
        p.scale(1.0 / zoomScale_, 1.0 / zoomScale_);

        QColor halo = Tokens::ACCENT;
        halo.setAlphaF(0.2);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(QPointF(0, 0), 15, 15);
        p.setBrush(Tokens::ACCENT);
        p.drawEllipse(QPointF(0, 0), 8, 8);
        const qreal headingRad = currentRobotHeadingDeg_ * M_PI / 180.0;
        const QPointF tip(std::cos(headingRad) * 10, std::sin(headingRad) * 10);
        p.setPen(QPen(Tokens::BG_SURFACE, 2.0));
        p.drawLine(QPointF(0, 0), tip);
        p.restore();
    }

    // 위치 보정 중 드래그 미리보기 — 로봇 다이얼 마커와 구분되도록 경고색(다른
    // 팔레트)을 쓰고, 화살표가 곧 "지금 잡고 있는 방향"임을 분명히 보이도록 halo 없이
    // 두꺼운 화살표만 그린다. 웨이포인트 마커와 같은 이유로 화면상 크기를 고정한다.
    if (poseCapturing_) {
        p.save();
        p.translate(poseCaptureAnchor_);
        p.scale(1.0 / zoomScale_, 1.0 / zoomScale_);

        const QPointF delta = poseCaptureCurrent_ - poseCaptureAnchor_;
        const qreal headingRad = std::hypot(delta.x(), delta.y()) < (5.0 / zoomScale_)
            ? 0.0
            : std::atan2(delta.y(), delta.x());

        p.setPen(Qt::NoPen);
        p.setBrush(Tokens::ATTENTION.main);
        p.drawEllipse(QPointF(0, 0), 8, 8);

        QPainterPath arrow;
        const QPointF tip(std::cos(headingRad) * 24, std::sin(headingRad) * 24);
        const qreal wingAngle = 2.6;
        arrow.moveTo(tip);
        arrow.lineTo(QPointF(std::cos(headingRad + wingAngle) * 10, std::sin(headingRad + wingAngle) * 10));
        arrow.lineTo(QPointF(std::cos(headingRad - wingAngle) * 10, std::sin(headingRad - wingAngle) * 10));
        arrow.closeSubpath();
        p.setPen(QPen(Tokens::ATTENTION.main, 3.0));
        p.drawLine(QPointF(0, 0), tip);
        p.setPen(Qt::NoPen);
        p.drawPath(arrow);
        p.restore();
    }

    // "WP 추가" 드래그 미리보기 — 위치 보정과 같은 그림이지만 액센트 색으로 그려
    // "새 웨이포인트를 놓는 중"임을 구분한다.
    if (addAiming_) {
        p.save();
        p.translate(addAimAnchor_);
        p.scale(1.0 / zoomScale_, 1.0 / zoomScale_);

        const QPointF delta = addAimCurrent_ - addAimAnchor_;
        const qreal headingRad = std::hypot(delta.x(), delta.y()) < (5.0 / zoomScale_)
            ? 0.0
            : std::atan2(delta.y(), delta.x());

        p.setPen(Qt::NoPen);
        p.setBrush(Tokens::ACCENT);
        p.drawEllipse(QPointF(0, 0), 8, 8);

        QPainterPath arrow;
        const QPointF tip(std::cos(headingRad) * 24, std::sin(headingRad) * 24);
        constexpr qreal kWingAngle = 2.6;
        arrow.moveTo(tip);
        arrow.lineTo(QPointF(std::cos(headingRad + kWingAngle) * 10, std::sin(headingRad + kWingAngle) * 10));
        arrow.lineTo(QPointF(std::cos(headingRad - kWingAngle) * 10, std::sin(headingRad - kWingAngle) * 10));
        arrow.closeSubpath();
        p.setPen(QPen(Tokens::ACCENT, 3.0));
        p.drawLine(QPointF(0, 0), tip);
        p.setPen(Qt::NoPen);
        p.drawPath(arrow);
        p.restore();
    }

    p.restore();  // 확대/패닝 종료 — 이 아래는 항상 고정 크기로 그린다.

    // 경로 편집 모드 안내는 캔버스 위 배너(P6PatrolPage::routeEditBanner_)가 맡는다 —
    // 여기서는 그 모드가 아닐 때만 캔버스 하단에 조작 힌트를 그린다.
    if (!routeEditMode_) {
        p.setFont(captionFont);
        p.setPen(Tokens::TEXT_MUTED);
        p.drawText(QRectF(bounds.left() + 4, bounds.bottom() - 16, bounds.width() - 8, 16),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   poseCaptureMode_
                       ? QStringLiteral("로봇의 실제 위치를 클릭한 채 드래그해 방향까지 지정 · 놓으면 확정 · 우클릭 취소")
                   : addWaypointMode_
                       ? QStringLiteral("드래그로 추가(방향 지정) · 마커 드래그로 이동+재조준 · 클릭하여 편집 · 우클릭 삭제 · 휠 버튼 드래그로 화면 이동")
                       : QStringLiteral("마커 드래그로 이동+재조준 · 클릭하여 편집(이동은 편집창의 \"경로 이동\") · 우클릭 삭제 · 드래그로 화면 이동 · 새 웨이포인트는 목록의 \"+ 추가\""));
    }
}
