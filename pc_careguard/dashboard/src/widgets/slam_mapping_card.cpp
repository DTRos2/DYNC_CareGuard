#include "slam_mapping_card.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

#include "theme/tokens.h"
#include "widgets/camera_live_view.h"
#include "widgets/canvas_geometry.h"

namespace {
constexpr qreal kTickMs = 50.0;
constexpr qreal kPxPerMps = 90.0;  // 캔버스 논리 px per (m/s * s) — 실측 없는 목업 스케일
constexpr qreal kMinZoom = 0.5;
constexpr qreal kMaxZoom = 4.0;
constexpr qreal kZoomStepFactor = 1.15;
}

// ============================================================
// SlamMapView
// ============================================================
SlamMapView::SlamMapView(QWidget* parent)
    : QFrame(parent), simTimer_(new QTimer(this)), heading_(0.0), linearMps_(0.0), angularRadps_(0.0) {
    setObjectName("slamCanvas");
    setMinimumHeight(320);
    robotPos_ = QPointF(90, 260);
    trajectory_.append(robotPos_);

    // 우측 상단에 카메라 라이브 화면 자리를 띄운다 — 자식 위젯이라 아래 paintEvent의
    // 확대/패닝과 무관하게 항상 같은 화면 위치에 고정된다.
    cameraView_ = new CameraLiveView(this);
    cameraView_->move(static_cast<int>(exploredBounds().right()) - cameraView_->width() - 8,
                       static_cast<int>(exploredBounds().top()) + 8);

    connect(simTimer_, &QTimer::timeout, this, &SlamMapView::tick);
    simTimer_->start(static_cast<int>(kTickMs));
}

void SlamMapView::setVelocity(qreal linearMps, qreal angularRadps) {
    linearMps_ = linearMps;
    angularRadps_ = angularRadps;
}

void SlamMapView::resetMapping() {
    trajectory_.clear();
    robotPos_ = QPointF(90, 260);
    heading_ = 0.0;
    trajectory_.append(robotPos_);
    update();
}

void SlamMapView::setBackgroundImage(const QImage& image) {
    backgroundImage_ = image;
    update();
}

QRectF SlamMapView::exploredBounds() const {
    const QRectF r = rect();
    const qreal margin = 28;
    return r.adjusted(margin, margin, -margin, -margin);
}

void SlamMapView::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);

    cameraView_->move(static_cast<int>(exploredBounds().right()) - cameraView_->width() - 8,
                       static_cast<int>(exploredBounds().top()) + 8);

    const QSize oldSize = event->oldSize();
    if (oldSize.width() <= 0 || oldSize.height() <= 0 || oldSize == event->size()) return;

    // WaypointCanvas::resizeEvent와 같은 이유 — 로봇 위치·주행 궤적이 캔버스 절대
    // 픽셀 좌표라서, 리사이즈 전/후 바닥 영역 비율을 보존해 옮기지 않으면 창을
    // 최대화했을 때 궤적이 지도에서 벗어나 보인다.
    const qreal margin = 28;
    const QRectF oldBounds = QRectF(QPointF(0, 0), oldSize).adjusted(margin, margin, -margin, -margin);
    const QRectF newBounds = exploredBounds();

    robotPos_ = CanvasGeometry::remapOnResize(robotPos_, oldBounds, newBounds, backgroundImage_);
    for (QPointF& p : trajectory_) {
        p = CanvasGeometry::remapOnResize(p, oldBounds, newBounds, backgroundImage_);
    }

    // 확대/패닝 상태는 리사이즈 전 배치 기준이라 그대로 두면 새 배치와 어긋난다.
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

