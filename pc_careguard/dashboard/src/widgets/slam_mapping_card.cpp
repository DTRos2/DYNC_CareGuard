#include "slam_mapping_card.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

#include "theme/tokens.h"

namespace {
constexpr qreal kTickMs = 50.0;
constexpr qreal kPxPerMps = 90.0;  // 캔버스 논리 px per (m/s * s) — 실측 없는 목업 스케일
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

    if (!backgroundImage_.isNull()) {
        // 파일로 등록된 실제 맵 — pgm 픽셀을 그대로 렌더링한다.
        p.setPen(QPen(Tokens::TEXT_SECONDARY, 1.5));
        p.setBrush(Tokens::BG_SURFACE);
        p.drawRoundedRect(bounds, 6, 6);
        const QImage scaled = backgroundImage_.scaled(bounds.size().toSize(), Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation);
        const QPointF topLeft(bounds.center().x() - scaled.width() / 2.0,
                               bounds.center().y() - scaled.height() / 2.0);
        p.drawImage(topLeft, scaled);
    } else {
        // 탐색된 영역 (흰 바닥) — 파일 등록 전 SLAM 매핑 진행 중 목업.
        p.setPen(QPen(Tokens::TEXT_SECONDARY, 1.5));
        p.setBrush(Tokens::BG_SURFACE);
        p.drawRoundedRect(bounds, 6, 6);

        // 미탐색 영역 표시 (우상단 구석)
        const QRectF unexplored(bounds.right() - bounds.width() * 0.32, bounds.top(), bounds.width() * 0.32,
                                 bounds.height() * 0.3);
        p.setPen(Qt::NoPen);
        QColor unexploredFill = Tokens::BORDER_STRONG;
        unexploredFill.setAlphaF(0.55);
        p.setBrush(unexploredFill);
        p.drawRect(unexplored);
        p.setPen(Tokens::TEXT_SECONDARY);
        p.drawText(unexplored, Qt::AlignCenter, QStringLiteral("미탐색"));
    }

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

    // 시작 지점
    const QPointF startPt = trajectory_.first();
    p.setPen(QPen(Tokens::TEXT_MUTED, 1.2));
    p.setBrush(Tokens::BG_SUNKEN);
    p.drawEllipse(startPt, 5, 5);
    p.setPen(Tokens::TEXT_SECONDARY);
    p.drawText(QRectF(startPt.x() + 8, startPt.y() - 8, 90, 16), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("시작 지점"));

    // 라이다 스캔 (로봇 주변 부채꼴 라인)
    p.setPen(QPen(Tokens::ACCENT, 0.8));
    for (int i = -3; i <= 3; ++i) {
        const qreal a = heading_ + i * 0.22;
        const QPointF ray(robotPos_.x() + std::cos(a) * 26, robotPos_.y() + std::sin(a) * 26);
        p.drawLine(robotPos_, ray);
    }

    // 로봇 위치 (헤일로 + 본체 + 헤딩)
    QColor halo = Tokens::ACCENT;
    halo.setAlphaF(0.18);
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawEllipse(robotPos_, 12, 12);
    p.setBrush(Tokens::ACCENT);
    p.drawEllipse(robotPos_, 7, 7);
    p.setPen(QPen(Tokens::BG_SURFACE, 1.6));
    const QPointF heading(robotPos_.x() + std::cos(heading_) * 9, robotPos_.y() + std::sin(heading_) * 9);
    p.drawLine(robotPos_, heading);

    // 캡션
    p.setPen(Tokens::TEXT_MUTED);
    QFont captionFont = p.font();
    captionFont.setPixelSize(9);
    p.setFont(captionFont);
    p.drawText(QRectF(bounds.left() + 4, bounds.bottom() - 14, bounds.width() - 8, 14),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("격자 0.05 m/px · 라이다 스캔 표시"));
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
