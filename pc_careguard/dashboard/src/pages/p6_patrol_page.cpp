#include "p6_patrol_page.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QAction>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedLayout>
#include <QStyle>
#include <QTimeEdit>
#include <QVBoxLayout>
#include <QtMath>
#include <cmath>

#include <QJsonArray>
#include <QJsonObject>

#include "net/ws_client.h"
#include "theme/tokens.h"
#include "widgets/canvas_geometry.h"
#include "widgets/pgm_image_loader.h"
#include "widgets/simple_icons.h"
#include "widgets/teleop_panel.h"
#include "widgets/toggle_switch.h"
#include "widgets/waypoint_popover.h"

namespace {
// designMapMetersToCanvas()의 폴백 전용 — 서버 map.resolution/origin_x/origin_y를 아직
// 못 받은 맵(로컬 데모 맵 등)에서만 쓰인다. 실제 지도가 있는 맵은 canvas_->
// canvasToMapMeters()/mapMetersToCanvas()가 그 맵의 진짜 resolution/origin으로 정확히
// 환산한다 — 예전엔 이 고정 배율(40px=1m)을 모든 맵에 무조건 썼는데, 실제 지도
// 해상도(대개 0.05m/px=20px/m)와 달라 "이동" 목표가 화면에서 클릭한 지점과 실제
// 로봇이 가는 곳이 어긋나는 원인이었다(2026-09-08 실 로봇 테스트로 확인).
constexpr qreal kPxPerMeter = 40.0;

// "복약 전달 경로" 체크박스가 켜진 경로의 patrol_route.name 리터럴. "주행 스케줄"
// 추가 폼이 경로를 이 이름과 비교해 복약 스케줄 콤보를 보일지 판단한다(요청 사양).
const QString kMedicineRouteName = QStringLiteral("medicine");
}

P6PatrolPage::P6PatrolPage(QWidget* parent) : QWidget(parent), currentMapIndex_(0), routeEditMode_(false) {
    // P5MapPage와 같은 이유 — teleopPanel_의 방향키(↑↓←→)/Space를 이 페이지가
    // 받아서 전달하려면 포커스가 필요하다.
    setFocusPolicy(Qt::StrongFocus);

    // maps_는 여기서 더미로 채우지 않는다 — 생성자 끝의 loadMapsFromServer()가
    // GET /maps/active로 실제 서버 데이터를 받아와야만 채워진다. DB에 맵이 없으면
    // 이 페이지는 그대로 빈 상태로 남는다(가짜 데모 맵으로 대체하지 않는다).

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    layout->addWidget(buildHeaderRow());

    connectionBanner_ = buildConnectionBanner();
    layout->addWidget(connectionBanner_);

    // loadMapData(0)이 아래에서 곧바로 호출되므로(그 안에서 robotOfflineBanner_를
    // 건드린다) wsClient_/apiClient_보다 먼저, 여기서 만들어둔다.
    robotOfflineBanner_ = buildRobotOfflineBanner();
    layout->addWidget(robotOfflineBanner_);

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
    connect(canvas_, &WaypointCanvas::initialPoseCaptureFinished, this, &P6PatrolPage::handleInitialPoseCaptured);
    connect(canvas_, &WaypointCanvas::initialPoseCaptureCancelled, this,
            [this] { poseCaptureBtn_->setChecked(false); });
    connect(canvas_, &WaypointCanvas::routeWaypointClicked, this, &P6PatrolPage::handleRouteWaypointClicked);
    connect(canvas_, &WaypointCanvas::chargingClickBlocked, this, [this] {
        QMessageBox::information(this, QStringLiteral("충전 스테이션"),
                                  QStringLiteral("충전 스테이션은 경로 순서에 포함할 수 없습니다 (자동 출발·복귀)."));
    });
    // 창 크기 변경으로 캔버스가 웨이포인트 좌표를 다시 배치했다 — 이 페이지가 들고
    // 있는 사본과 현재 맵 데이터도 같이 갱신해야 다음 편집에서 되돌아가지 않는다.
    connect(canvas_, &WaypointCanvas::viewRescaled, this, [this] {
        waypoints_ = canvas_->waypoints();
        // 로봇 "현재 위치"는 캔버스 내부에서도 같은 변환으로 리매핑되지만, 페이지가 아직
        // 숨겨진 상태(QStackedWidget에서 P6이 current가 아닐 때)에서 loadMapData()가
        // setCurrentRobotPosition을 호출하면 그 시점엔 캔버스 실제 크기를 몰라 값이
        // 어긋날 수 있다 — 방금 새로 갱신된 웨이포인트 좌표를 기준으로 다시 한 번
        // 확실하게 맞춰준다.
        const QString chargeId = chargingWaypointId();
        if (const WaypointCanvas::Waypoint* c = canvas_->findWaypoint(chargeId)) {
            canvas_->setCurrentRobotPositionToChargingStation(c->pos);
        }
        saveCurrentMapData();
        rebuildWpList();
        popover_->hide();
    });

    loadMapData(0);

    apiClient_ = new ApiClient(this);
    loadMapsFromServer();

    wsClient_ = new WsClient(this);
    connect(wsClient_, &WsClient::messageReceived, this, &P6PatrolPage::handleWsMessage);
    connect(wsClient_, &WsClient::connectionStateChanged, this,
            [this](bool connected) { connectionBanner_->setVisible(!connected); });
    wsClient_->connectToServer();
}

QFrame* P6PatrolPage::buildConnectionBanner() {
    // P5MapPage::buildConnectionBanner()와 같은 이유·같은 패턴 — 이 페이지도 별도
    // WsClient 연결을 쓰므로(P5와 독립) 끊김 표시도 각자 들고 있어야 한다.
    auto* banner = new QFrame(this);
    banner->setObjectName("banner");
    banner->setProperty("level", "attention");
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(10, 7, 10, 7);
    bannerLayout->setSpacing(6);
    auto* icon = new QLabel(banner);
    icon->setPixmap(SimpleIcons::plugOff(Tokens::ATTENTION.main, 12).pixmap(12, 12));
    bannerLayout->addWidget(icon);
    auto* text = new QLabel(QStringLiteral("서버 연결 끊김 · 재연결 시도 중…"), banner);
    text->setStyleSheet(QStringLiteral("font-weight:500;"));
    bannerLayout->addWidget(text);
    bannerLayout->addStretch();
    banner->hide();
    return banner;
}

QFrame* P6PatrolPage::buildRobotOfflineBanner() {
    // P5MapPage::buildRobotOfflineBanner()와 같은 이유·같은 패턴 — "이동" 명령이 실제로
    // 로봇에 안 먹히는데 사용자가 그걸 모른 채 계속 시도하는 걸 막는다. 아직 아무
    // robot_status도 못 받은 시작 시점엔 안전하게 오프라인으로 간주해 보이는 채로 둔다
    // (hide() 안 함) — handleWsMessage가 online:true를 받으면 숨긴다.
    auto* banner = new QFrame(this);
    banner->setObjectName("banner");
    banner->setProperty("level", "emergency");
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(10, 7, 10, 7);
    bannerLayout->setSpacing(6);
    auto* icon = new QLabel(banner);
    icon->setPixmap(SimpleIcons::alertCircle(Tokens::EMERGENCY.main, 12).pixmap(12, 12));
    bannerLayout->addWidget(icon);
    auto* text =
        new QLabel(QStringLiteral("로봇 오프라인 · 이동·순찰 명령이 전달되지 않습니다"), banner);
    text->setStyleSheet(QStringLiteral("font-weight:500;"));
    bannerLayout->addWidget(text);
    bannerLayout->addStretch();
    return banner;
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
    // 버튼에 딱 붙어 보이지 않도록 살짝 간격을 띄워서 연다.
    connect(mapMenu_, &QMenu::aboutToShow, this, [this] { mapMenu_->move(mapMenu_->pos() + QPoint(0, 6)); });
    rebuildMapMenu();
    layout->addWidget(mapChipBtn_);
    layout->addStretch();

    auto* estopBtn = new QPushButton(QStringLiteral("비상정지"), row);
    estopBtn->setProperty("variant", "danger");
    estopBtn->setProperty("size", "l");
    connect(estopBtn, &QPushButton::clicked, this, &P6PatrolPage::handleEmergencyStop);
    layout->addWidget(estopBtn);

    auto* shutdownBtn = new QPushButton(QStringLiteral("로봇 종료"), row);
    shutdownBtn->setProperty("variant", "danger");
    shutdownBtn->setToolTip(
        QStringLiteral("이 로봇에서 켜져 있는 모든 것(베이스 구동·Nav2/SLAM·rviz2·업링크·다운링크)을\n"
                        "한 번에 끕니다. 순찰/이동 중이면 먼저 안전하게 멈춘 뒤 끕니다."));
    connect(shutdownBtn, &QPushButton::clicked, this, &P6PatrolPage::shutdownCurrentRobot);
    layout->addWidget(shutdownBtn);

    return row;
}

