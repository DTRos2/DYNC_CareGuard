#include "robot_control_modal.h"

#include <algorithm>

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {

QLabel* makeBadge(const QString& text, QWidget* parent) {
    auto* badge = new QLabel(text, parent);
    badge->setObjectName("badge");
    badge->setProperty("level", Tokens::Prop::EMERGENCY);
    return badge;
}

QLabel* statusDot(bool ok, const QString& label, QWidget* parent) {
    auto* wrap = new QLabel(parent);
    const QColor color = ok ? Tokens::OK.main : Tokens::OFFLINE.main;
    wrap->setText(QStringLiteral("<span style=\"color:%1;\">●</span> %2").arg(color.name(), label));
    wrap->setProperty("role", "secondary");
    return wrap;
}

}  // namespace

// 모달 왼쪽의 작은 맵 미리보기 — P6WaypointCanvas(전체 편집용, 클릭·드래그·경로
// 편집 다 딸려있음)를 그대로 쓰기엔 여기선 "지금 웨이포인트가 어디 있는지"만
// 보여주면 충분해서 과하다. 좌표 범위를 위젯 크기에 맞춰 정규화해 원+라벨만
// 그리는 훨씬 가벼운 전용 위젯을 둔다.
class MiniMapView : public QFrame {
public:
    explicit MiniMapView(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName("miniMapView");
        // 사용자가 준 목업(회색 배경 없이 흰 맵 박스가 카드 위에 얹힌 모양)과 맞춰
        // 흰 배경 + 얇은 테두리로 통일한다.
        setStyleSheet(QStringLiteral("QFrame#miniMapView{background:%1; border:0.5px solid %2; border-radius:8px;}")
                          .arg(Tokens::BG_SURFACE.name(), Tokens::BORDER.name()));
        setMinimumSize(260, 200);
    }

    void setWaypoints(const QVector<ApiClient::WaypointInfo>& waypoints) {
        waypoints_ = waypoints;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QFrame::paintEvent(event);
        if (waypoints_.isEmpty()) return;

        double minX = waypoints_.first().posX, maxX = minX;
        double minY = waypoints_.first().posY, maxY = minY;
        for (const auto& wp : waypoints_) {
            minX = std::min(minX, wp.posX);
            maxX = std::max(maxX, wp.posX);
            minY = std::min(minY, wp.posY);
            maxY = std::max(maxY, wp.posY);
        }
        const double spanX = std::max(maxX - minX, 0.5);
        const double spanY = std::max(maxY - minY, 0.5);
        constexpr int MARGIN = 28;
        const QRectF plotRect = rect().adjusted(MARGIN, MARGIN, -MARGIN, -MARGIN);

        auto toPoint = [&](double x, double y) {
            // map 프레임은 y가 위로 갈수록 커지는 좌표계라 화면(y가 아래로 갈수록
            // 커짐)에 그리려면 y를 뒤집어야 한다.
            const double nx = (x - minX) / spanX;
            const double ny = 1.0 - (y - minY) / spanY;
            return QPointF(plotRect.left() + nx * plotRect.width(), plotRect.top() + ny * plotRect.height());
        };

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        if (waypoints_.size() >= 2) {
            QPen pathPen(Tokens::ACCENT, 1.5, Qt::DashLine);
            painter.setPen(pathPen);
            for (int i = 1; i < waypoints_.size(); ++i) {
                if (waypoints_[i].kind == QStringLiteral("charge")) continue;
                if (waypoints_[i - 1].kind == QStringLiteral("charge")) continue;
                painter.drawLine(toPoint(waypoints_[i - 1].posX, waypoints_[i - 1].posY),
                                  toPoint(waypoints_[i].posX, waypoints_[i].posY));
            }
        }

        for (const auto& wp : waypoints_) {
            const QPointF center = toPoint(wp.posX, wp.posY);
            const bool isCharge = wp.kind == QStringLiteral("charge");
            const QColor fill = isCharge ? Tokens::ACCENT : Tokens::BG_SURFACE;
            const QColor border = isCharge ? Tokens::ACCENT : Tokens::BORDER_STRONG;
            painter.setPen(QPen(border, 1.5));
            painter.setBrush(fill);
            const double r = isCharge ? 7.0 : 6.0;
            painter.drawEllipse(center, r, r);

            painter.setPen(Tokens::TEXT_SECONDARY);
            QFont f = painter.font();
            f.setPixelSize(10);
            painter.setFont(f);
            painter.drawText(center + QPointF(-24, 16), QString("%1 %2").arg(wp.code, wp.name));
        }
    }

private:
    QVector<ApiClient::WaypointInfo> waypoints_;
};

