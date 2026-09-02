#include "p6_patrol_page.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QAction>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedLayout>
#include <QStyle>
#include <QTimeEdit>
#include <QVBoxLayout>
#include <cmath>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"
#include "widgets/toggle_switch.h"
#include "widgets/waypoint_popover.h"

namespace {
constexpr qreal kPxPerMeter = 40.0;
}

P6PatrolPage::P6PatrolPage(QWidget* parent) : QWidget(parent), currentMapIndex_(0), routeEditMode_(false) {
    // 4개 맵 — P5 매핑 현황에 등록된 것과 동일한 맵/로봇/대상자 조합.
    // 행복빌라 302만 와이어프레임의 실제 예시 데이터를 쓰고, 나머지는 충전 스테이션만
    // 있는 빈 맵으로 시작한다 (사용자가 직접 웨이포인트를 채워나가면 된다).
    {
        MapData m;
        m.mapName = QStringLiteral("행복빌라 302");
        m.robotId = QStringLiteral("R-01");
        m.recipientName = QStringLiteral("김순자");
        m.waypoints = {
            {QStringLiteral("WP_01"), QStringLiteral("거실"), false, QPointF(90, 80), 90},
            {QStringLiteral("WP_02"), QStringLiteral("주방"), false, QPointF(210, 80), 27},
            {QStringLiteral("WP_03"), QStringLiteral("안방"), false, QPointF(290, 160), 315},
            {QStringLiteral("WP_04"), QStringLiteral("현관"), false, QPointF(290, 260), 180},
            {QStringLiteral("WP_05"), QStringLiteral("충전 스테이션"), true, QPointF(90, 260), 0},
        };
        m.routes[QStringLiteral("주간 순찰")] = {QStringLiteral("WP_01"), QStringLiteral("WP_02"),
                                                QStringLiteral("WP_03"), QStringLiteral("WP_04")};
        m.routes[QStringLiteral("야간 순찰")] = {};
        m.currentRoute = QStringLiteral("주간 순찰");
        m.nextWpSeq = 6;
        m.schedules.append(
            Schedule{QStringLiteral("주간 순찰"), QStringLiteral("10:00"), {0, 1, 2, 3, 4, 5, 6}, true});
        m.schedules.append(
            Schedule{QStringLiteral("야간 순찰"), QStringLiteral("21:00"), {0, 1, 2, 3, 4, 5, 6}, false});
        maps_.append(m);
    }
    auto addBlankMap = [this](const QString& mapName, const QString& robotId, const QString& recipient) {
        MapData m;
        m.mapName = mapName;
        m.robotId = robotId;
        m.recipientName = recipient;
        m.waypoints = {{QStringLiteral("WP_01"), QStringLiteral("충전 스테이션"), true, QPointF(90, 260), 0}};
        m.nextWpSeq = 2;
        maps_.append(m);
    };
    addBlankMap(QStringLiteral("은빛아파트 101"), QStringLiteral("R-02"), QStringLiteral("박영호"));
    addBlankMap(QStringLiteral("솔마을 7호"), QStringLiteral("R-03"), QStringLiteral("이말순"));
    addBlankMap(QStringLiteral("동산주택 2층"), QStringLiteral("R-04"), QStringLiteral("최금례"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    layout->addWidget(buildHeaderRow());

    auto* content = new QHBoxLayout();
    content->setSpacing(Tokens::Layout::CARD_GAP);
    content->addWidget(buildCanvasCard(), /*stretch=*/15);
    content->addWidget(buildRightColumn(), /*stretch=*/0);
    layout->addLayout(content, /*stretch=*/1);

    popover_ = new WaypointPopover(this);
    connect(popover_, &WaypointPopover::saveRequested, this, &P6PatrolPage::handlePopoverSave);
    connect(popover_, &WaypointPopover::deleteRequested, this, &P6PatrolPage::handleCanvasDeleteRequested);

    connect(canvas_, &WaypointCanvas::waypointAddRequested, this, &P6PatrolPage::handleCanvasAddRequested);
    connect(canvas_, &WaypointCanvas::waypointEditRequested, this, &P6PatrolPage::handleCanvasEditRequested);
    connect(canvas_, &WaypointCanvas::waypointDeleteRequested, this, &P6PatrolPage::handleCanvasDeleteRequested);
    connect(canvas_, &WaypointCanvas::waypointMoved, this, &P6PatrolPage::handleCanvasMoved);
    connect(canvas_, &WaypointCanvas::waypointDriveRequested, this, &P6PatrolPage::driveToSingleWaypoint);
    connect(canvas_, &WaypointCanvas::routeWaypointClicked, this, &P6PatrolPage::handleRouteWaypointClicked);
    connect(canvas_, &WaypointCanvas::chargingClickBlocked, this, [this] {
        QMessageBox::information(this, QStringLiteral("충전 스테이션"),
                                  QStringLiteral("충전 스테이션은 경로 순서에 포함할 수 없습니다 (자동 출발·복귀)."));
    });
    connect(canvas_, &WaypointCanvas::testRunFinished, this, [this] {
        QMessageBox::information(this, QStringLiteral("주행 완료"), QStringLiteral("목적지까지 주행을 마쳤습니다."));
    });

    loadMapData(0);
}

QWidget* P6PatrolPage::buildHeaderRow() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("순찰 · 경로 설정"), row);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    mapChipBtn_ = new QPushButton(SimpleIcons::mapPin(Tokens::TEXT_SECONDARY, 12), QString(), row);
    mapChipBtn_->setObjectName("robotChip");
    mapChipBtn_->setIconSize(QSize(12, 12));
    mapMenu_ = new QMenu(mapChipBtn_);
    mapChipBtn_->setMenu(mapMenu_);
    rebuildMapMenu();
    layout->addWidget(mapChipBtn_);
    layout->addStretch();

    auto* deployBtn = new QPushButton(SimpleIcons::deviceFloppy(Tokens::TEXT_INVERSE, 12),
                                       QStringLiteral("저장 · 로봇에 배포"), row);
    deployBtn->setProperty("variant", "primary");
    deployBtn->setIconSize(QSize(12, 12));
    connect(deployBtn, &QPushButton::clicked, this, [this] {
        QMessageBox::information(
            this, QStringLiteral("배포 완료"),
            QStringLiteral("웨이포인트·경로·스케줄이 저장되어 %1에 배포되었습니다.").arg(maps_[currentMapIndex_].robotId));
    });
    layout->addWidget(deployBtn);

    return row;
}