QWidget* P6PatrolPage::buildCanvasCard() {
    auto* card = new QFrame(this);
    card->setObjectName("card");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    layout->setSpacing(8);

    canvasToolbarRow_ = new QWidget(card);
    auto* toolbar = new QHBoxLayout(canvasToolbarRow_);
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(6);

    auto* addModeBtn = new QPushButton(QStringLiteral("WP 편집"), canvasToolbarRow_);
    auto* moveModeBtn = new QPushButton(QStringLiteral("로봇 이동"), canvasToolbarRow_);
    for (auto* btn : {addModeBtn, moveModeBtn}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
    }
    addModeBtn->setProperty("pos", "first");
    moveModeBtn->setProperty("pos", "last");
    // WaypointCanvas의 기본 모드(mode_)를 MoveWaypoint로 바꿔서, 여기 버튼 체크
    // 상태도 그에 맞춰 "이동"을 기본으로 보여준다.
    moveModeBtn->setChecked(true);
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

    // "위치 보정" — WP 추가/이동과 배타적인 세그먼트 그룹에는 넣지 않는다(그 둘은 계속
    // 유지되는 모드지만, 이건 드래그 한 번으로 끝나는 단발성 동작이라 캔버스가 알아서
    // 끝나면 poseCaptureMode를 꺼버린다 — initialPoseCaptureFinished/Cancelled에서 이
    // 버튼 체크 상태만 되돌려주면 된다).
    poseCaptureBtn_ = new QPushButton(QStringLiteral("위치 보정"), canvasToolbarRow_);
    poseCaptureBtn_->setObjectName("segBtn");
    poseCaptureBtn_->setProperty("pos", "only");
    poseCaptureBtn_->setCheckable(true);
    poseCaptureBtn_->setToolTip(
        QStringLiteral("로봇이 실제로 있는 위치를 지도에서 클릭한 채 드래그해 방향까지 지정하면,\n"
                        "그 자리로 AMCL 초기 위치를 다시 잡습니다(충전 스테이션 자동 추정이 틀렸을 때)."));
    toolbar->addWidget(poseCaptureBtn_);
    connect(poseCaptureBtn_, &QPushButton::clicked, this, [this](bool checked) {
        canvas_->setPoseCaptureMode(checked);
    });

    auto* hint = new QLabel(
        QStringLiteral("휠 확대/축소 · 휠 버튼 드래그로 이동 · 더블클릭 원상복귀 — 그 외 모드별 안내는 지도 하단 참고"),
        canvasToolbarRow_);
    hint->setProperty("role", "muted");
    hint->setStyleSheet(QStringLiteral("font-size:11px;"));
    toolbar->addWidget(hint);
    toolbar->addStretch();
    layout->addWidget(canvasToolbarRow_);

    // 경로 편집 모드에선 위 툴바(WP 추가/이동은 편집 중 의미가 없다) 대신 이 배너로
    // 바꿔 보여준다 — p6_route_edit_schedule_wireframe.html의 파란 안내 바.
    routeEditBanner_ = new QFrame(card);
    routeEditBanner_->setObjectName("banner");
    routeEditBanner_->setProperty("level", "editMode");
    auto* bannerLayout = new QHBoxLayout(routeEditBanner_);
    bannerLayout->setContentsMargins(10, 7, 10, 7);
    bannerLayout->setSpacing(6);
    auto* bannerIcon = new QLabel(routeEditBanner_);
    bannerIcon->setPixmap(SimpleIcons::route(Tokens::INFO.main, 12).pixmap(12, 12));
    bannerLayout->addWidget(bannerIcon);
    auto* bannerText = new QLabel(QStringLiteral("경로 편집 중 — 웨이포인트를 순서대로 클릭하세요"), routeEditBanner_);
    bannerText->setStyleSheet(QStringLiteral("font-weight:500;"));
    bannerLayout->addWidget(bannerText);
    bannerLayout->addStretch();
    auto* bannerHint = new QLabel(QStringLiteral("재클릭 = 제거"), routeEditBanner_);
    bannerHint->setStyleSheet(QStringLiteral("color:#5A6774; font-size:11px;"));
    bannerLayout->addWidget(bannerHint);
    routeEditBanner_->hide();
    layout->addWidget(routeEditBanner_);

    canvas_ = new WaypointCanvas(card);
    layout->addWidget(canvas_, /*stretch=*/1);

    return card;
}

