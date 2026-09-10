#include "slam_mapping_card.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "theme/tokens.h"
#include "widgets/camera_live_view.h"

namespace {
constexpr qreal kMinZoom = 0.5;
constexpr qreal kMaxZoom = 4.0;
constexpr qreal kZoomStepFactor = 1.15;

// rviz2 기본 팔레트를 흉내낸 색상 — 정확히 같지는 않지만 "빨간 화살표 + 알록달록한
// 스캔 점" 느낌을 재현하는 게 목적이라 근사치로 충분하다.
const QColor kPoseArrowColor(QStringLiteral("#E53935"));

QColor scanPointColor(qreal distMeters) {
    constexpr qreal kMaxDist = 6.0;  // 이 거리 이상은 같은 색(보라)으로 뭉친다
    const qreal t = std::clamp(distMeters / kMaxDist, 0.0, 1.0);
    // 가까움(빨강, hue 0) -> 멀어질수록 초록/파랑/보라(hue 270) 순서로 이동.
    // (예전엔 (1-t)*270이라 실제로는 반대로 계산되어 가까운 점이 파란색으로 나왔었다.)
    const int hue = static_cast<int>(t * 270.0);
    return QColor::fromHsv(hue, 220, 255);
}
}  // namespace

// ============================================================
// SlamMapView
// ============================================================
SlamMapView::SlamMapView(QWidget* parent) : QFrame(parent) {
    setObjectName("slamCanvas");
    setMinimumHeight(320);

    // 우측 상단에 카메라 라이브 화면 자리를 띄운다 — 자식 위젯이라 아래 paintEvent의
    // 확대/패닝과 무관하게 항상 같은 화면 위치에 고정된다.
    cameraView_ = new CameraLiveView(this);
    cameraView_->move(static_cast<int>(exploredBounds().right()) - cameraView_->width() - 8,
                       static_cast<int>(exploredBounds().top()) + 8);
}

void SlamMapView::resetMapping() {
    liveTrajectoryMeters_.clear();
    liveScanMeters_.clear();
    hasLivePose_ = false;
    backgroundImage_ = QImage();
    mapResolution_ = 0.0;
    mapPixelSize_ = QSize();
    update();
}

void SlamMapView::setBackgroundImage(const QImage& image) {
    backgroundImage_ = image;
    update();
}

void SlamMapView::setMapMeta(qreal resolutionMPerPx, const QPointF& originMeters, const QSize& imagePixelSize) {
    mapResolution_ = resolutionMPerPx;
    mapOriginMeters_ = originMeters;
    mapPixelSize_ = imagePixelSize;
}

void SlamMapView::setLivePose(qreal xMeters, qreal yMeters, qreal yawRad) {
    const QPointF newPos(xMeters, yMeters);
    if (hasLivePose_) {
        const qreal moved = std::hypot(newPos.x() - livePoseMeters_.x(), newPos.y() - livePoseMeters_.y());
        // 몇 mm 단위로 계속 찍으면 궤적 점선이 지저분해지니, 일정 거리 이상 움직였을
        // 때만 궤적에 추가한다.
        if (moved > 0.05) {
            liveTrajectoryMeters_.append(newPos);
            if (liveTrajectoryMeters_.size() > 2000) liveTrajectoryMeters_.removeFirst();
        }
    } else {
        liveTrajectoryMeters_.append(newPos);
    }
    hasLivePose_ = true;
    livePoseMeters_ = newPos;
    livePoseYaw_ = yawRad;
    update();
}

void SlamMapView::setCameraFrame(const QImage& frame) { cameraView_->setFrame(frame); }

void SlamMapView::setLiveScan(const QVector<QPointF>& pointsMeters) {
    liveScanMeters_ = pointsMeters;
    update();
}