void SlamMapView::tick() {
    if (qFuzzyIsNull(linearMps_) && qFuzzyIsNull(angularRadps_)) return;

    const qreal dt = kTickMs / 1000.0;
    heading_ += angularRadps_ * dt;

    const QPointF delta(std::cos(heading_) * linearMps_ * kPxPerMps * dt,
                         std::sin(heading_) * linearMps_ * kPxPerMps * dt);
    QPointF next = robotPos_ + delta;

    const QRectF bounds = exploredBounds();
    next.setX(qBound(bounds.left() + 6, next.x(), bounds.right() - 6));
    next.setY(qBound(bounds.top() + 6, next.y(), bounds.bottom() - 6));

    if (next != robotPos_) {
        const qreal pixelDist = std::hypot(next.x() - robotPos_.x(), next.y() - robotPos_.y());
        robotPos_ = next;
        trajectory_.append(robotPos_);
        if (trajectory_.size() > 600) trajectory_.removeFirst();
        emit distanceTraveled(pixelDist / kPxPerMps);
        update();
    } else if (!qFuzzyIsNull(angularRadps_)) {
        update();  // 제자리 회전도 화살표 방향 갱신
    }
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

    if (!backgroundImage_.isNull()) {
        // 파일로 등록된 실제 맵 — pgm 픽셀을 그대로 렌더링한다.
        const QImage scaled = backgroundImage_.scaled(bounds.size().toSize(), Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation);
        const QPointF topLeft(bounds.center().x() - scaled.width() / 2.0,
                               bounds.center().y() - scaled.height() / 2.0);
        p.drawImage(topLeft, scaled);
    }
    // 배경 이미지가 없을 때(파일 등록 전 SLAM 매핑 진행 중 목업)는 위 흰 바닥만 남는다.
    // 우상단의 "미탐색" 회색 박스 자리는 실은 카메라 라이브 화면 자리였다 —
    // cameraView_(실제 자식 위젯)로 대체했다.

    // 주행 궤적 (점선)
    if (trajectory_.size() > 1) {
        QPainterPath traj;
        traj.moveTo(trajectory_.first());
        for (int i = 1; i < trajectory_.size(); ++i) traj.lineTo(trajectory_.at(i));
        QPen trajPen(Tokens::ACCENT, 1.5, Qt::DashLine, Qt::RoundCap);
        p.setPen(trajPen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(traj);
    }

    p.restore();  // 확대/패닝 종료 — 이 아래는 항상 고정 크기로 그린다.

    // 캡션 — 로봇 마커(라이다 스캔 부채꼴 포함)는 요청에 따라 제거했다.
    p.setPen(Tokens::TEXT_MUTED);
    QFont captionFont = p.font();
    captionFont.setPixelSize(9);
    p.setFont(captionFont);
    p.drawText(QRectF(bounds.left() + 4, bounds.bottom() - 14, bounds.width() - 8, 14),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("격자 0.05 m/px · 휠 확대/축소 · 드래그 이동"));
}

// ============================================================
// SlamMappingCard
// ============================================================
SlamMappingCard::SlamMappingCard(QWidget* parent)
    : QFrame(parent),
      elapsedTimer_(new QTimer(this)),
      elapsedSeconds_(0),
      areaM2_(12.4),
      moving_(false) {
    setObjectName("card");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout();
    header->setSpacing(8);

    badge_ = new QLabel(QStringLiteral("SLAM 매핑 중"), this);
    badge_->setObjectName("badge");
    badge_->setProperty("level", "info");
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

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(6);

    auto* restartBtn = new QPushButton(QStringLiteral("매핑 재시작"), this);
    restartBtn->setProperty("variant", "secondary");
    connect(restartBtn, &QPushButton::clicked, this, [this] {
        view_->resetMapping();
        elapsedSeconds_ = 0;
        areaM2_ = 0.0;
    });
    buttons->addWidget(restartBtn);

    auto* cancelBtn = new QPushButton(QStringLiteral("매핑 취소"), this);
    cancelBtn->setProperty("variant", "secondary");
    buttons->addWidget(cancelBtn);

    buttons->addStretch();

    auto* estopBtn = new QPushButton(QStringLiteral("비상정지"), this);
    estopBtn->setProperty("variant", "danger");
    estopBtn->setProperty("size", "l");
    connect(estopBtn, &QPushButton::clicked, this, &SlamMappingCard::emergencyStopRequested);
    buttons->addWidget(estopBtn);

    layout->addLayout(buttons);

    elapsedLabel_ = stats;  // 시간·면적을 한 줄에 표시 (mono)

    connect(elapsedTimer_, &QTimer::timeout, this, &SlamMappingCard::tickElapsed);
    elapsedTimer_->start(1000);
    tickElapsed();

    connect(view_, &SlamMapView::distanceTraveled, this, [this](qreal deltaMeters) {
        // 대략적인 커버리지 증가 목업 — 실제 점유격자 계산이 아니다.
        areaM2_ += deltaMeters * 0.6;
    });
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

void SlamMappingCard::setVelocity(qreal linearMps, qreal angularRadps) {
    moving_ = !qFuzzyIsNull(linearMps) || !qFuzzyIsNull(angularRadps);
    view_->setVelocity(linearMps, angularRadps);
}

void SlamMappingCard::tickElapsed() {
    ++elapsedSeconds_;
    const int m = elapsedSeconds_ / 60;
    const int s = elapsedSeconds_ % 60;
    elapsedLabel_->setText(QStringLiteral("경과 %1:%2 · %3 m²")
                                .arg(m, 2, 10, QChar('0'))
                                .arg(s, 2, 10, QChar('0'))
                                .arg(areaM2_, 0, 'f', 1));
}