QWidget* P6PatrolPage::buildRightColumn() {
    auto* wrap = new QWidget();
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    teleopPanel_ = new TeleopPanel(wrap);
    connect(teleopPanel_, &TeleopPanel::velocityChanged, this, [this](qreal linearMps, qreal angularRadps) {
        // P5MapPage와 같은 경로(POST /teleop/{robot_code} -> robot_ws ->
        // downlink_node -> /cmd_vel) — 다만 P6은 로봇이 고정이 아니라 지금 보고
        // 있는 맵의 로봇으로 매번 새로 조회해서 보낸다(맵 전환 중 방향키를 누른
        // 경우에도 항상 지금 화면의 로봇에게 나가도록).
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        apiClient_->sendTeleop(maps_[currentMapIndex_].robotId, linearMps, angularRadps);
    });
    layout->addWidget(teleopPanel_);

    layout->addWidget(buildWpListCard());
    layout->addWidget(buildRouteCard());
    layout->addWidget(buildScheduleCard());
    layout->addStretch();

    // 원격 조작 패널이 추가되면서 오른쪽 칼럼 콘텐츠(패널+WP목록+경로+스케줄) 총
    // 세로 높이가 창 높이를 넘어설 수 있게 됐다 — 스크롤 없이 그대로 두면 QVBoxLayout이
    // 모든 카드를 실제 크기보다 눌러서 찌그러뜨린다("방향키 패드가 완전 납작해"
    // 리포트, TeleopPanel의 setFixedHeight(32) 버튼조차 그 안에서 뭉개져 보였다).
    // 넘치는 만큼 스크롤되게 해서 각 카드가 항상 자기 실제 크기를 유지하게 한다.
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("patrolRightColumnScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedWidth(236);
    scroll->setWidget(wrap);
    return scroll;
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
                                     QStringLiteral("순찰 시작"), normalPage);
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

    routeMedicineCheckbox_ = new QCheckBox(QStringLiteral("복약 전달 경로"), editPage);
    connect(routeMedicineCheckbox_, &QCheckBox::toggled, this, &P6PatrolPage::onRouteMedicineToggled);
    editLayout->addWidget(routeMedicineCheckbox_);
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
    auto* title = new QLabel(QStringLiteral("주행 스케줄"), card);
    title->setProperty("role", "label");
    titleRow->addWidget(title);
    titleRow->addStretch();
    auto* addBtn = new QPushButton(QStringLiteral("+ 추가"), card);
    addBtn->setObjectName("linkBtn");
    connect(addBtn, &QPushButton::clicked, this, [this] {
        scheduleEditIndex_ = -1;
        toggleScheduleForm(true);
    });
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
    connect(scheduleRouteCombo_, &QComboBox::currentTextChanged, this,
            &P6PatrolPage::onScheduleRouteComboChanged);
    formLayout->addWidget(scheduleRouteCombo_);
    formLayout->addSpacing(5);

    // 경로가 "medicine"(복약 전달 경로)일 때만 나타난다 — onScheduleRouteComboChanged 참고.
    scheduleMedLabel_ = new QLabel(QStringLiteral("복약 스케줄"), scheduleForm_);
    scheduleMedLabel_->setProperty("role", "secondary");
    formLayout->addWidget(scheduleMedLabel_);
    scheduleMedCombo_ = new QComboBox(scheduleForm_);
    connect(scheduleMedCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &P6PatrolPage::applyMedicationScheduleSelection);
    formLayout->addWidget(scheduleMedCombo_);
    formLayout->addSpacing(5);
    scheduleMedLabel_->hide();
    scheduleMedCombo_->hide();

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
    scheduleSaveBtn_ = new QPushButton(QStringLiteral("저장"), scheduleForm_);
    scheduleSaveBtn_->setProperty("variant", "primary");
    connect(scheduleSaveBtn_, &QPushButton::clicked, this, &P6PatrolPage::saveScheduleFromForm);
    formBtnRow->addWidget(scheduleSaveBtn_, /*stretch=*/1);
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

QPointF P6PatrolPage::designMapMetersToCanvas(const MapData& md, QPointF meters) const {
    if (md.resolution <= 0.0 || md.backgroundImage.isNull()) {
        return QPointF(meters.x() * kPxPerMeter, meters.y() * kPxPerMeter);
    }
    // canvas_->displayBounds()(위젯의 실제 현재 크기) 기준으로 계산한다 — canvas_는
    // "현재" 맵의 배경 이미지만 표시 중이지만, 그리기 가능 영역 자체(카드 여백을 뺀
    // 크기)는 어떤 맵을 보고 있든 동일하므로 아직 화면에 없는 다른 맵의 좌표를 미리
    // 계산해도 안전하다.
    //
    // 예전엔 여기서 와이어프레임 설계 기준의 고정 크기(380x330)를 쓰고 나중에
    // showEvent/resizeEvent의 remapOnResize가 실제 창 크기로 다시 배치해줄 거라고
    // 가정했는데 — 서버에서 웨이포인트를 비동기로 받아오는 시점(loadServerMapImageDone)
    // 은 보통 이 페이지가 이미 한 번 보여진 뒤라 hasFitInitialView_ 가드 때문에
    // showEvent의 재배치가 다시 실행되지 않는다. 그 결과 웨이포인트가 380x330 기준
    // 좌표 그대로 훨씬 큰 실제 캔버스에 찍혀 전부 왼쪽 위 구석에 뭉쳐 보였다
    // (2026-09-08 실사용 리포트로 확인 — 사용자가 지도 아래쪽에 찍은 웨이포인트가
    // "저장·배포" 후 다시 불러오니 왼쪽 위에 뭉쳐서 나타남).
    return CanvasGeometry::metersToCanvas(meters, md.resolution, md.originMeters, canvas_->displayBounds(),
                                           md.backgroundImage);
}

void P6PatrolPage::handleCanvasAddRequested(QPointF pos, qreal headingDeg) {
    popover_->setForNewWaypoint(pos, canvas_->canvasToMapMeters(pos), headingDeg);
    const QPoint anchor = canvas_->mapTo(this, pos.toPoint());
    movePopoverNear(anchor);
    popover_->raise();
    popover_->show();
}

void P6PatrolPage::handleCanvasEditRequested(const QString& id) {
    const WaypointCanvas::Waypoint* wp = canvas_->findWaypoint(id);
    if (!wp) return;
    popover_->setForEditWaypoint(*wp);
    const QPoint anchor = canvas_->mapTo(this, wp->pos.toPoint());
    movePopoverNear(anchor);
    popover_->raise();
    popover_->show();
}

void P6PatrolPage::movePopoverNear(const QPoint& anchor) {
    // 예전엔 아래쪽(y) 방향은 0 이상으로만 막고 화면 하단은 안 막아서, 지도 아래쪽을
    // 클릭하면 팝업이 창 밖으로 넘어가 저장·취소 버튼이 잘려 보였다. x와 같은 방식으로
    // 하단도 clamp한다.
    const int x = qMin(anchor.x() + 12, width() - popover_->width() - 8);
    const int y = qMin(qMax(anchor.y() - 20, 0), height() - popover_->height() - 8);
    popover_->move(x, y);
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

    // "저장 · 로봇에 배포"가 없어진 뒤로는(요청 사양) 각 동작이 그 자리에서 바로
    // 서버에 반영된다 — 삭제도 마찬가지. 서버에 아직 없던(생성 응답을 못 받은)
    // 웨이포인트면 waypointServerIds에 id가 없어 조용히 로컬에서만 지운다.
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    MapData& current = maps_[currentMapIndex_];
    const int serverId = current.waypointServerIds.value(id, -1);
    if (serverId > 0) {
        current.waypointServerIds.remove(id);
        apiClient_->deleteWaypoint(serverId, [](bool, const QString&) {});
    }
}

void P6PatrolPage::handleCanvasMoved(const QString& id, QPointF pos, qreal headingDeg) {
    for (WaypointCanvas::Waypoint& w : waypoints_) {
        if (w.id == id) {
            w.pos = pos;
            w.yawDeg = headingDeg;
            // metersPos도 같이 갱신해야 한다 — 이게 실제 map 프레임 좌표의 유일한 출처라,
            // 여기서 안 맞춰두면 다음에 위젯이 리사이즈될 때(예: "경로 편집" 모드 진입으로
            // 우측 패널 폭이 바뀌는 경우) requestMetersResync()가 이 웨이포인트를 옛 metersPos
            // (드래그 전 값, 새로 추가된 경우 기본값 0,0)로 되돌려버린다.
            w.metersPos = canvas_->canvasToMapMeters(pos);
            break;
        }
    }
    // canvas_ 자신의 내부 waypoints_ 사본은 드래그로 이미 pos가 맞지만 metersPos는
    // 안 바뀐 채로 남아있다 — 방금 고친 값을 canvas_ 쪽에도 반영해야, 그 사본을
    // 기준으로 도는 requestMetersResync()도 정확한 metersPos를 쓰게 된다.
    canvas_->setWaypoints(waypoints_);
    rebuildWpList();

    // waypointMoved는 드래그가 끝날 때(mouseReleaseEvent) 딱 한 번만 온다 — 드래그
    // 도중 계속 오는 게 아니라 여기서 바로 서버에 반영해도 요청이 쏟아지지 않는다.
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const int serverId = maps_[currentMapIndex_].waypointServerIds.value(id, -1);
    if (serverId <= 0) return;  // 아직 서버에 없는 웨이포인트 — 다음 "저장"(popover)에서 생성될 때 반영됨
    const QPointF meters = canvas_->canvasToMapMeters(pos);
    apiClient_->updateWaypoint(
        serverId,
        QJsonObject{{"pos_x", meters.x()}, {"pos_y", meters.y()},
                    {"yaw", CanvasGeometry::canvasYawDegToMapYawRad(headingDeg)}},
        [](bool, const ApiClient::WaypointInfo&, const QString&) {});
}

void P6PatrolPage::handlePopoverSave(const QString& id, const QString& name, bool charging, qreal yaw,
                                      QPointF pos) {
    QString oldChargingCode;  // 교체되어 'patrol'로 내려야 할 예전 충전 웨이포인트(있으면)
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
            oldChargingCode = existing;
        }
    }

    QString code = id;
    if (id.isEmpty()) {
        code = nextWaypointId();
        WaypointCanvas::Waypoint wp{code, name, charging, pos, yaw};
        // handleCanvasMoved와 같은 이유 — 로컬에서 새로 만든 웨이포인트는 metersPos가
        // 기본값(0,0)인 채로 남아서, 다음 리사이즈(예: "경로 편집" 모드 진입) 때
        // requestMetersResync()가 지도 원점(0,0m)으로 순간이동시켜버리는 버그가 있었다
        // ("WP 추가 후 경로 이름 입력하면 웨이포인트들이 다 이상한 곳에 모임" 리포트).
        wp.metersPos = canvas_->canvasToMapMeters(pos);
        ++nextWpSeq_;
        waypoints_.append(wp);
    } else {
        for (WaypointCanvas::Waypoint& w : waypoints_) {
            if (w.id == code) {
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

    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    if (maps_[currentMapIndex_].serverMapId <= 0) return;  // 서버 미등록 맵(방어적 — P6에 뜨는 맵은 항상 등록돼 있음)

    // 서버 waypoint.kind='charge' 유니크 제약(맵당 1개, api/v1/map.py 확인) 때문에,
    // 충전 스테이션을 교체하는 경우 예전 웨이포인트를 먼저 'patrol'로 내려야 한다 —
    // 순서를 안 지키고 새 걸 먼저 저장하면 409로 거부된다.
    auto saveThisWaypoint = [this, code, name, charging, pos, yaw]() {
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        MapData& current = maps_[currentMapIndex_];
        const int existingServerId = current.waypointServerIds.value(code, -1);
        const QPointF meters = canvas_->canvasToMapMeters(pos);
        const double yawRad = CanvasGeometry::canvasYawDegToMapYawRad(yaw);
        const QString kind = charging ? QStringLiteral("charge") : QStringLiteral("patrol");
        if (existingServerId > 0) {
            apiClient_->updateWaypoint(existingServerId,
                                        QJsonObject{{"name", name},
                                                     {"kind", kind},
                                                     {"pos_x", meters.x()},
                                                     {"pos_y", meters.y()},
                                                     {"yaw", yawRad}},
                                        [](bool, const ApiClient::WaypointInfo&, const QString&) {});
        } else {
            apiClient_->createWaypoint(
                current.serverMapId, code, name, kind, meters.x(), meters.y(), yawRad,
                [this, code](bool ok, const ApiClient::WaypointInfo& info, const QString&) {
                    if (ok && currentMapIndex_ >= 0 && currentMapIndex_ < maps_.size()) {
                        maps_[currentMapIndex_].waypointServerIds[code] = info.id;
                    }
                });
        }
    };

    if (!oldChargingCode.isEmpty()) {
        const int oldServerId = maps_[currentMapIndex_].waypointServerIds.value(oldChargingCode, -1);
        if (oldServerId > 0) {
            apiClient_->updateWaypoint(
                oldServerId, QJsonObject{{"kind", QStringLiteral("patrol")}},
                [saveThisWaypoint](bool, const ApiClient::WaypointInfo&, const QString&) { saveThisWaypoint(); });
            return;
        }
    }
    saveThisWaypoint();
}

void P6PatrolPage::rebuildWpList() {
    QLayoutItem* item;
    while ((item = wpListLayout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    // 헤더를 QWidget으로 감싸서 addWidget()한다 — addLayout()으로 바로 붙이면 위 clear
    // 루프의 delete item->widget()이 nullptr을 지우는 셈이라(레이아웃 아이템은 위젯이
    // 아니다) title·typeHint가 지워지지 않고 남아, 다시 그릴 때마다 옛 "웨이포인트 N"
    // 라벨이 새 라벨과 겹쳐 보이는 버그가 있었다.
    auto* headerRow = new QWidget(wpListCard_);
    auto* header = new QHBoxLayout(headerRow);
    header->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(QStringLiteral("웨이포인트 %1").arg(waypoints_.size()), headerRow);
    title->setProperty("role", "label");
    header->addWidget(title);
    header->addStretch();
    auto* typeHint = new QLabel(QStringLiteral("유형: 순찰 지점 · 충전"), headerRow);
    typeHint->setProperty("role", "muted");
    typeHint->setStyleSheet(QStringLiteral("font-size:11px;"));
    header->addWidget(typeHint);
    wpListLayout_->addWidget(headerRow);
    wpListLayout_->addSpacing(6);

    // p6_patrol_route_wireframe.html 그대로 — 하나의 테두리 안에 구분선으로만 나뉜
    // 단순한 한 줄짜리 행: ID · 이름 · (충전이면 "초기 위치" 배지, 아니면 우측 정렬된
    // "x, y, yaw°"). 행 자체를 누르면 편집 팝오버가 열린다(eventFilter) — 와이어프레임엔
    // 없는 동작이지만 겉모습은 바꾸지 않으면서 기존 "행 클릭 = 편집" 진입을 유지한다.
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
        row->setCursor(Qt::PointingHandCursor);
        row->setProperty("wpId", w.id);
        row->installEventFilter(this);

        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 5, 8, 5);
        rowLayout->setSpacing(6);

        auto* idLabel = new QLabel(w.id, row);
        idLabel->setStyleSheet(
            w.charging ? QStringLiteral("font-family:'JetBrains Mono','D2Coding',monospace; "
                                         "color:#0B7A6E; font-size:11px; font-weight:500;")
                       : QStringLiteral("font-family:'JetBrains Mono','D2Coding',monospace; "
                                        "color:#5A6774; font-size:11px;"));
        rowLayout->addWidget(idLabel);

        auto* nameLabel = new QLabel(w.name, row);
        nameLabel->setStyleSheet(QStringLiteral("color:#16202B; font-size:11px;"));
        rowLayout->addWidget(nameLabel);
        rowLayout->addStretch();

        if (w.charging) {
            auto* badge = new QLabel(QStringLiteral("초기 위치"), row);
            badge->setStyleSheet(QStringLiteral(
                "color:#0B7A6E; background:#E0F5F2; font-size:10px; padding:1px 5px; border-radius:3px;"));
            rowLayout->addWidget(badge);
        } else {
            const QPointF meters = canvas_->canvasToMapMeters(w.pos);
            auto* coordLabel = new QLabel(
                QStringLiteral("%1, %2, %3°")
                    .arg(meters.x(), 0, 'f', 1)
                    .arg(meters.y(), 0, 'f', 1)
                    .arg(static_cast<int>(w.yawDeg)),
                row);
            coordLabel->setStyleSheet(QStringLiteral(
                "font-family:'JetBrains Mono','D2Coding',monospace; color:#93A0AC; font-size:11px;"));
            rowLayout->addWidget(coordLabel);
        }

        listLayout->addWidget(row);
    }

    wpListLayout_->addWidget(listFrame);
}

bool P6PatrolPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        const QVariant wpId = watched->property("wpId");
        if (wpId.isValid()) {
            // 캔버스가 "이동" 모드일 때 지도 위 마커를 클릭하면 그 지점으로 주행하는
            // 것과 똑같이, 목록에서도 같은 웨이포인트를 누르면 주행 요청으로 이어지게
            // 한다. "WP 추가" 모드에서는 기존대로 편집 팝오버를 연다.
            if (canvas_->interactionMode() == WaypointCanvas::InteractionMode::MoveWaypoint) {
                driveToSingleWaypoint(wpId.toString());
            } else {
                handleCanvasEditRequested(wpId.toString());
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void P6PatrolPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // QStackedWidget이 이 페이지를 현재 화면으로 바꿀 때마다 호출된다 — "순찰
    // 시작"/"이동"을 누르기 전에도 로봇 위치를 먼저 보고 싶다는 요청에 대응한다.
    prepareNavigationForCurrentMap();
    // setFocusPolicy(Qt::StrongFocus)만으로는 방향키가 keyPressEvent까지 안 온다 —
    // 실제로 setFocus()를 불러줘야 한다("방향키 눌러도 안 움직여" 리포트, P5MapPage도
    // 같은 문제/같은 수정). 페이지 전환마다 다시 불러야 다른 위젯(웨이포인트 캔버스,
    // 사이드바 버튼 등)이 그 사이 포커스를 가져간 것도 되돌려진다.
    setFocus();
}

void P6PatrolPage::prepareNavigationForCurrentMap() {
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const MapData& current = maps_[currentMapIndex_];
    if (current.serverMapId <= 0 || current.robotId.isEmpty()) return;
    apiClient_->prepareNavigation(current.robotId, current.serverMapId);
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
        const QPointF a = canvas_->canvasToMapMeters(pts[i]);
        const QPointF b = canvas_->canvasToMapMeters(pts[i + 1]);
        distM += std::hypot(b.x() - a.x(), b.y() - a.y());
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
        // 오른쪽 컬럼이 236px로 고정돼 있어 이름·시각·요일·토글·편집·삭제를 한 줄에
        // 다 넣으면 편집/삭제 버튼이 카드 밖으로 밀려나 클릭이 안 먹는 문제가 있었다
        // ("편집은 되는데 삭제가 안돼" 리포트, 2026-09-09 — 삭제 버튼이 맨 끝이라 항상
        // 잘려나갔던 것). 정보 줄과 액션 줄을 분리해 액션 버튼이 항상 보이게 한다.
        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(8, 6, 8, 6);
        rowLayout->setSpacing(4);

        auto* infoRow = new QHBoxLayout();
        infoRow->setSpacing(6);
        auto* nameLabel = new QLabel(sched.routeName, row);
        nameLabel->setStyleSheet(QStringLiteral("font-size:11px;"));
        infoRow->addWidget(nameLabel);
        auto* timeLabel = new QLabel(sched.timeText, row);
        timeLabel->setProperty("role", "mono");
        timeLabel->setStyleSheet(QStringLiteral("color:#5A6774; font-size:11px;"));
        infoRow->addWidget(timeLabel);

        QString daysText = QStringLiteral("매일");
        if (sched.days.size() != 7) {
            QStringList names;
            for (int d : sched.days) names << dayNames.value(d);
            daysText = names.join(QStringLiteral(","));
        }
        auto* daysLabel = new QLabel(daysText, row);
        daysLabel->setProperty("role", "muted");
        daysLabel->setStyleSheet(QStringLiteral("font-size:11px;"));
        infoRow->addWidget(daysLabel);
        infoRow->addStretch();
        rowLayout->addLayout(infoRow);

        auto* actionRow = new QHBoxLayout();
        actionRow->setSpacing(4);
        actionRow->addStretch();

        auto* toggle = new ToggleSwitch(row);
        toggle->setChecked(sched.enabled);
        connect(toggle, &ToggleSwitch::toggled, this, [this, i](bool on) {
            schedules_[i].enabled = on;
            const int serverId = schedules_[i].serverId;
            if (serverId > 0) {
                apiClient_->updatePatrolSchedule(serverId, QJsonObject{{"enabled", on}},
                                                  [](bool, const ApiClient::PatrolScheduleInfo&,
                                                     const QString&) {});
            }
        });
        actionRow->addWidget(toggle);

        auto* editBtn = new QPushButton(row);
        editBtn->setIcon(SimpleIcons::edit(Tokens::TEXT_MUTED, 12));
        editBtn->setProperty("variant", "ghost");
        editBtn->setFixedSize(20, 20);
        editBtn->setIconSize(QSize(12, 12));
        connect(editBtn, &QPushButton::clicked, this, [this, i] { startEditSchedule(i); });
        actionRow->addWidget(editBtn);

        auto* deleteBtn = new QPushButton(row);
        deleteBtn->setIcon(SimpleIcons::trash(Tokens::TEXT_MUTED, 12));
        deleteBtn->setProperty("variant", "ghost");
        deleteBtn->setFixedSize(20, 20);
        deleteBtn->setIconSize(QSize(12, 12));
        connect(deleteBtn, &QPushButton::clicked, this, [this, i] { deleteScheduleAt(i); });
        actionRow->addWidget(deleteBtn);

        rowLayout->addLayout(actionRow);
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
    // 기존 "medicine" 경로를 다시 열면 체크박스도 그 상태로 맞춰야 하는데,
    // setChecked가 onRouteMedicineToggled를 그대로 다시 태우면 방금 세팅한
    // 이름을 자기가 되돌리려 들 수 있어 시그널을 잠깐 막는다.
    const bool isMedicineRoute = (routeName == kMedicineRouteName);
    routeMedicineCheckbox_->blockSignals(true);
    routeMedicineCheckbox_->setChecked(isMedicineRoute);
    routeMedicineCheckbox_->blockSignals(false);
    routeEditNameEdit_->setReadOnly(isMedicineRoute);
    canvas_->setRouteEditMode(true);
    canvas_->setRouteEditSelection(routeEditSelection_);
    routeStack_->setCurrentIndex(1);
    canvasToolbarRow_->hide();
    routeEditBanner_->show();
    rebuildRouteEditList();
}

void P6PatrolPage::exitRouteEditMode(bool save) {
    if (save) {
        const QString name = routeEditNameEdit_->text().trimmed();
        if (name.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("경로 이름 필요"), QStringLiteral("경로 이름을 입력하세요."));
            return;
        }
        const QString previousName = routeEditTargetName_;
        if (!previousName.isEmpty() && previousName != name) {
            routes_.remove(previousName);
        }
        routes_[name] = routeEditSelection_;
        currentRoute_ = name;

        // "저장 · 로봇에 배포"가 없어진 뒤로는(요청 사양) "경로 저장" 버튼 자체가 바로
        // 서버에 반영한다. steps는 이 경로가 참조하는 웨이포인트들의 *서버* id 배열이라,
        // 그 웨이포인트들이 이미 각자 저장(handlePopoverSave)돼 있어야 정확히 채워진다 —
        // 아직 서버에 없는 웨이포인트는(waypointServerIds에 없음) steps에서 조용히 빠진다.
        if (currentMapIndex_ >= 0 && currentMapIndex_ < maps_.size()) {
            MapData& current = maps_[currentMapIndex_];
            if (current.serverMapId > 0) {
                QVector<int> steps;
                for (const QString& code : routeEditSelection_) {
                    const int wpId = current.waypointServerIds.value(code, -1);
                    if (wpId > 0) steps.append(wpId);
                }
                const int existingRouteId = current.routeServerIds.value(previousName, -1);
                if (existingRouteId > 0) {
                    QJsonArray stepsArray;
                    for (int s : steps) stepsArray.append(s);
                    apiClient_->updatePatrolRoute(
                        existingRouteId, QJsonObject{{"name", name}, {"steps", stepsArray}},
                        [this, previousName, name](bool ok, const ApiClient::PatrolRouteInfo& info,
                                                    const QString&) {
                            if (!ok || currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
                            MapData& cur = maps_[currentMapIndex_];
                            if (previousName != name) cur.routeServerIds.remove(previousName);
                            cur.routeServerIds[name] = info.id;
                        });
                } else {
                    apiClient_->createPatrolRoute(
                        current.serverMapId, name, steps,
                        [this, name](bool ok, const ApiClient::PatrolRouteInfo& info, const QString&) {
                            if (ok && currentMapIndex_ >= 0 && currentMapIndex_ < maps_.size()) {
                                maps_[currentMapIndex_].routeServerIds[name] = info.id;
                            }
                        });
                }
            }
        }
    }

    routeEditMode_ = false;
    canvas_->setRouteEditMode(false);
    routeStack_->setCurrentIndex(0);
    routeEditBanner_->hide();
    canvasToolbarRow_->show();

    rebuildRouteChips();
    rebuildRouteOrderList();
    refreshCanvasPreview();
}

void P6PatrolPage::onRouteMedicineToggled(bool checked) {
    if (checked) {
        routeNameBeforeMedicineCheck_ = routeEditNameEdit_->text();
        routeEditNameEdit_->setText(kMedicineRouteName);
        routeEditNameEdit_->setReadOnly(true);
    } else {
        routeEditNameEdit_->setReadOnly(false);
        routeEditNameEdit_->setText(routeNameBeforeMedicineCheck_);
    }
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
    // 예전엔 QInputDialog::getText 하나로 이름만 받았는데, "복약 전달 경로" 체크박스를
    // 이름 칸 바로 밑에 두려면(요청 사양) QInputDialog에는 필드를 추가할 수 없어 직접
    // QDialog를 만든다 — Ok/Cancel 버튼은 QDialogButtonBox 표준 버튼을 그대로 써서
    // QInputDialog와 같은 플랫폼 아이콘이 그대로 나오게 한다.
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("새 경로"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* nameLabel = new QLabel(QStringLiteral("경로 이름"), &dialog);
    layout->addWidget(nameLabel);
    auto* nameEdit = new QLineEdit(QStringLiteral("새 경로"), &dialog);
    layout->addWidget(nameEdit);

    auto* medicineCheckbox = new QCheckBox(QStringLiteral("복약 전달 경로"), &dialog);
    layout->addWidget(medicineCheckbox);

    auto nameBeforeCheck = std::make_shared<QString>(nameEdit->text());
    connect(medicineCheckbox, &QCheckBox::toggled, &dialog, [nameEdit, nameBeforeCheck](bool checked) {
        if (checked) {
            *nameBeforeCheck = nameEdit->text();
            nameEdit->setText(kMedicineRouteName);
            nameEdit->setReadOnly(true);
        } else {
            nameEdit->setReadOnly(false);
            nameEdit->setText(*nameBeforeCheck);
        }
    });

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    nameEdit->selectAll();
    nameEdit->setFocus();

    if (dialog.exec() != QDialog::Accepted) return;
    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty()) return;
    enterRouteEditMode(name);
}

void P6PatrolPage::deleteCurrentRoute() {
    if (currentRoute_.isEmpty() || !routes_.contains(currentRoute_)) return;

    const auto reply = QMessageBox::question(
        this, QStringLiteral("경로 삭제"),
        QStringLiteral("'%1' 경로를 삭제할까요? 이 경로를 쓰는 주행 스케줄도 함께 삭제됩니다.")
            .arg(currentRoute_),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    const QString routeName = currentRoute_;
    routes_.remove(routeName);
    for (int i = schedules_.size() - 1; i >= 0; --i) {
        if (schedules_[i].routeName == routeName) schedules_.remove(i);
    }

    currentRoute_ = routes_.isEmpty() ? QString() : routes_.keys().first();

    // 서버에 이미 저장된 경로였으면 같이 지운다 — patrol.py::delete_patrol_route가
    // 이 경로를 쓰던 patrol_schedule도 함께 소프트 삭제해준다.
    if (currentMapIndex_ >= 0 && currentMapIndex_ < maps_.size()) {
        MapData& current = maps_[currentMapIndex_];
        const int serverId = current.routeServerIds.value(routeName, -1);
        if (serverId > 0) {
            current.routeServerIds.remove(routeName);
            apiClient_->deletePatrolRoute(serverId, [](bool, const QString&) {});
        }
    }

    rebuildRouteChips();
    rebuildRouteOrderList();
    rebuildScheduleList();
    refreshCanvasPreview();
}

void P6PatrolPage::runTestRun() {
    const QVector<QString> order = routes_.value(currentRoute_);
    const QString chargeId = chargingWaypointId();
    if (currentRoute_.isEmpty() || order.isEmpty() || chargeId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("순찰 시작"),
                                  QStringLiteral("경로에 웨이포인트가 없거나 충전 스테이션이 없습니다."));
        return;
    }
    // 예전엔 여기서 canvas_->startTestRun(order, chargeId)로 화면 위에서만 다이얼 마커를
    // 가짜로 움직였다 — 서버 연동 여부와 무관하게 항상 재생돼서, 실제로는 로봇이 안
    // 움직이는데(아래 route_id 없음으로 조용히 return하는 경우 등) 대시보드만 순찰이
    // 진행되는 것처럼 보이는 문제가 있었다("순찰시작을 누르면 대시보드의 현재위치는
    // 움직이는데 실제 로봇은 안 움직여" 리포트). driveToSingleWaypoint와 같은 이유로
    // 제거 — 현재위치 마커는 이제 로봇이 실제로 보내는 robot_position WS 메시지로만
    // 움직인다.

    // 서버에 등록된 맵·경로일 때만 실제 POST /patrol-runs로 로봇에 순찰 시작을 지시한다
    // ("경로 저장" 시점에 바로 서버에 반영되므로(exitRouteEditMode 참고) 정상적으로는
    // 항상 있어야 하지만, 로컬 데모 맵이거나 그 저장이 실패했을 경우를 대비한 방어).
    const MapData& current = maps_[currentMapIndex_];
    if (current.serverRobotId <= 0 || !current.routeServerIds.contains(currentRoute_)) {
        QMessageBox::information(
            this, QStringLiteral("순찰 시작"),
            QStringLiteral("이 경로가 아직 서버에 저장되지 않아 로봇에 순찰 명령을 보낼 수 없습니다. "
                            "경로를 다시 저장해 주세요."));
        return;
    }

    const int routeId = current.routeServerIds.value(currentRoute_);
    apiClient_->startPatrolRun(
        current.serverRobotId, routeId, QStringLiteral("manual"),
        [this](bool ok, const ApiClient::PatrolRunInfo& run, const QString& err) {
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("순찰 시작 실패"),
                                      err.isEmpty() ? QStringLiteral("서버에 순찰 시작 요청을 보내지 못했습니다.")
                                                    : err);
                return;
            }
            if (!run.robotConnected) {
                QMessageBox::information(
                    this, QStringLiteral("순찰 시작"),
                    QStringLiteral("순찰 기록은 저장했지만 로봇이 아직 연결되어 있지 않습니다."));
            }
        });
}

void P6PatrolPage::driveToSingleWaypoint(const QString& id) {
    // 예전엔 여기서 canvas_->startTestRun(...)으로 화면 위에서만 다이얼 마커를 가짜로
    // 움직였다 — 서버·로봇 어디에도 명령이 안 나가 실제 로봇은 그대로였다("wp 이동
    // 명령을 내리면 실제 로봇이 이동하지 않아" 리포트). 이제 POST
    // /patrol-runs/drive-to-point로 진짜 명령을 보낸다. 다이얼 마커는 이 클릭이 아니라
    // 로봇이 실제로 보내는 robot_position WS 메시지(handleWsMessage)로만 움직인다 —
    // 로컬 애니메이션과 섞이면 다시 "실제 위치와 다른 데 있는 마커" 문제가 재발한다.
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const WaypointCanvas::Waypoint* wp = canvas_->findWaypoint(id);
    if (!wp) return;

    const QString robotCode = maps_[currentMapIndex_].robotId;
    const int mapId = maps_[currentMapIndex_].serverMapId;
    const QPointF meters = canvas_->canvasToMapMeters(wp->pos);
    apiClient_->driveToPoint(robotCode, mapId, wp->id, meters.x(), meters.y(),
                              CanvasGeometry::canvasYawDegToMapYawRad(wp->yawDeg),
                              [this](bool ok, const QString& err) {
                                  if (!ok) {
                                      QMessageBox::warning(
                                          this, QStringLiteral("이동 실패"),
                                          err.isEmpty() ? QStringLiteral("로봇이 연결되어 있지 않습니다.") : err);
                                  }
                              });
}

void P6PatrolPage::handleInitialPoseCaptured(QPointF canvasPos, qreal headingDeg) {
    // "위치 보정" 드래그가 끝났다 — 캔버스는 이미 poseCaptureMode를 스스로 껐으니
    // (WaypointCanvas::mouseReleaseEvent), 여기선 툴바 버튼 체크 표시만 되돌리고
    // 서버에 실제 위치를 보낸다. 자동 초기 위치 세팅(충전 스테이션 좌표 가정)이
    // 틀렸을 때 로봇이 실제로 있는 자리를 직접 지정하는 경로 — nav_prepare/
    // drive-to-point와 달리 Nav2 자체를 새로 켜지 않고 이미 떠 있는 AMCL의
    // 위치 추정치만 즉석에서 다시 잡는다.
    poseCaptureBtn_->setChecked(false);
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const MapData& current = maps_[currentMapIndex_];
    if (current.serverMapId <= 0 || current.robotId.isEmpty()) return;

    const QPointF meters = canvas_->canvasToMapMeters(canvasPos);
    apiClient_->setInitialPose(current.robotId, current.serverMapId, meters.x(), meters.y(),
                                CanvasGeometry::canvasYawDegToMapYawRad(headingDeg),
                                [this](bool ok, const QString& err) {
                                    if (!ok) {
                                        QMessageBox::warning(
                                            this, QStringLiteral("위치 보정 실패"),
                                            err.isEmpty() ? QStringLiteral("로봇이 연결되어 있지 않습니다.") : err);
                                    }
                                });
}

void P6PatrolPage::shutdownCurrentRobot() {
    // "로봇 종료" — 이 로봇에서 켜져 있는 전부(베이스 구동+Nav2/SLAM+rviz2+업링크/
    // 다운링크)를 한 번에 내린다. 물리적 로봇의 동작에 영향을 주는 되돌리기 어려운
    // 조작이라(순찰/이동 중이었다면 그것도 멈춘다) 확인 없이 바로 실행하지 않는다.
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const QString robotCode = maps_[currentMapIndex_].robotId;
    if (robotCode.isEmpty()) return;

    const auto reply = QMessageBox::question(
        this, QStringLiteral("로봇 종료"),
        QStringLiteral("%1 로봇에서 켜져 있는 모든 프로그램(베이스 구동·Nav2/SLAM·rviz2·업링크·다운링크)을\n"
                        "전부 끕니다. 계속할까요?")
            .arg(robotCode),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    apiClient_->shutdownRobot(robotCode, [this](bool ok, const QString& err) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("종료 실패"),
                                  err.isEmpty() ? QStringLiteral("로봇이 연결되어 있지 않습니다.") : err);
        }
    });
}