QRectF SlamMapView::exploredBounds() const {
    const QRectF r = rect();
    // 예전엔 28px였다 — 바깥 카드(SlamMappingCard)가 흰 배경+테두리(QFrame#card)를
    // 두르고 있을 때는 이 안쪽 여백이 "카드 안의 여백"처럼 자연스러웠지만, 카드가
    // 자기 테두리를 없애고(위 SlamMappingCard 생성자 참고) 이 캔버스 자체가 유일한
    // 박스가 된 뒤로는 28px가 위젯 크기와 무관하게 고정된 회색 여백으로만 보여
    // "박스를 키워도 그대로"인 것처럼 보이는 원인이었다. 격자 캡션 문구·카메라
    // 오버레이가 겨우 들어갈 만큼만 남기고 최소화한다.
    const qreal margin = 8;
    return r.adjusted(margin, margin, -margin, -margin);
}

std::optional<QPointF> SlamMapView::mapMetersToCanvas(const QPointF& meters) const {
    if (mapResolution_ <= 0.0 || mapPixelSize_.isEmpty()) return std::nullopt;

    // meters -> 이미지 픽셀 좌표. uplink_node.py가 PGM으로 인코딩할 때 이미
    // np.flipud로 위아래를 뒤집어(OccupancyGrid는 좌하단 원점, 이미지는 좌상단 원점)
    // 저장해뒀으므로, 여기서도 세로를 한 번 더 뒤집어야 이미지 위 실제 자리와 맞는다.
    const qreal imgCol = (meters.x() - mapOriginMeters_.x()) / mapResolution_;
    const qreal imgRow = mapPixelSize_.height() - (meters.y() - mapOriginMeters_.y()) / mapResolution_;

    const QRectF bounds = exploredBounds();
    if (bounds.width() <= 0 || bounds.height() <= 0) return std::nullopt;

    // paintEvent가 backgroundImage_를 bounds에 KeepAspectRatio로 맞출 때와 같은 배율 —
    // 이 계산이 어긋나면 화살표·스캔점이 지도 위 엉뚱한 자리에 그려진다.
    const qreal scale =
        std::min(bounds.width() / mapPixelSize_.width(), bounds.height() / mapPixelSize_.height());
    const qreal scaledW = mapPixelSize_.width() * scale;
    const qreal scaledH = mapPixelSize_.height() * scale;
    const QPointF topLeft(bounds.center().x() - scaledW / 2.0, bounds.center().y() - scaledH / 2.0);

    return topLeft + QPointF(imgCol, imgRow) * scale;
}

void SlamMapView::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);

    cameraView_->move(static_cast<int>(exploredBounds().right()) - cameraView_->width() - 8,
                       static_cast<int>(exploredBounds().top()) + 8);

    // 로봇 위치·스캔점·궤적은 전부 미터(map 프레임) 좌표로 갖고 있다가 매 paint마다
    // 현재 위젯 크기 기준으로 다시 배치하므로(mapMetersToCanvas), 리사이즈 때 좌표
    // 자체를 옮겨줄 필요는 없다 — 확대/패닝 상태만 새 배치 기준으로 초기화한다
    // (WaypointCanvas::resizeEvent와 같은 이유).
    if (event->oldSize() == event->size()) return;
    zoomScale_ = 1.0;
    panOffset_ = QPointF(0.0, 0.0);
}

void SlamMapView::wheelEvent(QWheelEvent* event) {
    const QPointF screenPt = event->position();
    const QPointF worldBefore = (screenPt - panOffset_) / zoomScale_;

    const qreal steps = event->angleDelta().y() / 120.0;
    zoomScale_ = qBound(kMinZoom, zoomScale_ * std::pow(kZoomStepFactor, steps), kMaxZoom);

    // 커서 아래의 지도 지점이 화면상 같은 자리에 남도록 팬 오프셋을 재계산한다.
    panOffset_ = screenPt - worldBefore * zoomScale_;
    update();
    event->accept();
}

void SlamMapView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    panning_ = true;
    panStartMouse_ = event->position();
    panStartOffset_ = panOffset_;
}

void SlamMapView::mouseMoveEvent(QMouseEvent* event) {
    if (!panning_) return;
    panOffset_ = panStartOffset_ + (event->position() - panStartMouse_);
    update();
}

void SlamMapView::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    panning_ = false;
}