QWidget* P6PatrolPage::buildCanvasCard() {
    auto* card = new QFrame(this);
    card->setObjectName("card");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    layout->setSpacing(8);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);

    auto* addModeBtn = new QPushButton(QStringLiteral("WP 추가"), card);
    auto* moveModeBtn = new QPushButton(QStringLiteral("이동"), card);
    for (auto* btn : {addModeBtn, moveModeBtn}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
    }
    addModeBtn->setProperty("pos", "first");
    moveModeBtn->setProperty("pos", "last");
    addModeBtn->setChecked(true);
    toolbar->addWidget(addModeBtn);
    toolbar->addWidget(moveModeBtn);

    // WP 추가 / 이동은 배타적 — 하나만 선택 가능하고, 선택된 모드에 따라 캔버스
    // 클릭·드래그 동작이 달라진다 (WaypointCanvas::InteractionMode).
    auto* modeGroup = new QButtonGroup(card);
    modeGroup->setExclusive(true);
    modeGroup->addButton(addModeBtn);
    modeGroup->addButton(moveModeBtn);
    connect(addModeBtn, &QPushButton::clicked, this, [this] {
        canvas_->setInteractionMode(WaypointCanvas::InteractionMode::AddWaypoint);
    });
    connect(moveModeBtn, &QPushButton::clicked, this, [this] {
        canvas_->setInteractionMode(WaypointCanvas::InteractionMode::MoveWaypoint);
    });

    auto* hint = new QLabel(
        QStringLiteral("휠 확대/축소 · 빈 곳 드래그로 이동 · 더블클릭 원상복귀 — 그 외 모드별 안내는 지도 하단 참고"),
        card);
    hint->setProperty("role", "muted");
    hint->setStyleSheet(QStringLiteral("font-size:11px;"));
    toolbar->addWidget(hint);
    toolbar->addStretch();
    layout->addLayout(toolbar);

    canvas_ = new WaypointCanvas(card);
    layout->addWidget(canvas_, /*stretch=*/1);

    return card;
}

QWidget* P6PatrolPage::buildRightColumn() {
    auto* wrap = new QWidget(this);
    wrap->setFixedWidth(236);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    layout->addWidget(buildWpListCard());
    layout->addWidget(buildRouteCard());
    layout->addWidget(buildScheduleCard());
    layout->addStretch();

    return wrap;
}

QWidget* P6PatrolPage::buildWpListCard() {
    wpListCard_ = new QFrame(this);
    wpListCard_->setObjectName("card");
    auto* outer = new QVBoxLayout(wpListCard_);
    outer->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                               Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    outer->setSpacing(4);

    wpListLayout_ = new QVBoxLayout();
    wpListLayout_->setSpacing(0);
    outer->addLayout(wpListLayout_);

    return wpListCard_;
}