void P6PatrolPage::handleEmergencyStop() {
    // P5MapPage::handleEmergencyStop과 같은 이유·같은 패턴 — Space와 같은 경로라
    // 확인 대화상자를 띄우기 전에 이미 실제 정지 명령이 나간다(안전이 우선이라
    // 사용자 확인을 기다리지 않는다).
    teleopPanel_->handleKeyPress(Qt::Key_Space);
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const QString robotCode = maps_[currentMapIndex_].robotId;
    QMessageBox::warning(this, QStringLiteral("비상정지"),
                          QStringLiteral("%1를 비상정지합니다. 진행 중인 순찰/이동이 중단됩니다.").arg(robotCode));
}

void P6PatrolPage::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    switch (event->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Space:
            teleopPanel_->handleKeyPress(event->key());
            event->accept();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

void P6PatrolPage::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    switch (event->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Space:
            teleopPanel_->handleKeyRelease(event->key());
            event->accept();
            return;
        default:
            break;
    }
    QWidget::keyReleaseEvent(event);
}

void P6PatrolPage::toggleScheduleForm(bool show) {
    if (show && scheduleEditIndex_ < 0) {
        scheduleRouteCombo_->clear();
        scheduleRouteCombo_->addItems(routes_.keys());
        scheduleRouteCombo_->setEnabled(true);
        scheduleTimeEdit_->setTime(QTime(10, 0));
        for (int i = 0; i < scheduleDayChips_.size(); ++i) scheduleDayChips_[i]->setChecked(i < 5);
        scheduleSaveBtn_->setText(QStringLiteral("저장"));
    }
    if (!show) {
        scheduleEditIndex_ = -1;
        scheduleMedLabel_->hide();
        scheduleMedCombo_->hide();
    }
    scheduleForm_->setVisible(show);
}