void SlamMapView::mouseDoubleClickEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    zoomScale_ = 1.0;
    panOffset_ = QPointF(0.0, 0.0);
    update();
}

void SlamMapView::drawGrid(QPainter& p, const QRectF& bounds) const {
    // rviz의 Grid 디스플레이 흉내 — 맵 메타가 아직 없으면(SLAM 시작 전) 고정 논리
    // 픽셀 간격으로, 있으면 실제 1m 간격으로 원점에 맞춰 그린다.
    qreal spacing = 40.0;
    QPointF originPx = bounds.topLeft();

    if (mapResolution_ > 0.0 && !mapPixelSize_.isEmpty()) {
        const qreal scale =
            std::min(bounds.width() / mapPixelSize_.width(), bounds.height() / mapPixelSize_.height());
        spacing = (1.0 / mapResolution_) * scale;
        if (const auto originCanvas = mapMetersToCanvas(QPointF(0.0, 0.0))) {
            originPx = *originCanvas;
        }
    }
    if (spacing < 4.0) return;  // 과도하게 촘촘하면(축소 상태) 그리지 않는다

    QColor gridColor = Tokens::TEXT_MUTED;
    gridColor.setAlpha(50);
    p.setPen(QPen(gridColor, 0.6));

    const qreal kMin = std::floor((bounds.left() - originPx.x()) / spacing);
    for (qreal x = originPx.x() + kMin * spacing; x <= bounds.right() + 1.0; x += spacing) {
        if (x < bounds.left() - 1.0) continue;
        p.drawLine(QPointF(x, bounds.top()), QPointF(x, bounds.bottom()));
    }
    const qreal jMin = std::floor((bounds.top() - originPx.y()) / spacing);
    for (qreal y = originPx.y() + jMin * spacing; y <= bounds.bottom() + 1.0; y += spacing) {
        if (y < bounds.top() - 1.0) continue;
        p.drawLine(QPointF(bounds.left(), y), QPointF(bounds.right(), y));
    }
}