QWidget* P6PatrolPage::buildRouteCard() {
    auto* card = new QFrame(this);
    card->setObjectName("card");
    auto* outer = new QVBoxLayout(card);
    outer->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                               Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    outer->setSpacing(0);

    routeStack_ = new QStackedLayout();
    outer->addLayout(routeStack_);

    // --- 일반 뷰 ---
    auto* normalPage = new QWidget(card);
    auto* normalLayout = new QVBoxLayout(normalPage);
    normalLayout->setContentsMargins(0, 0, 0, 0);
    normalLayout->setSpacing(6);

    auto* titleRow = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("경로 · Trajectory"), normalPage);
    title->setProperty("role", "label");
    titleRow->addWidget(title);
    titleRow->addStretch();
    auto* newRouteBtn = new QPushButton(QStringLiteral("+ 새 경로"), normalPage);
    newRouteBtn->setObjectName("linkBtn");
    connect(newRouteBtn, &QPushButton::clicked, this, &P6PatrolPage::startNewRoute);
    titleRow->addWidget(newRouteBtn);

    auto* deleteRouteBtn =
        new QPushButton(SimpleIcons::trash(Tokens::TEXT_MUTED, 11), QString(), normalPage);
    deleteRouteBtn->setProperty("variant", "ghost");
    deleteRouteBtn->setFixedSize(22, 22);
    deleteRouteBtn->setIconSize(QSize(11, 11));
    deleteRouteBtn->setToolTip(QStringLiteral("현재 경로 삭제"));
    connect(deleteRouteBtn, &QPushButton::clicked, this, &P6PatrolPage::deleteCurrentRoute);
    titleRow->addWidget(deleteRouteBtn);
    normalLayout->addLayout(titleRow);

    routeChipsLayout_ = new QHBoxLayout();
    routeChipsLayout_->setSpacing(4);
    normalLayout->addLayout(routeChipsLayout_);

    routeOrderList_ = new QListWidget(normalPage);
    routeOrderList_->setObjectName("routeOrderList");
    routeOrderList_->setDragDropMode(QAbstractItemView::InternalMove);
    routeOrderList_->setFixedHeight(120);
    connect(routeOrderList_->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        QVector<QString> order;
        for (int i = 0; i < routeOrderList_->count(); ++i) {
            order.append(routeOrderList_->item(i)->data(Qt::UserRole).toString());
        }
        routes_[currentRoute_] = order;
        refreshCanvasPreview();
    });
    normalLayout->addWidget(routeOrderList_);

    routeStatsLabel_ = new QLabel(normalPage);
    routeStatsLabel_->setProperty("role", "muted");
    routeStatsLabel_->setStyleSheet(QStringLiteral("font-size:11px;"));
    routeStatsLabel_->setWordWrap(true);
    normalLayout->addWidget(routeStatsLabel_);

    auto* testBtn = new QPushButton(SimpleIcons::radar(Tokens::TEXT_SECONDARY, 11),
                                     QStringLiteral("테스트 실행 (1회 주행)"), normalPage);
    testBtn->setProperty("variant", "secondary");
    testBtn->setIconSize(QSize(11, 11));
    connect(testBtn, &QPushButton::clicked, this, &P6PatrolPage::runTestRun);
    normalLayout->addWidget(testBtn);

    routeStack_->addWidget(normalPage);

    // --- 편집 뷰 ---
    auto* editPage = new QWidget(card);
    auto* editLayout = new QVBoxLayout(editPage);
    editLayout->setContentsMargins(0, 0, 0, 0);
    editLayout->setSpacing(3);

    auto* nameLabel = new QLabel(QStringLiteral("경로 이름"), editPage);
    nameLabel->setProperty("role", "secondary");
    editLayout->addWidget(nameLabel);
    routeEditNameEdit_ = new QLineEdit(editPage);
    editLayout->addWidget(routeEditNameEdit_);
    editLayout->addSpacing(5);

    auto* orderLabel = new QLabel(QStringLiteral("순서 — 지도에서 순서대로 클릭"), editPage);
    orderLabel->setProperty("role", "secondary");
    editLayout->addWidget(orderLabel);

    routeEditListLayout_ = new QVBoxLayout();
    routeEditListLayout_->setSpacing(4);
    editLayout->addLayout(routeEditListLayout_);
    editLayout->addSpacing(6);

    auto* editBtnRow = new QHBoxLayout();
    auto* saveRouteBtn = new QPushButton(QStringLiteral("경로 저장"), editPage);
    saveRouteBtn->setProperty("variant", "primary");
    connect(saveRouteBtn, &QPushButton::clicked, this, [this] { exitRouteEditMode(true); });
    editBtnRow->addWidget(saveRouteBtn, /*stretch=*/1);
    auto* cancelRouteBtn = new QPushButton(QStringLiteral("취소"), editPage);
    cancelRouteBtn->setProperty("variant", "secondary");
    connect(cancelRouteBtn, &QPushButton::clicked, this, [this] { exitRouteEditMode(false); });
    editBtnRow->addWidget(cancelRouteBtn);
    editLayout->addLayout(editBtnRow);

    routeStack_->addWidget(editPage);
    routeStack_->setCurrentIndex(0);

    return card;
}