void P6PatrolPage::startEditSchedule(int index) {
    if (index < 0 || index >= schedules_.size()) return;
    scheduleEditIndex_ = index;
    toggleScheduleForm(true);  // scheduleEditIndex_가 이미 설정돼 있어 위 기본값 리셋은 건너뛴다

    const Schedule& sched = schedules_[index];
    // addItems()가 첫 항목에서 한 번, 아래 setCurrentIndex가 실제 목표 경로로 또 한 번
    // currentTextChanged를 태운다 — 그대로 두면 onScheduleRouteComboChanged가 엉뚱한
    // 경로 이름으로 한 번 먼저(불필요한 네트워크 조회) 불렸다가 곧바로 다시 불린다.
    // 시그널을 막아뒀다가 목표 경로로 정확히 세팅된 뒤 한 번만 직접 호출한다.
    scheduleRouteCombo_->blockSignals(true);
    scheduleRouteCombo_->clear();
    scheduleRouteCombo_->addItems(routes_.keys());
    const int routeIdx = scheduleRouteCombo_->findText(sched.routeName);
    if (routeIdx >= 0) scheduleRouteCombo_->setCurrentIndex(routeIdx);
    scheduleRouteCombo_->blockSignals(false);
    // PUT /patrol-schedules는 route_id 변경을 지원하지 않아 편집 중엔 경로를 고정한다.
    scheduleRouteCombo_->setEnabled(false);
    const QTime t = QTime::fromString(sched.timeText, QStringLiteral("HH:mm"));
    scheduleTimeEdit_->setTime(t.isValid() ? t : QTime(10, 0));
    for (int i = 0; i < scheduleDayChips_.size(); ++i) scheduleDayChips_[i]->setChecked(sched.days.contains(i));
    scheduleSaveBtn_->setText(QStringLiteral("수정 저장"));
    onScheduleRouteComboChanged(scheduleRouteCombo_->currentText());
}