void SlamMapView::drawLiveTrajectory(QPainter& p) const {
    if (liveTrajectoryMeters_.size() < 2) return;

    QPainterPath path;
    bool started = false;
    for (const QPointF& meters : liveTrajectoryMeters_) {
        const auto canvasPt = mapMetersToCanvas(meters);
        if (!canvasPt) continue;
        if (!started) {
            path.moveTo(*canvasPt);
            started = true;
        } else {
            path.lineTo(*canvasPt);
        }
    }
    if (!started) return;

    p.setPen(QPen(Tokens::ACCENT, 1.5, Qt::DashLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void SlamMapView::drawLiveScan(QPainter& p) const {
    if (liveScanMeters_.isEmpty()) return;
    const QPointF refPoint = hasLivePose_ ? livePoseMeters_ : liveScanMeters_.first();

    p.setPen(Qt::NoPen);
    for (const QPointF& meters : liveScanMeters_) {
        const auto canvasPt = mapMetersToCanvas(meters);
        if (!canvasPt) continue;
        const qreal dist = std::hypot(meters.x() - refPoint.x(), meters.y() - refPoint.y());
        p.setBrush(scanPointColor(dist));
        p.drawEllipse(*canvasPt, 2.0, 2.0);
    }
}

void SlamMapView::drawLivePose(QPainter& p) const {
    if (!hasLivePose_) return;
    const auto canvasPt = mapMetersToCanvas(livePoseMeters_);
    if (!canvasPt) return;
    const QPointF pos = *canvasPt;

    // map 프레임(x: 오른쪽+, y: 위+, yaw: 반시계+)을 이미지 좌표(y 아래+)로 옮기는
    // 중이라 mapMetersToCanvas가 세로를 뒤집어 배치한 것과 같은 이유로, 화살표 방향도
    // y 성분의 부호를 반대로 써야 실제 로봇이 보는 방향과 화면이 일치한다.
    const qreal dx = std::cos(livePoseYaw_);
    const qreal dy = -std::sin(livePoseYaw_);
    const QPointF shaftDir(dx, dy);
    const QPointF normal(-dy, dx);

    constexpr qreal kArrowLen = 26.0;
    constexpr qreal kHeadLen = 9.0;
    constexpr qreal kHeadWidth = 8.0;
    const QPointF tip = pos + shaftDir * kArrowLen;

    p.setPen(QPen(kPoseArrowColor, 3.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(pos, tip - shaftDir * kHeadLen * 0.6);

    QPainterPath head;
    head.moveTo(tip);
    head.lineTo(tip - shaftDir * kHeadLen + normal * kHeadWidth / 2.0);
    head.lineTo(tip - shaftDir * kHeadLen - normal * kHeadWidth / 2.0);
    head.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(kPoseArrowColor);
    p.drawPath(head);

    // 로봇 몸체 — rviz의 로봇 footprint 마커를 단순한 원판으로 대신한다.
    p.setBrush(Tokens::TEXT_PRIMARY);
    p.setPen(QPen(Qt::white, 1.0));
    p.drawEllipse(pos, 5.0, 5.0);
}

void SlamMapView::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = exploredBounds();

    // 테두리(흰 바닥)는 항상 고정 크기로 그린다 — 확대/패닝 대상은 그 안쪽 내용물뿐이다.
    p.setPen(QPen(Tokens::TEXT_SECONDARY, 1.5));
    p.setBrush(Tokens::BG_SURFACE);
    p.drawRoundedRect(bounds, 6, 6);

    p.save();
    p.setClipRect(bounds);
    p.translate(panOffset_);
    p.scale(zoomScale_, zoomScale_);

    // rviz의 Grid 디스플레이 흉내 — 지도보다 먼저 깔아 SLAM 시작 전에도 바닥 격자가
    // 항상 보이게 한다.
    drawGrid(p, bounds);

    if (!backgroundImage_.isNull()) {
        // 실시간 SLAM 미리보기든, "파일로 등록"한 실제 맵이든 같은 경로로 그린다.
        // PgmImageLoader가 만드는 이미지는 Format_Grayscale8인데, 이 포맷을 그대로
        // scaled(SmoothTransformation)+drawImage 하면 이 Qt 빌드에서 전부 검게 나온다
        // (실제 로봇 지도가 하얀/검은 화면으로만 보이던 원인). RGB32로 바꾼 뒤 스케일링하면
        // 정상적으로 그레이스케일 그대로 그려진다.
        const QImage rgb = backgroundImage_.convertToFormat(QImage::Format_RGB32);
        const QImage scaled = rgb.scaled(bounds.size().toSize(), Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation);
        const QPointF topLeft(bounds.center().x() - scaled.width() / 2.0,
                               bounds.center().y() - scaled.height() / 2.0);
        p.drawImage(topLeft, scaled);
    }

    // rviz2처럼: 지도 위에 궤적 -> 라이다 스캔(색상 점) -> 로봇 화살표 순서로 겹쳐 그린다.
    drawLiveTrajectory(p);
    drawLiveScan(p);
    drawLivePose(p);

    p.restore();  // 확대/패닝 종료 — 이 아래는 항상 고정 크기로 그린다.

    p.setPen(Tokens::TEXT_MUTED);
    QFont captionFont = p.font();
    captionFont.setPixelSize(9);
    p.setFont(captionFont);
    p.drawText(QRectF(bounds.left() + 4, bounds.bottom() - 14, bounds.width() - 8, 14),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("격자 1 m · 휠 확대/축소 · 드래그 이동"));
}

// ============================================================
// SlamMappingCard
// ============================================================
SlamMappingCard::SlamMappingCard(QWidget* parent)
    : QFrame(parent),
      elapsedTimer_(new QTimer(this)),
      elapsedSeconds_(0),
      areaM2_(0.0),
      moving_(false) {
    // objectName("card")를 안 준다 — QFrame#card의 흰 배경+테두리(QSS)를 두르지
    // 않는다. 테두리는 SlamMapView::paintEvent가 이미 직접 그리고 있어서(위 border
    // 없이도 캔버스 자체가 하나의 박스로 보인다), 이 바깥 프레임까지 테두리를 두르면
    // "테두리 안에 테두리"로 겹쳐 보였다. 여백(CARD_PADDING)도 없애 캔버스가 이 카드가
    // 차지하는 바깥 박스 크기 그대로 확장되게 한다.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout();
    header->setSpacing(8);

    // 실제로 매핑 중인지 아직 모르는 초기 상태 — "SLAM 매핑 중"이 기본값이면 페이지에
    // 막 들어왔을 때도(또는 등록된 맵이 없는 로봇을 봐도) 항상 이 문구가 떠 있었다.
    // 실제 상태는 resetForRobot/applyRegisteredMap/markMappingStarted가 뒤이어 바로잡는다.
    badge_ = new QLabel(QStringLiteral("맵 없음"), this);
    badge_->setObjectName("badge");
    badge_->setProperty("level", "offline");
    header->addWidget(badge_);

    targetLabel_ = new QLabel(QStringLiteral("동산주택 2층 · R-04"), this);
    targetLabel_->setProperty("role", "secondary");
    header->addWidget(targetLabel_);
    header->addStretch();

    auto* stats = new QLabel(this);
    stats->setProperty("role", "muted");
    stats->setProperty("role", "mono");
    header->addWidget(stats);
    layout->addLayout(header);

    view_ = new SlamMapView(this);
    layout->addWidget(view_, /*stretch=*/1);

    elapsedLabel_ = stats;  // 시간·면적을 한 줄에 표시 (mono)

    connect(elapsedTimer_, &QTimer::timeout, this, &SlamMappingCard::tickElapsed);
    elapsedTimer_->start(1000);
    tickElapsed();
}

void SlamMappingCard::applyRegisteredMap(const QImage& image, const QString& mapName,
                                          const QString& robotId) {
    view_->setBackgroundImage(image);
    badge_->setText(QStringLiteral("맵 등록됨"));
    badge_->setProperty("level", "ok");
    badge_->style()->unpolish(badge_);
    badge_->style()->polish(badge_);
    targetLabel_->setText(QStringLiteral("%1 · %2").arg(mapName, robotId));
}

void SlamMappingCard::resetForRobot(const QString& robotId) {
    view_->resetMapping();
    badge_->setText(QStringLiteral("맵 없음"));
    badge_->setProperty("level", "offline");
    badge_->style()->unpolish(badge_);
    badge_->style()->polish(badge_);
    targetLabel_->setText(QStringLiteral("맵 없음 · %1").arg(robotId));
    elapsedSeconds_ = 0;
    areaM2_ = 0.0;
    updateStatsLabel();
}

void SlamMappingCard::markMappingStarted() {
    badge_->setText(QStringLiteral("SLAM 매핑 중"));
    badge_->setProperty("level", "info");
    badge_->style()->unpolish(badge_);
    badge_->style()->polish(badge_);
}

void SlamMappingCard::setVelocity(qreal linearMps, qreal angularRadps) {
    moving_ = !qFuzzyIsNull(linearMps) || !qFuzzyIsNull(angularRadps);
}

void SlamMappingCard::tickElapsed() {
    // 서버(slam_progress)가 아직 한 번도 안 왔거나 다음 갱신 사이를 메우는 로컬
    // 보간용 카운트업 — applyProgress()가 오면 그 값으로 덮어써 드리프트를 바로잡는다.
    ++elapsedSeconds_;
    updateStatsLabel();
}

void SlamMappingCard::applyProgress(int elapsedSec, qreal areaM2) {
    elapsedSeconds_ = elapsedSec;
    areaM2_ = areaM2;
    updateStatsLabel();
}

void SlamMappingCard::updateStatsLabel() {
    const int m = elapsedSeconds_ / 60;
    const int s = elapsedSeconds_ % 60;
    elapsedLabel_->setText(QStringLiteral("경과 %1:%2 · %3 m²")
                                .arg(m, 2, 10, QChar('0'))
                                .arg(s, 2, 10, QChar('0'))
                                .arg(areaM2_, 0, 'f', 1));
}