RobotControlModal::RobotControlModal(const Data& data, QWidget* parent) : QDialog(parent), data_(data) {
    setWindowTitle(QStringLiteral("로봇 상세 — %1").arg(data_.robotCode));
    setModal(true);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setFixedWidth(Tokens::Layout::MODAL_W_M);

    apiClient_ = new ApiClient(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildHeader());
    root->addWidget(buildBody());
    root->addWidget(buildFooter());

    loadChargeWaypoint();
}

QWidget* RobotControlModal::buildHeader() {
    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    auto* title = new QLabel(data_.headerTitle, header);
    title->setProperty("role", "h3");
    layout->addWidget(title);

    if (!data_.urgencyBadge.isEmpty()) {
        layout->addWidget(makeBadge(data_.urgencyBadge, header));
    }

    layout->addStretch();

    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_SECONDARY, 16), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(closeBtn);

    return header;
}

QWidget* RobotControlModal::buildBody() {
    auto* body = new QWidget(this);
    auto* layout = new QHBoxLayout(body);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);
    layout->addWidget(buildLeftColumn(), 5);
    layout->addWidget(buildRightColumn(), 6);
    return body;
}

QWidget* RobotControlModal::buildLeftColumn() {
    auto* column = new QWidget(this);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* mapLabel = new QLabel(data_.mapLabel, column);
    mapLabel->setProperty("role", "label");
    layout->addWidget(mapLabel);

    mapView_ = new MiniMapView(column);
    layout->addWidget(mapView_, 1);

    if (!data_.positionLabel.isEmpty()) {
        auto* posLabel = new QLabel(data_.positionLabel, column);
        posLabel->setProperty("role", "secondary");
        layout->addWidget(posLabel);
    }

    return column;
}

QWidget* RobotControlModal::buildStatCard(const QString& label, const QString& value, const QColor& valueColor) {
    auto* card = new QFrame(this);
    card->setObjectName("robotStatCard");
    card->setStyleSheet(QStringLiteral("QFrame#robotStatCard{background:%1; border-radius:8px;}")
                             .arg(Tokens::BG_SUNKEN.name()));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(Tokens::Layout::CARD_PADDING, 10, Tokens::Layout::CARD_PADDING, 10);
    layout->setSpacing(4);

    auto* labelWidget = new QLabel(label, card);
    labelWidget->setProperty("role", "caption");
    layout->addWidget(labelWidget);

    auto* valueWidget = new QLabel(value, card);
    valueWidget->setObjectName("kpiValue");
    QFont valueFont = valueWidget->font();
    valueFont.setPixelSize(17);
    valueFont.setWeight(QFont::DemiBold);
    valueWidget->setFont(valueFont);
    valueWidget->setStyleSheet(QStringLiteral("color:%1;").arg(valueColor.name()));
    layout->addWidget(valueWidget);

    return card;
}

QWidget* RobotControlModal::buildStatusDotsRow() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    // 2026-09-12 확정 — 웨어러블은 이 카드가 다룰 정보가 아니라 뺐다(로봇 상태
    // 카드이지 대상자 웨어러블 상태 카드가 아니다). 카메라/마이크/Nav2 셋만 남는다.
    layout->addWidget(statusDot(data_.cameraOk, QStringLiteral("카메라"), row));
    layout->addWidget(statusDot(data_.micOk, QStringLiteral("마이크"), row));
    layout->addWidget(statusDot(data_.nav2Ok, QStringLiteral("Nav2"), row));
    layout->addStretch();
    return row;
}

QWidget* RobotControlModal::buildEventBox() {
    if (data_.eventCode.isEmpty()) return nullptr;

    auto* box = new QFrame(this);
    box->setObjectName("eventBox");
    box->setStyleSheet(QStringLiteral("QFrame#eventBox{background:%1; border:0.5px solid %2; border-radius:8px;}")
                            .arg(Tokens::EMERGENCY.soft.name(), Tokens::EMERGENCY.border.name()));
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(4);

    auto* title = new QLabel(QStringLiteral("진행 중 이벤트 · %1").arg(data_.eventCode), box);
    title->setStyleSheet(QStringLiteral("font-weight:600; color:%1;").arg(Tokens::EMERGENCY.main.name()));
    layout->addWidget(title);

    auto* body = new QLabel(data_.eventText, box);
    body->setWordWrap(true);
    body->setProperty("role", "secondary");
    layout->addWidget(body);

    auto* openLink = new QPushButton(QStringLiteral("이벤트 상세 열기 →"), box);
    openLink->setProperty("variant", "ghost");
    openLink->setStyleSheet(QStringLiteral("text-align:left; padding:0; color:%1;").arg(Tokens::EMERGENCY.main.name()));
    connect(openLink, &QPushButton::clicked, this,
            [this]() { emit eventNavigationRequested(data_.eventCode); });
    layout->addWidget(openLink);

    return box;
}