QWidget* P6PatrolPage::buildScheduleCard() {
    auto* card = new QFrame(this);
    card->setObjectName("card");
    auto* outer = new QVBoxLayout(card);
    outer->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                               Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    outer->setSpacing(6);

    auto* titleRow = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("순찰 스케줄"), card);
    title->setProperty("role", "label");
    titleRow->addWidget(title);
    titleRow->addStretch();
    auto* addBtn = new QPushButton(QStringLiteral("+ 추가"), card);
    addBtn->setObjectName("linkBtn");
    connect(addBtn, &QPushButton::clicked, this, [this] { toggleScheduleForm(true); });
    titleRow->addWidget(addBtn);
    outer->addLayout(titleRow);

    scheduleListLayout_ = new QVBoxLayout();
    scheduleListLayout_->setSpacing(0);
    outer->addLayout(scheduleListLayout_);

    scheduleForm_ = new QWidget(card);
    auto* formLayout = new QVBoxLayout(scheduleForm_);
    formLayout->setContentsMargins(0, 8, 0, 0);
    formLayout->setSpacing(3);

    auto* routeLabel = new QLabel(QStringLiteral("경로"), scheduleForm_);
    routeLabel->setProperty("role", "secondary");
    formLayout->addWidget(routeLabel);
    scheduleRouteCombo_ = new QComboBox(scheduleForm_);
    formLayout->addWidget(scheduleRouteCombo_);
    formLayout->addSpacing(5);

    auto* timeLabel = new QLabel(QStringLiteral("시각"), scheduleForm_);
    timeLabel->setProperty("role", "secondary");
    formLayout->addWidget(timeLabel);
    scheduleTimeEdit_ = new QTimeEdit(QTime(10, 0), scheduleForm_);
    scheduleTimeEdit_->setDisplayFormat(QStringLiteral("HH:mm"));
    scheduleTimeEdit_->setFixedWidth(76);
    formLayout->addWidget(scheduleTimeEdit_);
    formLayout->addSpacing(5);

    auto* dayLabel = new QLabel(QStringLiteral("요일"), scheduleForm_);
    dayLabel->setProperty("role", "secondary");
    formLayout->addWidget(dayLabel);
    auto* dayRow = new QHBoxLayout();
    dayRow->setSpacing(3);
    const QStringList dayNames = {QStringLiteral("월"), QStringLiteral("화"), QStringLiteral("수"),
                                   QStringLiteral("목"), QStringLiteral("금"), QStringLiteral("토"),
                                   QStringLiteral("일")};
    scheduleDayChips_.clear();
    for (int i = 0; i < dayNames.size(); ++i) {
        auto* chip = new QPushButton(dayNames[i], scheduleForm_);
        chip->setObjectName("dayChip");
        chip->setCheckable(true);
        chip->setChecked(i < 5);
        dayRow->addWidget(chip);
        scheduleDayChips_.append(chip);
    }
    formLayout->addLayout(dayRow);
    formLayout->addSpacing(8);

    auto* formBtnRow = new QHBoxLayout();
    auto* saveBtn = new QPushButton(QStringLiteral("저장"), scheduleForm_);
    saveBtn->setProperty("variant", "primary");
    connect(saveBtn, &QPushButton::clicked, this, &P6PatrolPage::addScheduleFromForm);
    formBtnRow->addWidget(saveBtn, /*stretch=*/1);
    auto* cancelBtn = new QPushButton(QStringLiteral("취소"), scheduleForm_);
    cancelBtn->setProperty("variant", "secondary");
    connect(cancelBtn, &QPushButton::clicked, this, [this] { toggleScheduleForm(false); });
    formBtnRow->addWidget(cancelBtn);
    formLayout->addLayout(formBtnRow);

    scheduleForm_->hide();
    outer->addWidget(scheduleForm_);

    return card;
}

QString P6PatrolPage::nextWaypointId() const {
    return QStringLiteral("WP_%1").arg(nextWpSeq_, 2, 10, QLatin1Char('0'));
}

QString P6PatrolPage::chargingWaypointId() const {
    for (const WaypointCanvas::Waypoint& w : waypoints_) {
        if (w.charging) return w.id;
    }
    return QString();
}

void P6PatrolPage::handleCanvasAddRequested(QPointF pos) {
    popover_->setForNewWaypoint(pos);
    const QPoint anchor = canvas_->mapTo(this, pos.toPoint());
    popover_->move(qMin(anchor.x() + 12, width() - popover_->width() - 8), qMax(anchor.y() - 20, 0));
    popover_->raise();
    popover_->show();
}

void P6PatrolPage::handleCanvasEditRequested(const QString& id) {
    const WaypointCanvas::Waypoint* wp = canvas_->findWaypoint(id);
    if (!wp) return;
    popover_->setForEditWaypoint(*wp);
    const QPoint anchor = canvas_->mapTo(this, wp->pos.toPoint());
    popover_->move(qMin(anchor.x() + 12, width() - popover_->width() - 8), qMax(anchor.y() - 20, 0));
    popover_->raise();
    popover_->show();
}

void P6PatrolPage::handleCanvasDeleteRequested(const QString& id) {
    popover_->hide();
    for (int i = 0; i < waypoints_.size(); ++i) {
        if (waypoints_[i].id == id) {
            waypoints_.remove(i);
            break;
        }
    }
    for (auto it = routes_.begin(); it != routes_.end(); ++it) it.value().removeAll(id);
    routeEditSelection_.removeAll(id);

    canvas_->setWaypoints(waypoints_);
    canvas_->setRouteEditSelection(routeEditSelection_);
    rebuildWpList();
    rebuildRouteOrderList();
    if (routeEditMode_) rebuildRouteEditList();
    refreshCanvasPreview();
}

void P6PatrolPage::handleCanvasMoved(const QString& id, QPointF pos) {
    for (WaypointCanvas::Waypoint& w : waypoints_) {
        if (w.id == id) {
            w.pos = pos;
            break;
        }
    }
    rebuildWpList();
}