void P6PatrolPage::onScheduleRouteComboChanged(const QString& routeName) {
    const bool isMedicineRoute = (routeName == kMedicineRouteName);
    scheduleMedLabel_->setVisible(isMedicineRoute);
    scheduleMedCombo_->setVisible(isMedicineRoute);
    if (!isMedicineRoute) {
        currentRobotMedSchedules_.clear();
        return;
    }

    if (scheduleEditIndex_ >= 0 && scheduleEditIndex_ < schedules_.size()) {
        loadMedicationSchedulesForCurrentRobot(schedules_[scheduleEditIndex_].medicationScheduleId);
    } else {
        loadMedicationSchedulesForCurrentRobot();
    }
}

void P6PatrolPage::loadMedicationSchedulesForCurrentRobot(int matchMedicationScheduleId) {
    scheduleMedCombo_->clear();
    currentRobotMedSchedules_.clear();
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const QString robotCode = maps_[currentMapIndex_].robotId;

    scheduleMedCombo_->addItem(QStringLiteral("불러오는 중…"));
    scheduleMedCombo_->setEnabled(false);

    apiClient_->fetchRecipients([this, robotCode, matchMedicationScheduleId](
                                     bool ok, const QVector<ApiClient::RecipientInfo>& recipients) {
        int recipientId = 0;
        for (const auto& r : recipients) {
            if (r.robotCode == robotCode) {
                recipientId = r.id;
                break;
            }
        }
        if (!ok || recipientId == 0) {
            scheduleMedCombo_->clear();
            scheduleMedCombo_->addItem(QStringLiteral("배정된 대상자 없음"));
            scheduleMedCombo_->setEnabled(false);
            return;
        }

        apiClient_->fetchRecipientDetail(
            recipientId,
            [this, matchMedicationScheduleId](bool detailOk, const ApiClient::RecipientDetailInfo& detail,
                                               const QString&) {
                currentRobotMedSchedules_ =
                    detailOk ? detail.schedules : QVector<ApiClient::MedicationScheduleInfo>();
                scheduleMedCombo_->clear();
                if (currentRobotMedSchedules_.isEmpty()) {
                    scheduleMedCombo_->addItem(QStringLiteral("등록된 복약 스케줄 없음"));
                    scheduleMedCombo_->setEnabled(false);
                    return;
                }
                scheduleMedCombo_->setEnabled(true);
                // 0번 = "선택 안 함" — currentRobotMedSchedules_[i]는 콤보의 i+1번에 대응.
                scheduleMedCombo_->addItem(QStringLiteral("선택 안 함"));
                int matchIndex = -1;
                for (int i = 0; i < currentRobotMedSchedules_.size(); ++i) {
                    const auto& sched = currentRobotMedSchedules_[i];
                    scheduleMedCombo_->addItem(QStringLiteral("%1번 칸 · %2 · %3")
                                                    .arg(sched.slotNo)
                                                    .arg(sched.drugName, sched.doseTime));
                    if (matchMedicationScheduleId > 0 && sched.id == matchMedicationScheduleId) {
                        matchIndex = i;
                    }
                }
                if (matchIndex >= 0) scheduleMedCombo_->setCurrentIndex(matchIndex + 1);
            });
    });
}

void P6PatrolPage::applyMedicationScheduleSelection(int comboIndex) {
    const int schedIndex = comboIndex - 1;  // 0번 = "선택 안 함"
    if (schedIndex < 0 || schedIndex >= currentRobotMedSchedules_.size()) return;
    const ApiClient::MedicationScheduleInfo& sched = currentRobotMedSchedules_[schedIndex];
    const QTime t = QTime::fromString(sched.doseTime, QStringLiteral("HH:mm"));
    if (t.isValid()) scheduleTimeEdit_->setTime(t);
    for (int i = 0; i < scheduleDayChips_.size(); ++i) {
        scheduleDayChips_[i]->setChecked((sched.daysOfWeek & (1 << i)) != 0);
    }
}