QWidget* RobotControlModal::buildMissionList() {
    auto* box = new QWidget(this);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("최근 미션"), box);
    title->setProperty("role", "label");
    layout->addWidget(title);

    for (const auto& mission : data_.recentMissions) {
        auto* row = new QWidget(box);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto* time = new QLabel(mission.time, row);
        time->setProperty("role", "secondary");
        time->setFixedWidth(48);
        rowLayout->addWidget(time);

        auto* label = new QLabel(mission.label, row);
        rowLayout->addWidget(label, 1);

        auto* status = new QLabel(mission.status, row);
        status->setStyleSheet(QStringLiteral("color:%1;").arg(Tokens::ACCENT.name()));
        rowLayout->addWidget(status);

        layout->addWidget(row);
    }

    return box;
}

QWidget* RobotControlModal::buildRightColumn() {
    auto* column = new QWidget(this);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* statsRow = new QWidget(column);
    auto* statsLayout = new QHBoxLayout(statsRow);
    statsLayout->setContentsMargins(0, 0, 0, 0);
    statsLayout->setSpacing(Tokens::Layout::CARD_GAP);
    // 30% 이하는 빨강(EMERGENCY), 50% 이하는 주황(ATTENTION), 그 외엔 기본 텍스트색.
    QColor batteryColor = Tokens::TEXT_PRIMARY;
    if (data_.batteryPct >= 0 && data_.batteryPct <= 30) batteryColor = Tokens::EMERGENCY.main;
    else if (data_.batteryPct >= 0 && data_.batteryPct <= 50) batteryColor = Tokens::ATTENTION.main;
    statsLayout->addWidget(buildStatCard(QStringLiteral("배터리"),
                                          data_.batteryPct >= 0 ? QStringLiteral("%1%").arg(data_.batteryPct)
                                                                : QStringLiteral("-"),
                                          batteryColor));
    statsLayout->addWidget(buildStatCard(QStringLiteral("통신 지연"),
                                          data_.latencySec >= 0 ? QStringLiteral("%1s").arg(data_.latencySec, 0, 'f', 1)
                                                                : QStringLiteral("-")));
    layout->addWidget(statsRow);

    layout->addWidget(buildStatusDotsRow());

    if (auto* eventBox = buildEventBox()) {
        layout->addWidget(eventBox);
    }

    layout->addWidget(buildMissionList());
    layout->addStretch();

    return column;
}

QWidget* RobotControlModal::buildFooter() {
    auto* footer = new QFrame(this);
    footer->setObjectName("modalFooter");
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);

    auto* estopBtn = new QPushButton(QStringLiteral("비상정지"), footer);
    estopBtn->setProperty("variant", "danger");
    layout->addWidget(estopBtn);

    // 2026-09-12 확정 — "충전 복귀" 버튼은 따로 없다. 순찰 취소를 누르면 5초 뒤
    // 자동으로 충전소 웨이포인트까지 이동 명령이 나가므로 그 기능을 흡수했다.
    cancelPatrolBtn_ = new QPushButton(QStringLiteral("순찰 취소"), footer);
    cancelPatrolBtn_->setProperty("variant", "secondary");
    connect(cancelPatrolBtn_, &QPushButton::clicked, this, &RobotControlModal::handleCancelPatrolClicked);
    layout->addWidget(cancelPatrolBtn_);

    layout->addStretch();

    // 진행 중 이벤트가 없어도(예: 정상 순찰 중인 로봇) 버튼 자체는 항상 보인다 —
    // 다만 이동할 이벤트가 없으니 그럴 땐 비활성화한다.
    auto* goToEventBtn = new QPushButton(QStringLiteral("이벤트로 이동"), footer);
    goToEventBtn->setObjectName("goToEventBtn");
    goToEventBtn->setStyleSheet(
        QStringLiteral("QPushButton#goToEventBtn{background:%1; color:#FFFFFF; border:none; font-weight:500;}"
                        "QPushButton#goToEventBtn:hover{background:#12684A;}"
                        "QPushButton#goToEventBtn:disabled{background:%2; color:%3;}")
            .arg(Tokens::OK.main.name(), Tokens::BG_SUNKEN.name(), Tokens::TEXT_MUTED.name()));
    goToEventBtn->setEnabled(!data_.eventCode.isEmpty());
    connect(goToEventBtn, &QPushButton::clicked, this,
            [this]() { emit eventNavigationRequested(data_.eventCode); });
    layout->addWidget(goToEventBtn);

    return footer;
}