void P6PatrolPage::handlePopoverSave(const QString& id, const QString& name, bool charging, qreal yaw,
                                      QPointF pos) {
    if (charging) {
        const QString existing = chargingWaypointId();
        if (!existing.isEmpty() && existing != id) {
            const auto reply = QMessageBox::question(
                this, QStringLiteral("충전 스테이션 교체"),
                QStringLiteral("이미 다른 충전 스테이션(%1)이 있습니다. 이 웨이포인트로 교체할까요? "
                                "(맵당 1개만 허용)")
                    .arg(existing),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
            for (WaypointCanvas::Waypoint& w : waypoints_) {
                if (w.id == existing) w.charging = false;
            }
        }
    }

    if (id.isEmpty()) {
        WaypointCanvas::Waypoint wp{nextWaypointId(), name, charging, pos, yaw};
        ++nextWpSeq_;
        waypoints_.append(wp);
    } else {
        for (WaypointCanvas::Waypoint& w : waypoints_) {
            if (w.id == id) {
                w.name = name;
                w.charging = charging;
                w.yawDeg = yaw;
                break;
            }
        }
    }

    canvas_->setWaypoints(waypoints_);
    rebuildWpList();
    refreshCanvasPreview();
}

void P6PatrolPage::rebuildWpList() {
    QLayoutItem* item;
    while ((item = wpListLayout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("웨이포인트 %1").arg(waypoints_.size()), wpListCard_);
    title->setProperty("role", "label");
    header->addWidget(title);
    header->addStretch();
    auto* typeHint = new QLabel(QStringLiteral("유형: 순찰 지점 · 충전"), wpListCard_);
    typeHint->setProperty("role", "muted");
    typeHint->setStyleSheet(QStringLiteral("font-size:11px;"));
    header->addWidget(typeHint);
    wpListLayout_->addLayout(header);
    wpListLayout_->addSpacing(4);

    auto* listFrame = new QFrame(wpListCard_);
    listFrame->setStyleSheet(
        QStringLiteral("border:1px solid #E3E8ED; border-radius:8px; background:#FFFFFF;"));
    auto* listLayout = new QVBoxLayout(listFrame);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);

    for (int i = 0; i < waypoints_.size(); ++i) {
        const WaypointCanvas::Waypoint& w = waypoints_[i];
        auto* row = new QFrame(listFrame);
        if (i + 1 < waypoints_.size()) {
            row->setStyleSheet(QStringLiteral("border-bottom:1px solid #E3E8ED;"));
        }
        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(8, 5, 8, 5);
        rowLayout->setSpacing(1);

        auto* topRow = new QHBoxLayout();
        topRow->setSpacing(6);

        auto* idLabel = new QLabel(w.id, row);
        idLabel->setProperty("role", "mono");
        idLabel->setStyleSheet(QStringLiteral("color:#5A6774; font-size:11px;"));
        topRow->addWidget(idLabel);

        auto* nameLabel = new QLabel(w.name, row);
        nameLabel->setStyleSheet(QStringLiteral("font-size:11px;"));
        topRow->addWidget(nameLabel);
        topRow->addStretch();

        if (w.charging) {
            auto* badge = new QLabel(QStringLiteral("초기 위치"), row);
            badge->setStyleSheet(
                QStringLiteral("color:#0B7A6E; background:#E0F5F2; font-size:10px; padding:1px 5px; "
                                "border-radius:3px;"));
            topRow->addWidget(badge);
        }

        const QString id = w.id;
        auto* editBtn = new QPushButton(SimpleIcons::chevronRight(Tokens::TEXT_MUTED, 10), QString(), row);
        editBtn->setProperty("variant", "ghost");
        editBtn->setFixedSize(20, 20);
        editBtn->setIconSize(QSize(10, 10));
        editBtn->setToolTip(QStringLiteral("웨이포인트 편집"));
        connect(editBtn, &QPushButton::clicked, this, [this, id] { handleCanvasEditRequested(id); });
        topRow->addWidget(editBtn);

        if (!w.charging) {
            auto* driveBtn = new QPushButton(SimpleIcons::play(Tokens::ACCENT, 9), QString(), row);
            driveBtn->setProperty("variant", "ghost");
            driveBtn->setFixedSize(20, 20);
            driveBtn->setIconSize(QSize(9, 9));
            driveBtn->setToolTip(QStringLiteral("이 지점으로만 주행"));
            connect(driveBtn, &QPushButton::clicked, this, [this, id] { driveToSingleWaypoint(id); });
            topRow->addWidget(driveBtn);
        }

        rowLayout->addLayout(topRow);

        auto* coordLabel = new QLabel(
            QStringLiteral("x %1  y %2  yaw %3°")
                .arg(w.pos.x() / kPxPerMeter, 0, 'f', 1)
                .arg(w.pos.y() / kPxPerMeter, 0, 'f', 1)
                .arg(static_cast<int>(w.yawDeg)),
            row);
        coordLabel->setProperty("role", "mono");
        coordLabel->setStyleSheet(QStringLiteral("color:#93A0AC; font-size:10px;"));
        rowLayout->addWidget(coordLabel);

        listLayout->addWidget(row);
    }

    wpListLayout_->addWidget(listFrame);
}

void P6PatrolPage::rebuildRouteChips() {
    QLayoutItem* item;
    while ((item = routeChipsLayout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    for (auto it = routes_.constBegin(); it != routes_.constEnd(); ++it) {
        const QString name = it.key();
        auto* chip = new QPushButton(name, this);
        chip->setObjectName("routeChip");
        chip->setCheckable(true);
        chip->setChecked(name == currentRoute_);
        connect(chip, &QPushButton::clicked, this, [this, name] {
            currentRoute_ = name;
            rebuildRouteChips();
            rebuildRouteOrderList();
            refreshCanvasPreview();
        });
        routeChipsLayout_->addWidget(chip);
    }
    routeChipsLayout_->addStretch();
}

void P6PatrolPage::rebuildRouteOrderList() {
    routeOrderList_->clear();

    if (currentRoute_.isEmpty()) {
        routeStatsLabel_->setText(QStringLiteral("경로가 없습니다 — \"+ 새 경로\"로 만들어보세요."));
        return;
    }

    for (const QString& id : routes_.value(currentRoute_)) {
        const WaypointCanvas::Waypoint* wp = canvas_->findWaypoint(id);
        const QString text = wp ? QStringLiteral("%1 %2").arg(id, wp->name) : id;
        auto* listItem = new QListWidgetItem(text, routeOrderList_);
        listItem->setData(Qt::UserRole, id);
    }

    qreal distM = 0.0;
    const QString chargeId = chargingWaypointId();
    QVector<QPointF> pts;
    if (const WaypointCanvas::Waypoint* c = canvas_->findWaypoint(chargeId)) pts.append(c->pos);
    for (const QString& id : routes_.value(currentRoute_)) {
        if (const WaypointCanvas::Waypoint* w = canvas_->findWaypoint(id)) pts.append(w->pos);
    }
    if (!pts.isEmpty() && !chargeId.isEmpty()) {
        if (const WaypointCanvas::Waypoint* c = canvas_->findWaypoint(chargeId)) pts.append(c->pos);
    }
    for (int i = 0; i + 1 < pts.size(); ++i) {
        distM += std::hypot(pts[i + 1].x() - pts[i].x(), pts[i + 1].y() - pts[i].y()) / kPxPerMeter;
    }
    const int etaMin = qMax(1, static_cast<int>(std::round(distM / 0.3 / 60.0)));
    routeStatsLabel_->setText(
        QStringLiteral("예상 이동 %1 m · 약 %2분 · 복약 미션도 이 경로로 주행").arg(distM, 0, 'f', 1).arg(etaMin));
}

void P6PatrolPage::rebuildRouteEditList() {
    QLayoutItem* item;
    while ((item = routeEditListLayout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    for (int i = 0; i < routeEditSelection_.size(); ++i) {
        const QString id = routeEditSelection_[i];
        const WaypointCanvas::Waypoint* wp = canvas_->findWaypoint(id);
        auto* row = new QFrame(this);
        row->setStyleSheet(QStringLiteral("border:1px solid #E3E8ED; border-radius:6px; background:#fff;"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 4, 8, 4);
        rowLayout->setSpacing(6);

        auto* idxLabel = new QLabel(QString::number(i + 1), row);
        idxLabel->setStyleSheet(QStringLiteral("color:#2563A8; font-weight:500; font-size:11px;"));
        rowLayout->addWidget(idxLabel);
        auto* nameLabel = new QLabel(wp ? wp->name : id, row);
        nameLabel->setStyleSheet(QStringLiteral("font-size:11px;"));
        rowLayout->addWidget(nameLabel, 1);

        auto* removeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_MUTED, 10), QString(), row);
        removeBtn->setProperty("variant", "ghost");
        removeBtn->setFixedSize(18, 18);
        removeBtn->setIconSize(QSize(10, 10));
        connect(removeBtn, &QPushButton::clicked, this, [this, id] { handleRouteWaypointClicked(id); });
        rowLayout->addWidget(removeBtn);

        routeEditListLayout_->addWidget(row);
    }
}

void P6PatrolPage::rebuildScheduleList() {
    QLayoutItem* item;
    while ((item = scheduleListLayout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    scheduleRouteCombo_->clear();
    scheduleRouteCombo_->addItems(routes_.keys());

    const QStringList dayNames = {QStringLiteral("월"), QStringLiteral("화"), QStringLiteral("수"),
                                   QStringLiteral("목"), QStringLiteral("금"), QStringLiteral("토"),
                                   QStringLiteral("일")};

    auto* listFrame = new QFrame(this);
    listFrame->setStyleSheet(
        QStringLiteral("border:1px solid #E3E8ED; border-radius:8px; background:#FFFFFF;"));
    auto* listLayout = new QVBoxLayout(listFrame);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);

    for (int i = 0; i < schedules_.size(); ++i) {
        Schedule& sched = schedules_[i];
        auto* row = new QFrame(listFrame);
        if (i + 1 < schedules_.size()) row->setStyleSheet(QStringLiteral("border-bottom:1px solid #E3E8ED;"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 6, 8, 6);
        rowLayout->setSpacing(6);

        auto* nameLabel = new QLabel(sched.routeName, row);
        nameLabel->setStyleSheet(QStringLiteral("font-size:11px;"));
        rowLayout->addWidget(nameLabel);
        auto* timeLabel = new QLabel(sched.timeText, row);
        timeLabel->setProperty("role", "mono");
        timeLabel->setStyleSheet(QStringLiteral("color:#5A6774; font-size:11px;"));
        rowLayout->addWidget(timeLabel);

        QString daysText = QStringLiteral("매일");
        if (sched.days.size() != 7) {
            QStringList names;
            for (int d : sched.days) names << dayNames.value(d);
            daysText = names.join(QStringLiteral(","));
        }
        auto* daysLabel = new QLabel(daysText, row);
        daysLabel->setProperty("role", "muted");
        daysLabel->setStyleSheet(QStringLiteral("font-size:11px;"));
        rowLayout->addWidget(daysLabel);
        rowLayout->addStretch();

        auto* toggle = new ToggleSwitch(row);
        toggle->setChecked(sched.enabled);
        connect(toggle, &ToggleSwitch::toggled, this, [this, i](bool on) { schedules_[i].enabled = on; });
        rowLayout->addWidget(toggle);

        listLayout->addWidget(row);
    }

    scheduleListLayout_->addWidget(listFrame);
}

void P6PatrolPage::refreshCanvasPreview() {
    // 충전 스테이션(초기 위치) → 경로 순서 → 충전 복귀까지 전 구간에 선을 그리도록
    // 양 끝에 충전 스테이션을 붙인다 (안 붙이면 초기위치→첫 웨이포인트 구간이 비어 보인다).
    const QString chargeId = chargingWaypointId();
    QVector<QString> full;
    if (!chargeId.isEmpty()) full.append(chargeId);
    full += routes_.value(currentRoute_);
    if (!chargeId.isEmpty() && !routes_.value(currentRoute_).isEmpty()) full.append(chargeId);
    canvas_->setRoutePreview(full);
}

void P6PatrolPage::enterRouteEditMode(const QString& routeName) {
    routeEditMode_ = true;
    routeEditTargetName_ = routeName;
    routeEditSelection_ = routes_.value(routeName);
    routeEditNameEdit_->setText(routeName);
    canvas_->setRouteEditMode(true);
    canvas_->setRouteEditSelection(routeEditSelection_);
    routeStack_->setCurrentIndex(1);
    rebuildRouteEditList();
}

void P6PatrolPage::exitRouteEditMode(bool save) {
    if (save) {
        const QString name = routeEditNameEdit_->text().trimmed();
        if (name.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("경로 이름 필요"), QStringLiteral("경로 이름을 입력하세요."));
            return;
        }
        if (!routeEditTargetName_.isEmpty() && routeEditTargetName_ != name) {
            routes_.remove(routeEditTargetName_);
        }
        routes_[name] = routeEditSelection_;
        currentRoute_ = name;
    }

    routeEditMode_ = false;
    canvas_->setRouteEditMode(false);
    routeStack_->setCurrentIndex(0);

    rebuildRouteChips();
    rebuildRouteOrderList();
    refreshCanvasPreview();
}

void P6PatrolPage::handleRouteWaypointClicked(const QString& id) {
    if (routeEditSelection_.contains(id)) {
        routeEditSelection_.removeAll(id);
    } else {
        routeEditSelection_.append(id);
    }
    canvas_->setRouteEditSelection(routeEditSelection_);
    rebuildRouteEditList();
}

void P6PatrolPage::startNewRoute() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("새 경로"), QStringLiteral("경로 이름"),
                                                 QLineEdit::Normal, QStringLiteral("새 경로"), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    enterRouteEditMode(name.trimmed());
}

void P6PatrolPage::deleteCurrentRoute() {
    if (currentRoute_.isEmpty() || !routes_.contains(currentRoute_)) return;

    const auto reply = QMessageBox::question(
        this, QStringLiteral("경로 삭제"),
        QStringLiteral("'%1' 경로를 삭제할까요? 이 경로를 쓰는 순찰 스케줄도 함께 삭제됩니다.")
            .arg(currentRoute_),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    routes_.remove(currentRoute_);
    for (int i = schedules_.size() - 1; i >= 0; --i) {
        if (schedules_[i].routeName == currentRoute_) schedules_.remove(i);
    }

    currentRoute_ = routes_.isEmpty() ? QString() : routes_.keys().first();

    rebuildRouteChips();
    rebuildRouteOrderList();
    rebuildScheduleList();
    refreshCanvasPreview();
}

void P6PatrolPage::runTestRun() {
    const QVector<QString> order = routes_.value(currentRoute_);
    const QString chargeId = chargingWaypointId();
    if (currentRoute_.isEmpty() || order.isEmpty() || chargeId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("테스트 실행"),
                                  QStringLiteral("경로에 웨이포인트가 없거나 충전 스테이션이 없습니다."));
        return;
    }
    canvas_->startTestRun(order, chargeId);
}

void P6PatrolPage::driveToSingleWaypoint(const QString& id) {
    const QString chargeId = chargingWaypointId();
    if (chargeId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("주행 불가"), QStringLiteral("충전 스테이션이 없습니다."));
        return;
    }
    // 단일 웨이포인트 주행은 도착 후 복귀하지 않고 그 자리에 머문다.
    canvas_->startTestRun({id}, chargeId, /*returnToCharging=*/false);
}

void P6PatrolPage::toggleScheduleForm(bool show) {
    if (show) {
        scheduleRouteCombo_->clear();
        scheduleRouteCombo_->addItems(routes_.keys());
        scheduleTimeEdit_->setTime(QTime(10, 0));
        for (int i = 0; i < scheduleDayChips_.size(); ++i) scheduleDayChips_[i]->setChecked(i < 5);
    }
    scheduleForm_->setVisible(show);
}

void P6PatrolPage::addScheduleFromForm() {
    Schedule sched;
    sched.routeName = scheduleRouteCombo_->currentText();
    sched.timeText = scheduleTimeEdit_->time().toString(QStringLiteral("HH:mm"));
    sched.enabled = true;
    for (int i = 0; i < scheduleDayChips_.size(); ++i) {
        if (scheduleDayChips_[i]->isChecked()) sched.days.append(i);
    }
    if (sched.routeName.isEmpty() || sched.days.isEmpty()) return;

    schedules_.append(sched);
    rebuildScheduleList();
    toggleScheduleForm(false);
}

void P6PatrolPage::saveCurrentMapData() {
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    MapData& m = maps_[currentMapIndex_];
    m.waypoints = waypoints_;
    m.routes = routes_;
    m.currentRoute = currentRoute_;
    m.nextWpSeq = nextWpSeq_;
    m.schedules = schedules_;
}

void P6PatrolPage::loadMapData(int index) {
    if (index < 0 || index >= maps_.size()) return;
    MapData& m = maps_[index];
    currentMapIndex_ = index;
    waypoints_ = m.waypoints;
    routes_ = m.routes;
    currentRoute_ = m.currentRoute;
    nextWpSeq_ = m.nextWpSeq;
    schedules_ = m.schedules;

    mapChipBtn_->setText(
        QStringLiteral("  %1 (%2 · %3)  ▾").arg(m.mapName, m.robotId, m.recipientName));

    canvas_->resetView();
    canvas_->setBackgroundImage(m.backgroundImage);
    canvas_->setWaypoints(waypoints_);

    // 실제 맵 이미지가 있으면 충전 스테이션(=초기/현재 위치)이 벽 위에 있지 않은지
    // 확인하고, 벽 위라면 가장 가까운 자유 공간으로 옮긴다 (좌표가 실제 지도와
    // 무관하게 임의로 정해졌을 수 있어서 — 특히 새로 등록한 맵의 기본 위치).
    const QString chargeId = chargingWaypointId();
    if (!m.backgroundImage.isNull() && !chargeId.isEmpty()) {
        for (WaypointCanvas::Waypoint& w : waypoints_) {
            if (w.id != chargeId) continue;
            const QPointF freePos = canvas_->nearestFreePoint(w.pos);
            if (freePos != w.pos) {
                w.pos = freePos;
                canvas_->setWaypoints(waypoints_);
            }
            break;
        }
        m.waypoints = waypoints_;
    }

    if (const WaypointCanvas::Waypoint* c = canvas_->findWaypoint(chargeId)) {
        canvas_->setCurrentRobotPosition(c->pos, 0.0);
    }
    rebuildWpList();
    rebuildRouteChips();
    rebuildRouteOrderList();
    rebuildScheduleList();
    refreshCanvasPreview();
}

void P6PatrolPage::switchToMap(int index) {
    if (index == currentMapIndex_) return;

    if (routeEditMode_) exitRouteEditMode(false);
    popover_->hide();

    saveCurrentMapData();
    loadMapData(index);
}

void P6PatrolPage::rebuildMapMenu() {
    mapMenu_->clear();
    for (int i = 0; i < maps_.size(); ++i) {
        const MapData& m = maps_[i];
        auto* action = mapMenu_->addAction(
            QStringLiteral("%1 (%2 · %3)").arg(m.mapName, m.robotId, m.recipientName));
        connect(action, &QAction::triggered, this, [this, i] { switchToMap(i); });
    }
}

void P6PatrolPage::upsertMap(const QImage& mapImage, const QString& mapName, const QString& robotId,
                              const QString& recipientName) {
    int foundIndex = -1;
    for (int i = 0; i < maps_.size(); ++i) {
        if (maps_[i].robotId == robotId) {
            foundIndex = i;
            break;
        }
    }

    if (foundIndex < 0) {
        MapData m;
        m.mapName = mapName;
        m.robotId = robotId;
        m.recipientName = recipientName;
        m.backgroundImage = mapImage;
        m.waypoints = {
            {QStringLiteral("WP_01"), QStringLiteral("충전 스테이션"), true, QPointF(90, 260), 0}};
        m.nextWpSeq = 2;
        maps_.append(m);
        foundIndex = maps_.size() - 1;
    } else {
        // 같은 로봇에 이미 있던 맵 — 이름·대상자·배경 이미지만 갱신하고 웨이포인트/경로/
        // 스케줄은 그대로 둔다 (사용자가 P6에서 미리 만들어둔 것을 지우지 않기 위해).
        maps_[foundIndex].mapName = mapName;
        maps_[foundIndex].recipientName = recipientName;
        maps_[foundIndex].backgroundImage = mapImage;
    }

    rebuildMapMenu();

    if (foundIndex == currentMapIndex_) {
        loadMapData(currentMapIndex_);
    }
}