void P6PatrolPage::deleteScheduleAt(int index) {
    if (index < 0 || index >= schedules_.size()) return;
    const Schedule sched = schedules_[index];
    const auto reply = QMessageBox::question(
        this, QStringLiteral("스케줄 삭제"),
        QStringLiteral("%1 %2 스케줄을 삭제할까요?").arg(sched.routeName, sched.timeText),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    if (scheduleEditIndex_ == index) toggleScheduleForm(false);
    schedules_.remove(index);
    rebuildScheduleList();

    if (sched.serverId > 0) {
        apiClient_->deletePatrolSchedule(sched.serverId, [](bool, const QString&) {});
    }
}

void P6PatrolPage::saveScheduleFromForm() {
    const QString routeName = scheduleRouteCombo_->currentText();
    const QString timeText = scheduleTimeEdit_->time().toString(QStringLiteral("HH:mm"));
    QVector<int> days;
    for (int i = 0; i < scheduleDayChips_.size(); ++i) {
        if (scheduleDayChips_[i]->isChecked()) days.append(i);
    }
    if (routeName.isEmpty() || days.isEmpty()) return;

    int daysBitmask = 0;
    for (int d : days) daysBitmask |= (1 << d);

    // 경로가 "medicine"이면 지금 콤보 선택 상태가 그대로 매칭 결과다 — "선택 안 함"이면
    // 매칭 해제(0/null), 실제 항목이면 그 medication_schedule.id. 추가·수정 둘 다 이 값
    // 그대로를 저장한다(요청 사양 — patrol_schedule.medication_schedule_id, 경로가 아니라
    // 이 회차 자체의 속성).
    int medicationScheduleId = 0;
    if (routeName == kMedicineRouteName) {
        const int medIndex = scheduleMedCombo_->currentIndex() - 1;  // 0번 = "선택 안 함"
        if (medIndex >= 0 && medIndex < currentRobotMedSchedules_.size()) {
            medicationScheduleId = currentRobotMedSchedules_[medIndex].id;
        }
    }

    if (scheduleEditIndex_ >= 0 && scheduleEditIndex_ < schedules_.size()) {
        const int editIndex = scheduleEditIndex_;
        Schedule& sched = schedules_[editIndex];
        sched.timeText = timeText;
        sched.days = days;
        sched.medicationScheduleId = medicationScheduleId;
        const int serverId = sched.serverId;
        rebuildScheduleList();
        toggleScheduleForm(false);

        if (serverId > 0) {
            QJsonObject fields{{"run_time", timeText + QStringLiteral(":00")}, {"days_of_week", daysBitmask}};
            if (routeName == kMedicineRouteName) {
                fields["medication_schedule_id"] =
                    medicationScheduleId > 0 ? QJsonValue(medicationScheduleId) : QJsonValue();
            }
            apiClient_->updatePatrolSchedule(serverId, fields,
                                              [](bool, const ApiClient::PatrolScheduleInfo&, const QString&) {});
        }
        return;
    }

    Schedule sched;
    sched.routeName = routeName;
    sched.timeText = timeText;
    sched.days = days;
    sched.enabled = true;
    sched.medicationScheduleId = medicationScheduleId;
    schedules_.append(sched);
    const int newIndex = schedules_.size() - 1;
    rebuildScheduleList();
    toggleScheduleForm(false);

    // 이 경로가 이미 서버에 저장되어 있으면(routeServerIds에 id가 있으면) 바로 서버에도
    // 등록한다. "경로 저장"이 이제 즉시 서버에 반영되므로(exitRouteEditMode 참고)
    // 정상적으로는 항상 있어야 하지만, 아직 경로를 한 번도 저장 안 한 채 스케줄부터
    // 추가하려는 경우를 대비한 방어 — 이때는 스케줄이 로컬에만 남는다.
    const MapData& current = maps_[currentMapIndex_];
    if (!current.routeServerIds.contains(sched.routeName)) return;

    apiClient_->createPatrolSchedule(
        current.routeServerIds.value(sched.routeName), sched.timeText, daysBitmask,
        [this, newIndex](bool ok, const ApiClient::PatrolScheduleInfo& info, const QString&) {
            if (ok && newIndex < schedules_.size()) schedules_[newIndex].serverId = info.id;
        },
        medicationScheduleId);
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

    // 맵(=로봇)이 바뀌면 이전 로봇의 온라인 상태를 그대로 들고 있으면 안 된다 — 새
    // robot_status가 올 때까지는 안전하게 오프라인으로 되돌린다.
    robotOnline_ = false;
    robotOfflineBanner_->setVisible(true);
    // 다른 로봇의 맵으로 전환한 것 — 그 로봇의 Nav2도 미리 준비시킨다(showEvent와
    // 같은 이유).
    prepareNavigationForCurrentMap();
    waypoints_ = m.waypoints;
    routes_ = m.routes;
    currentRoute_ = m.currentRoute;
    nextWpSeq_ = m.nextWpSeq;
    schedules_ = m.schedules;

    mapChipBtn_->setText(
        QStringLiteral("  %1 (%2 · %3)  ▾").arg(m.mapName, m.robotId, m.recipientName));

    canvas_->resetView();
    canvas_->setBackgroundImage(m.backgroundImage);
    canvas_->setMapMeta(m.resolution, m.originMeters);
    canvas_->setWaypoints(waypoints_);
    // 이 맵으로 (다시) 전환할 때마다, 다음 실제로 그려지는 시점에 웨이포인트 좌표를
    // metersPos(서버 원본 미터)로부터 확실하게 다시 계산하도록 예약한다 —
    // WaypointCanvas::showEvent 주석 참고(이 위젯이 이미 한 번 보여진 뒤라면
    // showEvent가 다시 안 불려서 그 보정 기회를 놓칠 수 있다).
    canvas_->requestMetersResync();

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
                              const QString& recipientName, int serverMapId, int serverRobotId, double resolution,
                              QPointF originMeters) {
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
        m.serverMapId = serverMapId;
        m.serverRobotId = serverRobotId;
        m.resolution = resolution;
        m.originMeters = originMeters;
        m.waypoints = {
            {QStringLiteral("WP_01"), QStringLiteral("충전 스테이션"), true, QPointF(90, 260), 0}};
        m.nextWpSeq = 2;
        maps_.append(m);
        foundIndex = maps_.size() - 1;
    } else {
        // 같은 로봇에 이미 있던 맵 — 이름·대상자·배경 이미지만 갱신하고 웨이포인트/경로/
        // 스케줄은 그대로 둔다 (사용자가 P6에서 미리 만들어둔 것을 지우지 않기 위해).
        // 하지만 serverMapId/resolution/originMeters는 반드시 새 값으로 덮어써야 한다 —
        // 이게 예전엔 안 바뀌어서 로봇이 계속 옛 map_id로 알고 있던 버그였다(클래스
        // 코멘트 참고).
        maps_[foundIndex].mapName = mapName;
        maps_[foundIndex].recipientName = recipientName;
        maps_[foundIndex].backgroundImage = mapImage;
        maps_[foundIndex].serverMapId = serverMapId;
        maps_[foundIndex].serverRobotId = serverRobotId;
        maps_[foundIndex].resolution = resolution;
        maps_[foundIndex].originMeters = originMeters;
    }

    rebuildMapMenu();

    if (foundIndex == currentMapIndex_) {
        loadMapData(currentMapIndex_);
        // loadMapData는 캔버스/웨이포인트 목록만 다시 그린다 — 로봇에게 "이 map_id로
        // Nav2 바꿔라"라고 알려주는 건 별도로 nav_prepare를 보내야 한다. 이게 빠지면
        // 화면(이미지)은 새 지도로 바뀌어 보이는데 로봇의 Nav2·rviz2는 여전히 예전
        // 지도로 떠 있는 상태로 남는다.
        prepareNavigationForCurrentMap();
    }
}

void P6PatrolPage::loadMapsFromServer() {
    apiClient_->fetchActiveMaps([this](bool ok, const QVector<ApiClient::MapInfo>& serverMaps) {
        if (!ok || serverMaps.isEmpty()) return;  // 서버 미기동/빈 DB — maps_는 빈 채로 둔다

        auto maps = std::make_shared<QVector<MapData>>();
        maps->reserve(serverMaps.size());
        for (const ApiClient::MapInfo& m : serverMaps) {
            MapData md;
            md.serverMapId = m.id;
            md.serverRobotId = m.robotId;
            md.mapName = m.name;
            md.robotId = m.robotCode;
            md.recipientName = m.recipientName;
            md.imageUrl = m.imageUrl;
            md.resolution = m.resolution;
            md.originMeters = QPointF(m.originX, m.originY);
            md.nextWpSeq = 1;
            maps->append(md);
        }
        loadServerMapDetails(maps, 0);
    });
}

void P6PatrolPage::refreshMapForRobot(const QString& robotCode) {
    if (robotCode.isEmpty()) return;

    apiClient_->fetchActiveMaps([this, robotCode](bool ok, const QVector<ApiClient::MapInfo>& serverMaps) {
        if (!ok) return;
        for (const ApiClient::MapInfo& m : serverMaps) {
            if (m.robotCode != robotCode) continue;

            if (m.imageUrl.isEmpty()) {
                upsertMap(QImage(), m.name, m.robotCode, m.recipientName, m.id, m.robotId, m.resolution,
                          QPointF(m.originX, m.originY));
                return;
            }
            apiClient_->fetchBytes(m.imageUrl, [this, m](bool imgOk, const QByteArray& data) {
                QImage image;
                if (imgOk) image = PgmImageLoader::loadFromData(data);
                upsertMap(image, m.name, m.robotCode, m.recipientName, m.id, m.robotId, m.resolution,
                          QPointF(m.originX, m.originY));
            });
            return;
        }
    });
}

void P6PatrolPage::loadServerMapDetails(std::shared_ptr<QVector<MapData>> maps, int index) {
    if (index >= maps->size()) {
        // 모든 맵의 웨이포인트·경로·스케줄을 다 채웠다 — 로컬 데모 데이터를 서버 데이터로 교체.
        maps_ = *maps;
        rebuildMapMenu();
        loadMapData(0);
        return;
    }

    const QString imageUrl = (*maps)[index].imageUrl;
    if (imageUrl.isEmpty()) {
        loadServerMapImageDone(maps, index);
        return;
    }
    apiClient_->fetchBytes(imageUrl, [this, maps, index](bool ok, const QByteArray& data) {
        if (ok) {
            const QImage image = PgmImageLoader::loadFromData(data);
            if (!image.isNull()) (*maps)[index].backgroundImage = image;
        }
        loadServerMapImageDone(maps, index);
    });
}