void RobotControlModal::loadChargeWaypoint() {
    // 순찰 취소 후 "충전소로 복귀"를 실제로 보내려면 그 로봇의 활성 맵과, 그 맵의
    // 충전 스테이션 웨이포인트(kind=='charge') 좌표를 미리 알아야 한다. 모달을 열자마자
    // 백그라운드로 미리 가져와두고, 버튼을 누른 시점엔 이미 준비돼 있게 한다.
    apiClient_->fetchActiveMaps([this](bool ok, const QVector<ApiClient::MapInfo>& maps) {
        if (!ok) return;
        for (const auto& map : maps) {
            if (map.robotCode != data_.robotCode) continue;
            mapId_ = map.id;
            break;
        }
        if (mapId_ <= 0) return;

        apiClient_->fetchWaypoints(mapId_, [this](bool wpOk, const QVector<ApiClient::WaypointInfo>& waypoints) {
            if (!wpOk) return;
            if (mapView_) mapView_->setWaypoints(waypoints);
            for (const auto& wp : waypoints) {
                if (wp.kind == QStringLiteral("charge")) {
                    chargeWaypoint_ = wp;
                    chargeWaypointLoaded_ = true;
                    break;
                }
            }
        });
    });
}

void RobotControlModal::handleCancelPatrolClicked() {
    cancelPatrolBtn_->setEnabled(false);
    cancelPatrolBtn_->setText(QStringLiteral("취소 중..."));

    apiClient_->cancelPatrolRun(data_.robotCode, [this](bool ok, const QString& err) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("순찰 취소 실패"),
                                  err.isEmpty() ? QStringLiteral("로봇 연결을 확인해주세요.") : err);
            resetCancelButton();
            return;
        }
        beginReturnToChargeCountdown();
    });
}

void RobotControlModal::beginReturnToChargeCountdown() {
    countdownRemaining_ = 5;
    cancelPatrolBtn_->setText(QStringLiteral("%1초 후 충전소로 복귀").arg(countdownRemaining_));

    countdownTimer_ = new QTimer(this);
    connect(countdownTimer_, &QTimer::timeout, this, &RobotControlModal::tickCountdown);
    countdownTimer_->start(1000);
}

void RobotControlModal::tickCountdown() {
    --countdownRemaining_;
    if (countdownRemaining_ <= 0) {
        countdownTimer_->stop();
        countdownTimer_->deleteLater();
        countdownTimer_ = nullptr;
        sendReturnToCharge();
        return;
    }
    cancelPatrolBtn_->setText(QStringLiteral("%1초 후 충전소로 복귀").arg(countdownRemaining_));
}

void RobotControlModal::sendReturnToCharge() {
    if (!chargeWaypointLoaded_) {
        QMessageBox::warning(this, QStringLiteral("충전소 복귀 실패"),
                              QStringLiteral("이 로봇의 충전 스테이션 좌표를 아직 못 불러왔습니다."));
        resetCancelButton();
        return;
    }

    cancelPatrolBtn_->setText(QStringLiteral("충전소로 복귀 중..."));
    apiClient_->driveToPoint(data_.robotCode, mapId_, chargeWaypoint_.code, chargeWaypoint_.posX,
                              chargeWaypoint_.posY, chargeWaypoint_.yaw, [this](bool ok, const QString& err) {
                                  if (!ok) {
                                      QMessageBox::warning(
                                          this, QStringLiteral("충전소 복귀 실패"),
                                          err.isEmpty() ? QStringLiteral("로봇 연결을 확인해주세요.") : err);
                                      resetCancelButton();
                                      return;
                                  }
                                  cancelPatrolBtn_->setText(QStringLiteral("충전소로 복귀 중"));
                              });
}

void RobotControlModal::resetCancelButton() {
    cancelPatrolBtn_->setEnabled(true);
    cancelPatrolBtn_->setText(QStringLiteral("순찰 취소"));
}