void P6PatrolPage::loadServerMapImageDone(std::shared_ptr<QVector<MapData>> maps, int index) {
    const int mapId = (*maps)[index].serverMapId;
    apiClient_->fetchWaypoints(
        mapId,
        [this, maps, index, mapId](bool ok, const QVector<ApiClient::WaypointInfo>& serverWaypoints) {
            MapData& md = (*maps)[index];
            QMap<int, QString> codeById;  // 순찰 경로 steps(웨이포인트 id)를 code로 옮기는 데 필요
            int nextSeq = 1;
            for (const ApiClient::WaypointInfo& w : serverWaypoints) {
                const bool charging = w.kind == QStringLiteral("charge");
                const QPointF meters(w.posX, w.posY);
                md.waypoints.append(WaypointCanvas::Waypoint{
                    w.code, w.name, charging, designMapMetersToCanvas(md, meters),
                    CanvasGeometry::mapYawRadToCanvasYawDeg(w.yaw),
                    // metersPos: canvas_->showEvent()가 이 위젯이 처음으로 유효한 실제
                    // 크기를 갖게 되는 시점에 pos를 이 값으로부터 정확히 다시 계산한다
                    // (designMapMetersToCanvas가 지금 계산한 pos는 이 시점에 위젯 크기가
                    // 아직 확정 전이면 틀릴 수 있다 — WaypointCanvas::showEvent 주석 참고).
                    meters});
                md.waypointServerIds[w.code] = w.id;
                codeById[w.id] = w.code;

                if (w.code.startsWith(QStringLiteral("WP_"))) {
                    bool convOk = false;
                    const int n = w.code.mid(3).toInt(&convOk);
                    if (convOk) nextSeq = qMax(nextSeq, n + 1);
                }
            }
            md.nextWpSeq = nextSeq;

            if (!ok) {
                loadServerMapDetails(maps, index + 1);
                return;
            }

            apiClient_->fetchPatrolRoutes(
                mapId, [this, maps, index, codeById](bool routesOk,
                                                      const QVector<ApiClient::PatrolRouteInfo>& routes) {
                    MapData& md2 = (*maps)[index];
                    if (!routesOk || routes.isEmpty()) {
                        loadServerMapDetails(maps, index + 1);
                        return;
                    }

                    for (const ApiClient::PatrolRouteInfo& r : routes) {
                        QVector<QString> codes;
                        for (int waypointId : r.steps) codes.append(codeById.value(waypointId));
                        md2.routes[r.name] = codes;
                        md2.routeServerIds[r.name] = r.id;
                    }
                    md2.currentRoute = routes.first().name;

                    loadServerRouteSchedules(maps, index, routes, 0);
                });
        });
}

void P6PatrolPage::loadServerRouteSchedules(std::shared_ptr<QVector<MapData>> maps, int mapIndex,
                                             QVector<ApiClient::PatrolRouteInfo> routes, int routeIndex) {
    if (routeIndex >= routes.size()) {
        loadServerMapDetails(maps, mapIndex + 1);
        return;
    }

    const ApiClient::PatrolRouteInfo route = routes[routeIndex];
    apiClient_->fetchPatrolSchedules(
        route.id, [this, maps, mapIndex, routes, routeIndex,
                   route](bool ok, const QVector<ApiClient::PatrolScheduleInfo>& schedules) {
            if (ok) {
                MapData& md = (*maps)[mapIndex];
                for (const ApiClient::PatrolScheduleInfo& s : schedules) {
                    QVector<int> days;
                    for (int i = 0; i < 7; ++i) {
                        if (s.daysOfWeek & (1 << i)) days.append(i);
                    }
                    md.schedules.append(
                        Schedule{route.name, s.runTime, days, s.enabled, s.id, s.medicationScheduleId});
                }
            }
            loadServerRouteSchedules(maps, mapIndex, routes, routeIndex + 1);
        });
}

void P6PatrolPage::handleWsMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("slam_done")) {
        // 예전엔 이 함수가 robot_position/camera_frame만 처리하고 나머지는 다 무시했다
        // — 그래서 P5에서 SLAM 맵을 새로 저장해도 P6의 맵 드롭다운은 앱을 껐다 켤 때까지
        // (loadMapsFromServer()가 생성자에서 딱 한 번만 불림) 절대 안 바뀌었다.
        refreshMapForRobot(message.value(QStringLiteral("robot_code")).toString());
        return;
    }

    if (type == QStringLiteral("patrol_schedules_changed")) {
        // P3 대상자 페이지에서 매칭된 복약 스케줄의 시각·요일을 바꾸면 서버가 이걸
        // 보낸다(recipient.py::update_recipient) — 대시보드를 껐다 켜지 않아도 바로
        // 반영되게 한다("대상자에서 복약 스케줄을 바꿔도 주행 스케줄엔 반영이 안 된다"
        // 리포트).
        const int routeId = message.value(QStringLiteral("route_id")).toInt();
        if (routeId > 0) refreshSchedulesForRoute(routeId);
        return;
    }

    if (type == QStringLiteral("robot_status")) {
        // P5MapPage::handleWsMessage의 같은 처리와 같은 이유 — "이동" 버튼이 실제로는
        // 안 먹히는데 그걸 모른 채 계속 누르는 상황을 막는다.
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        if (maps_[currentMapIndex_].robotId != message.value(QStringLiteral("robot_code")).toString()) return;
        robotOnline_ = message.value(QStringLiteral("online")).toBool();
        robotOfflineBanner_->setVisible(!robotOnline_);
        return;
    }

    if (type != QStringLiteral("robot_position") && type != QStringLiteral("camera_frame") &&
        type != QStringLiteral("patrol_path_update"))
        return;
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;

    const QString robotCode = message.value(QStringLiteral("robot_code")).toString();
    if (maps_[currentMapIndex_].robotId != robotCode) return;

    if (type == QStringLiteral("patrol_path_update")) {
        // Nav2 planner_server가 발행한 전역 경로(map 프레임 미터) — rviz2가 보여주는
        // "로봇이 가려는 길" 선과 같은 것을 캔버스에 그대로 그린다. 좌표 변환은
        // WaypointCanvas::paintEvent가 그릴 때마다 하므로 여기서는 미터 그대로 넘긴다.
        const QJsonArray flat = message.value(QStringLiteral("points")).toArray();
        QVector<QPointF> pathMeters;
        pathMeters.reserve(flat.size() / 2);
        for (int i = 0; i + 1 < flat.size(); i += 2) {
            pathMeters.append(QPointF(flat.at(i).toDouble(), flat.at(i + 1).toDouble()));
        }
        canvas_->setPlannedPath(pathMeters);
        return;
    }

    if (type == QStringLiteral("camera_frame")) {
        // /camera/color/image_raw/compressed를 uplink_node가 재인코딩 없이 그대로
        // base64로 실어 보낸 것 — JPEG라서 QImage가 바로 디코딩한다.
        const QByteArray jpegBytes =
            QByteArray::fromBase64(message.value(QStringLiteral("jpeg_base64")).toString().toLatin1());
        QImage frame;
        if (frame.loadFromData(jpegBytes)) {
            canvas_->setCameraFrame(frame);
        }
        return;
    }

    const qreal xMeters = message.value(QStringLiteral("x")).toDouble();
    const qreal yMeters = message.value(QStringLiteral("y")).toDouble();
    const qreal yawRad = message.value(QStringLiteral("yaw")).toDouble();
    canvas_->setCurrentRobotPosition(canvas_->mapMetersToCanvas(QPointF(xMeters, yMeters)),
                                      CanvasGeometry::mapYawRadToCanvasYawDeg(yawRad));
}

void P6PatrolPage::refreshSchedulesForRoute(int routeId) {
    // 지금 스케줄 폼을 편집 중이면(scheduleEditIndex_>=0) 건드리지 않는다 — 목록을
    // 통째로 교체하면 폼이 가리키던 인덱스가 어긋날 수 있다. 폼을 닫은 뒤 다음
    // 새로고침에서 자연히 최신 값으로 맞춰진다.
    if (scheduleEditIndex_ >= 0) return;

    // routeId가 어느 맵의 어느 경로 이름인지 먼저 찾는다 — routeServerIds가 맵마다
    // 독립이라 이 route_id를 들고 있는 맵은 최대 하나뿐이다.
    for (int mi = 0; mi < maps_.size(); ++mi) {
        QString routeName;
        const QMap<QString, int>& ids = maps_[mi].routeServerIds;
        for (auto it = ids.begin(); it != ids.end(); ++it) {
            if (it.value() == routeId) {
                routeName = it.key();
                break;
            }
        }
        if (routeName.isEmpty()) continue;

        apiClient_->fetchPatrolSchedules(
            routeId, [this, mi, routeName](bool ok, const QVector<ApiClient::PatrolScheduleInfo>& list) {
                if (!ok || mi < 0 || mi >= maps_.size()) return;
                QVector<Schedule> fresh;
                fresh.reserve(list.size());
                for (const ApiClient::PatrolScheduleInfo& s : list) {
                    QVector<int> days;
                    for (int i = 0; i < 7; ++i) {
                        if (s.daysOfWeek & (1 << i)) days.append(i);
                    }
                    fresh.append(Schedule{routeName, s.runTime, days, s.enabled, s.id, s.medicationScheduleId});
                }

                if (mi == currentMapIndex_) {
                    if (scheduleEditIndex_ >= 0) return;  // 그 사이 편집을 시작했으면 여기서도 건너뛴다
                    for (int i = schedules_.size() - 1; i >= 0; --i) {
                        if (schedules_[i].routeName == routeName) schedules_.remove(i);
                    }
                    schedules_ += fresh;
                    rebuildScheduleList();
                } else {
                    MapData& md = maps_[mi];
                    for (int i = md.schedules.size() - 1; i >= 0; --i) {
                        if (md.schedules[i].routeName == routeName) md.schedules.remove(i);
                    }
                    md.schedules += fresh;
                }
            });
        return;
    }
}
