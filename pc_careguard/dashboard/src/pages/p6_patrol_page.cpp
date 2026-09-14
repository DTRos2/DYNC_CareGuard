#include "p6_patrol_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QAction>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStyle>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>
#include <cmath>

#include <QJsonArray>
#include <QJsonObject>

#include "net/ws_client.h"
#include "theme/tokens.h"
#include "widgets/canvas_geometry.h"
#include "widgets/careguard_combo_box.h"
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

// 웨이포인트 목록 카드 — 5줄까지만 보이고 그 이상은 스크롤(요청 사양, 2026-09-10).
// 한 행의 실제 렌더링 높이 — 실 데이터(웨이포인트 6개)로 스크린샷을 찍어 정확히
// 맞춘 값(24px). routeOrderList_->setFixedHeight(120)도 같은 24px 기준의 5줄.
constexpr int kWpListRowHeightPx = 24;
constexpr int kWpListVisibleRows = 5;

// "위치 보정" 정착 판단 — robot_position의 pos_std_m(위치 표준편차, 미터)이 이
// 값 이하로 내려오면 AMCL이 충분히 수렴했다고 본다. 시작값은 추정치 — 실
// 로봇(R-07)에서 `ros2 topic echo /amcl_pose`로 수렴 전/후 공분산을 직접 보고
// 맞춰야 한다(AMCL 파라미터·환경마다 다르다).
constexpr double kPoseConvergedStdMeters = 0.3;
// pos_std_m이 끝까지 임계값 밑으로 안 내려오는 경우(필드 유실, 환경이 안 좋아
// AMCL이 못 붙는 경우 등)에 대비한 안전판 — 정상 경로에서는 거의 안 걸린다.
constexpr int kPoseCaptureSettleFallbackMs = 8000;
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
    // "경로 이동" — 웨이포인트 편집 팝오버 안으로 옮겨왔다(예전엔 캔버스 툴바의
    // "로봇 이동" 모드에서 마커를 직접 클릭해야 했다). driveToSingleWaypoint는 그때와
    // 동일한 함수를 그대로 재사용한다.
    connect(popover_, &WaypointPopover::driveRequested, this, &P6PatrolPage::driveToSingleWaypoint);
    connect(popover_, &WaypointPopover::cancelDriveRequested, this, &P6PatrolPage::cancelCurrentDrive);

    connect(canvas_, &WaypointCanvas::waypointAddRequested, this, &P6PatrolPage::handleCanvasAddRequested);
    connect(canvas_, &WaypointCanvas::waypointEditRequested, this, &P6PatrolPage::handleCanvasEditRequested);
    connect(canvas_, &WaypointCanvas::waypointDeleteRequested, this, &P6PatrolPage::handleCanvasDeleteRequested);
    connect(canvas_, &WaypointCanvas::waypointMoved, this, &P6PatrolPage::handleCanvasMoved);
    connect(canvas_, &WaypointCanvas::initialPoseCaptureFinished, this, &P6PatrolPage::handleInitialPoseCaptured);
    connect(canvas_, &WaypointCanvas::initialPoseCaptureCancelled, this,
            [this] { poseCaptureBtn_->setChecked(false); });

    // handleInitialPoseCaptured가 시작하는 "위치 설정 중" 구간의 안전판 —
    // poseCaptureSettling_ 주석 참고. 정상 경로는 handleWsMessage가 pos_std_m으로
    // 먼저 끝내므로, 이 타이머는 그게 안 될 때만 발동한다.
    poseCaptureSettleTimer_ = new QTimer(this);
    poseCaptureSettleTimer_->setSingleShot(true);
    connect(poseCaptureSettleTimer_, &QTimer::timeout, this, &P6PatrolPage::finishPoseCaptureSettling);
    connect(canvas_, &WaypointCanvas::routeWaypointClicked, this, &P6PatrolPage::handleRouteWaypointClicked);
    connect(canvas_, &WaypointCanvas::chargeClickBlocked, this, [this] {
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
        // 편집 중인 웨이포인트가 있으면 닫지 않고 새 스케일의 위치로 다시 채운다 —
        // 예전엔 무조건 hide()했는데, "경로 이동"->"주행 취소" 토글을 팝오버 안에서
        // 유지하는 지금 설계에선 driveToSingleWaypoint 성공 직후 뜨는 "수동 주행 중"
        // 배너가 캔버스 카드 높이를 줄여 곧바로 이 rescale을 유발한다 — 그때마다
        // hide()되면 방금 누른 "경로 이동"이 "주행 취소"로 바뀌는 걸 사용자가 볼 수
        // 없다. 또한 setForEditWaypoint로 canvasPos_를 새 좌표로 갱신하지 않으면
        // 이후 "저장"이 스케일 변경 전(낡은) 좌표로 나가는 문제도 이 김에 같이 막는다.
        const QString editingId = popover_->editingWaypointId();
        if (popover_->isVisible() && !editingId.isEmpty()) {
            if (const WaypointCanvas::Waypoint* wp = canvas_->findWaypoint(editingId)) {
                popover_->setForEditWaypoint(*wp);
                refreshPopoverDriveButton();
                movePopoverNear(canvas_->mapTo(this, canvas_->worldToScreen(wp->pos).toPoint()));
            } else {
                popover_->hide();
            }
        } else {
            popover_->hide();
        }
    });

    loadMapData(0);
    // loadMapData(0)은 maps_가 아직 비어 있으면(서버 응답 전) 맨 위 가드에서 바로
    // 되돌아가 아무 것도 안 한다 — robotOnline_/isPatrolling_ 멤버 기본값(둘 다 false)
    // 자체는 이미 "오프라인·대기"로 안전하지만, 그 값을 실제 위젯(배너·버튼)에
    // 반영하는 건 이 두 함수라서 명시적으로 한 번 불러야 한다. 안 부르면 서버가
    // 아직 응답하기 전까지 "순찰 시작" 버튼이 (기본 활성화 상태 그대로) 눌리는
    // 잠깐의 창이 생긴다.
    updateRobotStatusBanner();
    updateDrivingButtonsEnabled();

    apiClient_ = new ApiClient(this);
    loadMapsFromServer();
}

void P6PatrolPage::setWsClient(WsClient* client) {
    wsClient_ = client;
    connect(wsClient_, &WsClient::messageReceived, this, &P6PatrolPage::handleWsMessage);
    connect(wsClient_, &WsClient::connectionStateChanged, this,
            [this](bool connected) { connectionBanner_->setVisible(!connected); });
}

QFrame* P6PatrolPage::buildConnectionBanner() {
    // P5MapPage::buildConnectionBanner()와 같은 이유·같은 패턴 — WsClient는 앱 전체가
    // 공유하지만, 끊김 배너는 페이지별 위젯이라 각자 들고 있어야 한다.
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
    // P5MapPage::buildRobotOfflineBanner()와 같은 이유·같은 패턴으로 시작했지만, 이제
    // 오프라인 여부 하나만이 아니라 "로봇 상태 바" 역할도 겸한다(updateRobotStatusBanner()
    // 참고) — 아이콘·문구를 매번 다시 만들지 않고 멤버로 들고 있다가 상태가 바뀔 때마다
    // 내용만 바꿔 그린다. 아직 아무 robot_status도 못 받은 시작 시점엔 안전하게
    // 오프라인으로 간주해 보이는 채로 둔다(hide() 안 함).
    auto* banner = new QFrame(this);
    banner->setObjectName("banner");
    banner->setProperty("level", "emergency");
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(10, 7, 10, 7);
    bannerLayout->setSpacing(6);
    robotStatusIcon_ = new QLabel(banner);
    robotStatusIcon_->setPixmap(SimpleIcons::alertCircle(Tokens::EMERGENCY.main, 12).pixmap(12, 12));
    bannerLayout->addWidget(robotStatusIcon_);
    robotStatusText_ =
        new QLabel(QStringLiteral("로봇 오프라인 · 이동·순찰 명령이 전달되지 않습니다"), banner);
    robotStatusText_->setStyleSheet(QStringLiteral("font-weight:500;"));
    bannerLayout->addWidget(robotStatusText_);
    bannerLayout->addStretch();
    return banner;
}

void P6PatrolPage::updateRobotStatusBanner() {
    // 우선순위: 오프라인 > SLAM 매핑 중 > 주행 중(수동/스케줄) > Nav2 미준비 > 정상(숨김).
    // isPatrolling_을 slamActive_ 다음, navRunning_ 앞에 두는 이유 — 순찰이 실제로
    // 진행 중이면 Nav2는 당연히 떠 있는 것이지만, navRunning_이 아직 그 확인을 못
    // 받은 상태일 수 있다(refreshRobotState의 조회들이 서로 독립적으로 비동기라
    // 도착 순서가 다를 수 있음) — 이미 더 구체적으로 아는 사실(주행 중)을 그보다
    // 약한 신호로 덮어쓰면 안 된다.
    if (!robotOnline_) {
        robotOfflineBanner_->setProperty("level", "emergency");
        robotStatusIcon_->setPixmap(SimpleIcons::alertCircle(Tokens::EMERGENCY.main, 12).pixmap(12, 12));
        robotStatusText_->setText(QStringLiteral("로봇 오프라인 · 이동·순찰 명령이 전달되지 않습니다"));
        robotOfflineBanner_->setVisible(true);
    } else if (slamActive_) {
        robotOfflineBanner_->setProperty("level", "attention");
        robotStatusIcon_->setPixmap(SimpleIcons::alertCircle(Tokens::ATTENTION.main, 12).pixmap(12, 12));
        robotStatusText_->setText(
            QStringLiteral("SLAM 매핑 중 · 완료 전까지 이동·순찰 명령을 보낼 수 없습니다"));
        robotOfflineBanner_->setVisible(true);
    } else if (isPatrolling_) {
        robotOfflineBanner_->setProperty("level", "info");
        robotStatusIcon_->setPixmap(SimpleIcons::route(Tokens::INFO.main, 12).pixmap(12, 12));
        // 스케줄 주행 중엔 수동 이동·순찰 버튼이 막히는 이유가 "누군가 방금 순찰
        // 시작을 눌러서"가 아니라 "예약된 스케줄이 돌고 있어서"다 — 운영자가 왜
        // 막혀 있는지 바로 알 수 있게 문구로 구분한다.
        robotStatusText_->setText(activePatrolTrigger_ == QStringLiteral("schedule")
                                       ? QStringLiteral("스케줄 주행 중 · 완료 전까지 새 이동·순찰 명령을 보낼 수 없습니다")
                                       : QStringLiteral("수동 주행 중 · 완료 전까지 새 이동·순찰 명령을 보낼 수 없습니다"));
        robotOfflineBanner_->setVisible(true);
    } else if (!navRunning_) {
        robotOfflineBanner_->setProperty("level", "attention");
        robotStatusIcon_->setPixmap(SimpleIcons::alertCircle(Tokens::ATTENTION.main, 12).pixmap(12, 12));
        robotStatusText_->setText(
            QStringLiteral("Nav2 준비 중이거나 연결할 수 없습니다 · 잠시 후 새로고침해보세요"));
        robotOfflineBanner_->setVisible(true);
    } else {
        robotOfflineBanner_->setVisible(false);
        return;
    }
    // QSS의 [level=...] 선택자가 다시 매칭되도록 스타일을 강제로 다시 계산시킨다 —
    // level 프로퍼티만 바꾸고 이걸 안 하면 이미 폴리시된 위젯이라 색이 안 바뀐다
    // (SlamMappingCard::applyRegisteredMap의 badge_ 처리와 같은 패턴).
    robotOfflineBanner_->style()->unpolish(robotOfflineBanner_);
    robotOfflineBanner_->style()->polish(robotOfflineBanner_);
}

bool P6PatrolPage::canDrive() const {
    return robotOnline_ && !isPatrolling_ && !slamActive_ && navRunning_;
}

void P6PatrolPage::updateDrivingButtonsEnabled() {
    const bool drivable = canDrive();
    testBtn_->setEnabled(drivable);
    testBtn_->setToolTip(drivable ? QString() : QStringLiteral("로봇 ws 연결을 확인해주세요"));
    refreshPopoverDriveButton();
}

void P6PatrolPage::refreshPopoverDriveButton() {
    // 팝오버가 지금 화면에 떠 있고, 그게 보고 있는 웨이포인트가 마침
    // drivingWaypointId_(지금 수동 주행 중인 그 지점)이면 "주행 취소"로, 아니면
    // (로봇 온라인·유휴·SLAM 아님·Nav2 준비됨 여부에 따라) "경로 이동"으로 토글
    // 버튼을 맞춘다 — updateDrivingButtonsEnabled()와 같은 canDrive() 기준을 쓴다.
    if (!popover_->isVisible()) return;
    const QString editingId = popover_->editingWaypointId();
    if (editingId.isEmpty()) return;  // 신규 웨이포인트 팝오버 — 버튼 자체가 숨어 있음.
    const bool isDrivingThis = !drivingWaypointId_.isEmpty() && editingId == drivingWaypointId_;
    popover_->setDriveButtonState(canDrive(), isDrivingThis);
}

void P6PatrolPage::refreshRobotState() {
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const MapData& current = maps_[currentMapIndex_];
    const QString robotCode = current.robotId;
    const int serverRobotId = current.serverRobotId;
    if (robotCode.isEmpty()) return;

    // 실시간 온라인 여부 — GET /robots가 호출마다 다시 계산해서 내려준다
    // (robots.py::is_connected). 응답이 오는 사이 다른 로봇으로 전환됐을 수 있어,
    // 반영 직전 지금도 여전히 같은 로봇을 보고 있는지 다시 확인한다.
    apiClient_->fetchRobots([this, robotCode](bool ok, const QVector<ApiClient::RobotInfo>& robots) {
        if (!ok) return;
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        if (maps_[currentMapIndex_].robotId != robotCode) return;
        for (const ApiClient::RobotInfo& r : robots) {
            if (r.code == robotCode) {
                robotOnline_ = r.isOnline;
                navRunning_ = r.isNavRunning;
                break;
            }
        }
        updateRobotStatusBanner();
        updateDrivingButtonsEnabled();
    });

    if (serverRobotId <= 0) return;  // 서버 미등록(로컬 데모) 맵 — 순찰 기록 조회 대상 자체가 없다.
    // "지금 running 상태인 patrol_run이 있는지" — 결과가 하나라도 있으면 주행 중으로
    // 본다(patrol_task.py가 goal을 하나만 허용해 정상적으로는 0건 또는 1건).
    apiClient_->fetchPatrolRuns(
        serverRobotId, QStringLiteral("running"),
        [this, robotCode](bool ok, const QVector<ApiClient::PatrolRunInfo>& runs) {
            if (!ok) return;
            if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
            if (maps_[currentMapIndex_].robotId != robotCode) return;
            isPatrolling_ = !runs.isEmpty();
            // "정상적으로는 0건 또는 1건"이지만(위 주석), 혹시 고아 레코드가 여러 건
            // 남아있는 경우엔 가장 최근 것(서버가 started_at DESC로 내려줌) 기준으로
            // 문구를 정한다.
            activePatrolTrigger_ = runs.isEmpty() ? QString() : runs.first().trigger;
            // drive-to-point(run_id=0)는 patrol_run 행 자체가 없어 DB 재조회로는 "어느
            // 웨이포인트로 가고 있었는지" 되살릴 수 없다 — 주행 중이 아니게 됐을 때만
            // 확실하게 지운다(주행 중인 채로 새로고침한 경우엔 기존 값을 그대로 둔다:
            // 대부분 이 페이지 안에서 직접 시작한 것이라 이미 맞는 값일 가능성이 높다).
            if (!isPatrolling_) drivingWaypointId_.clear();
            updateRobotStatusBanner();
            updateDrivingButtonsEnabled();
            rebuildWpList();
        });

    // "지금 이 로봇이 SLAM 매핑 중인지" — status='queued'인 slam_job이 하나라도 있으면
    // 매핑 중으로 본다(slam_job.status는 시작 시 'queued'로 들어간 뒤 취소/저장으로만
    // 바뀌므로 'queued'가 곧 "진행 중" — GET /slam-jobs 문서 참고).
    apiClient_->fetchSlamJobs(
        serverRobotId, QStringLiteral("queued"),
        [this, robotCode](bool ok, const QVector<ApiClient::SlamJobInfo>& jobs) {
            if (!ok) return;
            if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
            if (maps_[currentMapIndex_].robotId != robotCode) return;
            slamActive_ = !jobs.isEmpty();
            updateRobotStatusBanner();
            updateDrivingButtonsEnabled();
        });
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

    // 로봇 상태(온라인 여부·주행 중 여부)를 DB/실시간 연결 기준으로 다시 맞춘다 —
    // WS 메시지만으로는 페이지를 새로 열었을 때나 메시지를 놓쳤을 때 실제 상태를
    // 알 길이 없어서 생긴 버튼. 배너가 떠 있을 때만이 아니라 항상 눌러볼 수 있게
    // 헤더에 둔다(상태가 잘못 숨어 있는 경우도 있을 수 있어서).
    refreshRobotStateBtn_ = new QPushButton(SimpleIcons::refresh(Tokens::TEXT_SECONDARY, 12), QString(), row);
    refreshRobotStateBtn_->setProperty("variant", "secondary");
    refreshRobotStateBtn_->setIconSize(QSize(12, 12));
    refreshRobotStateBtn_->setToolTip(QStringLiteral("로봇 상태 새로고침"));
    connect(refreshRobotStateBtn_, &QPushButton::clicked, this, [this] {
        refreshRobotState();
        // 배지가 떠 있을 때만(=staleMapIds_에 지금 맵이 있을 때만) 실제 재조회를
        // 한다 — 평소엔 이 버튼이 로봇 상태만 다시 맞추던 것과 동작이 그대로다.
        if (currentMapIndex_ >= 0 && currentMapIndex_ < maps_.size() &&
            staleMapIds_.contains(maps_[currentMapIndex_].serverMapId)) {
            reloadCurrentMapWaypointsAndRoutes();
        }
    });
    layout->addWidget(refreshRobotStateBtn_);

    // 다른 대시보드가 이 맵의 웨이포인트/경로를 저장했을 때만 나타난다
    // (handleWsMessage의 waypoints_changed/patrol_routes_changed) — 눌러야 하는
    // 이유를 보여주는 용도라, 새로고침 버튼처럼 항상 떠 있지 않고 평소엔 숨어 있다.
    refreshStaleBadge_ = new QLabel(QStringLiteral("웨이포인트/경로 변경됨"), row);
    refreshStaleBadge_->setObjectName("badge");
    refreshStaleBadge_->setProperty("level", "info");
    refreshStaleBadge_->setProperty("size", "xs");
    refreshStaleBadge_->setToolTip(QStringLiteral("새로고침을 누르면 최신 웨이포인트·경로를 받아옵니다"));
    refreshStaleBadge_->setVisible(false);
    layout->addWidget(refreshStaleBadge_);

    layout->addStretch();

    return row;
}

QWidget* P6PatrolPage::buildCanvasCard() {
    // objectName("card")를 안 준다 — P5MapPage::SlamMappingCard와 같은 이유(QFrame#card의
    // 흰 배경+테두리를 두르면 캔버스 자신이 paintEvent에서 그리는 테두리와 겹쳐 "테두리
    // 안에 테두리"로 보였다). 여백(CARD_PADDING)도 없애 캔버스가 이 카드가 차지하는
    // 바깥 박스 크기 그대로 확장되게 한다.
    auto* card = new QFrame(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    canvasToolbarRow_ = new QWidget(card);
    auto* toolbar = new QHBoxLayout(canvasToolbarRow_);
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(6);

    // 예전엔 "WP 편집"/"로봇 이동" 두 모드를 토글로 오갔다 — "로봇 이동"은 웨이포인트
    // 편집 팝오버의 "경로 이동" 버튼으로 옮겨갔고(2026-09-10), 남은 동작(추가·이동·
    // 편집·삭제)은 전부 하나의 상호작용으로 통일돼 더 이상 모드 전환 자체가 필요 없다.
    //
    // "위치 보정"은 드래그 한 번으로 끝나는 단발성 동작이라(캔버스가 알아서 끝나면
    // poseCaptureMode를 꺼버린다 — initialPoseCaptureFinished/Cancelled에서 이 버튼
    // 체크 상태만 되돌려주면 된다) 위 모드 개념과는 원래도 무관했다.
    poseCaptureBtn_ = new QPushButton(QStringLiteral("초기 위치 설정"), canvasToolbarRow_);
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
        QStringLiteral("휠 확대/축소 · 휠 버튼 드래그로 이동 · 더블클릭 원상복귀 — 그 외 안내는 지도 하단 참고"),
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
    // buildRouteCard()의 normalPage와 같은 뼈대(타이틀 행 + 목록 + 하단 안내 라벨)로
    // 맞춘다 — 디자인 통일 요청(2026-09-10).
    wpListCard_ = new QFrame(this);
    wpListCard_->setObjectName("card");
    auto* outer = new QVBoxLayout(wpListCard_);
    outer->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                               Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    outer->setSpacing(6);

    auto* titleRow = new QHBoxLayout();
    wpListTitleLabel_ = new QLabel(QStringLiteral("웨이포인트 0"), wpListCard_);
    wpListTitleLabel_->setProperty("role", "label");
    titleRow->addWidget(wpListTitleLabel_);
    titleRow->addStretch();
    // 체크 상태가 canvas_->addWaypointMode()와 그대로 연결되는 토글이라(위 헤더 주석
    // 참고) rebuildWpList()가 아니라 여기서 한 번만 만든다. 켜져 있는 동안만 지도 위
    // 빈 곳 클릭이 새 웨이포인트 추가로 이어진다(WaypointCanvas::setAddWaypointMode).
    addWpBtn_ = new QPushButton(QStringLiteral("+ 추가"), wpListCard_);
    addWpBtn_->setObjectName("linkBtn");
    addWpBtn_->setCheckable(true);
    addWpBtn_->setToolTip(QStringLiteral("켜면 지도에서 빈 곳을 클릭(드래그로 방향 지정)해 새 웨이포인트를 추가할 수 있습니다."));
    connect(addWpBtn_, &QPushButton::clicked, this, [this](bool checked) {
        canvas_->setAddWaypointMode(checked);
    });
    titleRow->addWidget(addWpBtn_);
    outer->addLayout(titleRow);

    // 5줄까지만 보이고 그 이상은 세로 스크롤 — P6PatrolPage::buildRightColumn()의
    // patrolRightColumnScroll과 같은 이유·같은 패턴(QScrollArea여야 내용이 실제
    // 크기를 유지한 채 넘치는 만큼만 스크롤된다). QScrollArea::setWidget()은 레이아웃이
    // 아니라 위젯을 요구하므로, wpListLayout_을 담을 빈 컨테이너 위젯이 하나 더 필요하다.
    auto* wpListScroll = new QScrollArea(wpListCard_);
    wpListScroll->setFrameShape(QFrame::NoFrame);
    wpListScroll->setWidgetResizable(true);
    wpListScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // setMaximumHeight만으로는 안 된다 — QScrollArea의 sizeHint()가 이미 그보다 작은
    // 값을 돌려줘서 상한 자체가 한 번도 걸릴 일이 없었다(실측: 내용이 6줄이어도
    // 3줄 높이로만 렌더링됨). routeOrderList_->setFixedHeight(120)과 같은 이유로
    // 최小/최大를 모두 이 값으로 고정해 레이아웃이 반드시 이만큼 배정하게 한다.
    wpListScroll->setFixedHeight(kWpListRowHeightPx * kWpListVisibleRows);
    auto* wpListScrollContent = new QWidget(wpListScroll);
    wpListLayout_ = new QVBoxLayout(wpListScrollContent);
    wpListLayout_->setContentsMargins(0, 0, 0, 0);
    wpListLayout_->setSpacing(0);
    wpListScroll->setWidget(wpListScrollContent);
    outer->addWidget(wpListScroll);

    wpTypeHintLabel_ = new QLabel(QStringLiteral("유형: 순찰 지점 · 충전"), wpListCard_);
    wpTypeHintLabel_->setProperty("role", "muted");
    wpTypeHintLabel_->setStyleSheet(QStringLiteral("font-size:11px;"));
    outer->addWidget(wpTypeHintLabel_);

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

    auto* routeSelectRow = new QHBoxLayout();
    routeSelectRow->setSpacing(4);
    routeSelectCombo_ = new CareGuardComboBox(normalPage);
    connect(routeSelectCombo_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (name.isEmpty() || name == currentRoute_) return;
        currentRoute_ = name;
        // rebuildRouteOrderList()가 매번 resetRouteOrderEditUi()를 먼저 부르므로,
        // 다른 경로로 전환하면서 이전 경로 기준의 편집 상태가 그대로 남는 일은 없다.
        rebuildRouteOrderList();
        refreshCanvasPreview();
    });
    routeSelectRow->addWidget(routeSelectCombo_, /*stretch=*/1);

    // "편집" — 누르면 enterRouteOrderEdit()로 진입한다. 편집 중엔 이 버튼이 숨고
    // 대신 routeOrderSaveBtn_/routeOrderCancelBtn_ 쌍이 보인다(resetRouteOrderEditUi 참고).
    routeOrderEditToggleBtn_ = new QPushButton(QStringLiteral("편집"), normalPage);
    routeOrderEditToggleBtn_->setProperty("variant", "secondary");
    connect(routeOrderEditToggleBtn_, &QPushButton::clicked, this, &P6PatrolPage::enterRouteOrderEdit);
    routeSelectRow->addWidget(routeOrderEditToggleBtn_);

    routeOrderSaveBtn_ = new QPushButton(QStringLiteral("저장"), normalPage);
    routeOrderSaveBtn_->setProperty("variant", "primary");
    connect(routeOrderSaveBtn_, &QPushButton::clicked, this, &P6PatrolPage::saveRouteOrderEdit);
    routeOrderSaveBtn_->hide();
    routeSelectRow->addWidget(routeOrderSaveBtn_);

    routeOrderCancelBtn_ = new QPushButton(QStringLiteral("취소"), normalPage);
    routeOrderCancelBtn_->setProperty("variant", "secondary");
    connect(routeOrderCancelBtn_, &QPushButton::clicked, this, &P6PatrolPage::cancelRouteOrderEdit);
    routeOrderCancelBtn_->hide();
    routeSelectRow->addWidget(routeOrderCancelBtn_);
    normalLayout->addLayout(routeSelectRow);

    routeOrderList_ = new QListWidget(normalPage);
    routeOrderList_->setObjectName("routeOrderList");
    // 기본은 드래그 잠금 — "편집"을 눌러야 재정렬할 수 있다(enterRouteOrderEdit).
    routeOrderList_->setDragDropMode(QAbstractItemView::NoDragDrop);
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

    // 편집 중에만 보이는 "+ 추가" 행 — refreshRouteOrderAddCombo()가 콤보를 채운다.
    routeOrderAddRow_ = new QWidget(normalPage);
    auto* addRowLayout = new QHBoxLayout(routeOrderAddRow_);
    addRowLayout->setContentsMargins(0, 0, 0, 0);
    addRowLayout->setSpacing(4);
    routeOrderAddCombo_ = new QComboBox(routeOrderAddRow_);
    addRowLayout->addWidget(routeOrderAddCombo_, /*stretch=*/1);
    routeOrderAddBtn_ = new QPushButton(QStringLiteral("+ 추가"), routeOrderAddRow_);
    routeOrderAddBtn_->setProperty("variant", "secondary");
    connect(routeOrderAddBtn_, &QPushButton::clicked, this, &P6PatrolPage::addWaypointToCurrentRouteOrder);
    addRowLayout->addWidget(routeOrderAddBtn_);
    routeOrderAddRow_->hide();
    normalLayout->addWidget(routeOrderAddRow_);

    routeStatsLabel_ = new QLabel(normalPage);
    routeStatsLabel_->setProperty("role", "muted");
    routeStatsLabel_->setStyleSheet(QStringLiteral("font-size:11px;"));
    routeStatsLabel_->setWordWrap(true);
    normalLayout->addWidget(routeStatsLabel_);

    testBtn_ = new QPushButton(SimpleIcons::radar(Tokens::TEXT_SECONDARY, 11),
                                QStringLiteral("경로 주행"), normalPage);
    testBtn_->setProperty("variant", "secondary");
    testBtn_->setIconSize(QSize(11, 11));
    connect(testBtn_, &QPushButton::clicked, this, &P6PatrolPage::runTestRun);
    normalLayout->addWidget(testBtn_);

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
    formLayout->addWidget(scheduleRouteCombo_);
    formLayout->addSpacing(5);

    // "이 스케줄이 복약 전달용인지" — 예전엔 경로 이름이 "medicine"인지로 판별했는데,
    // 경로 하나당 여부가 고정돼버려 "같은 경로의 스케줄 중 일부만 복약용" 같은 조합이
    // 안 됐다. 스케줄(patrol_schedule) 하나하나의 속성으로 옮겼다(요청 사양).
    scheduleMedicineCheckbox_ = new QCheckBox(QStringLiteral("복약 스케줄"), scheduleForm_);
    connect(scheduleMedicineCheckbox_, &QCheckBox::toggled, this, &P6PatrolPage::onScheduleMedicineToggled);
    formLayout->addWidget(scheduleMedicineCheckbox_);
    formLayout->addSpacing(5);

    // 체크박스가 켜져 있을 때만 나타난다 — onScheduleMedicineToggled 참고.
    scheduleMedLabel_ = new QLabel(QStringLiteral("복약 스케줄 선택"), scheduleForm_);
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
        if (w.kind == QStringLiteral("charge")) return w.id;
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
    // pos는 캔버스 "월드" 좌표(확대/축소·화면 이동 전)다 — mapTo()는 위젯 간 오프셋만
    // 더할 뿐 그 배율·이동은 모르니, worldToScreen()으로 먼저 지금 실제 화면 위치로
    // 바꿔야 한다(WaypointCanvas::worldToScreen 주석 참고).
    const QPoint anchor = canvas_->mapTo(this, canvas_->worldToScreen(pos).toPoint());
    movePopoverNear(anchor);
    popover_->raise();
    popover_->show();
}

void P6PatrolPage::handleCanvasEditRequested(const QString& id) {
    const WaypointCanvas::Waypoint* wp = canvas_->findWaypoint(id);
    if (!wp) return;
    popover_->setForEditWaypoint(*wp);
    const QPoint anchor = canvas_->mapTo(this, canvas_->worldToScreen(wp->pos).toPoint());
    movePopoverNear(anchor);
    popover_->raise();
    popover_->show();
    // "경로 이동"/"주행 취소" 토글 버튼을 지금 상태로 맞춘다. isVisible() 체크가
    // 있는 refreshPopoverDriveButton()이라 반드시 show() 다음에 불러야 한다 — 이후
    // updateDrivingButtonsEnabled()가 다시 불릴 때마다(로봇 상태·주행 상태가
    // 바뀔 때마다) 같은 함수가 또 불려 열려 있는 동안에도 실시간으로 따라간다.
    refreshPopoverDriveButton();
}

void P6PatrolPage::movePopoverNear(const QPoint& anchor) {
    // x·y 둘 다 [8, 이 페이지 크기 - 팝오버 크기 - 8] 범위로 확실히 가둔다 — 예전엔
    // x가 위쪽 경계(qMin)만 있고 아래쪽 경계(qMax)가 없어서, 캔버스를 왼쪽 끝까지
    // 축소·이동한 뒤 거기 있는 마커를 누르면 팝오버가 페이지 왼쪽 밖으로 나가
    // "메인 뷰를 벗어난" 채로(저장·취소 버튼이 화면 밖) 뜨는 경우가 있었다. y는
    // 이미 qMax가 있었지만 같은 이유로 qBound로 통일한다.
    const int x = qBound(8, anchor.x() + 12, width() - popover_->width() - 8);
    const int y = qBound(8, anchor.y() - 20, height() - popover_->height() - 8);
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

void P6PatrolPage::handlePopoverSave(const QString& id, const QString& name, const QString& kind, qreal yaw,
                                      QPointF pos) {
    QString oldChargingCode;  // 교체되어 'patrol'로 내려야 할 예전 충전 웨이포인트(있으면)
    if (kind == QStringLiteral("charge")) {
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
                if (w.id == existing) w.kind = QStringLiteral("patrol");
            }
            oldChargingCode = existing;
        }
    }
    // emergency는 charge와 달리 맵당 여러 개 허용되므로(요청 사양) 여기서는 아무런
    // 유니크 검사도 하지 않는다 — 서버도 kind=='charge'일 때만 맵당 1개 제약을 검사한다
    // (api/v1/map.py::create_waypoint/update_waypoint).

    QString code = id;
    if (id.isEmpty()) {
        code = nextWaypointId();
        WaypointCanvas::Waypoint wp{code, name, kind, pos, yaw};
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
                w.kind = kind;
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
    auto saveThisWaypoint = [this, code, name, kind, pos, yaw]() {
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        MapData& current = maps_[currentMapIndex_];
        const int existingServerId = current.waypointServerIds.value(code, -1);
        const QPointF meters = canvas_->canvasToMapMeters(pos);
        const double yawRad = CanvasGeometry::canvasYawDegToMapYawRad(yaw);
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

    // "웨이포인트 N" 개수만 갱신한다 — 타이틀 라벨과 "+ 추가" 버튼(addWpBtn_) 자체는
    // buildWpListCard()가 한 번만 만든 채로 유지된다(addWpBtn_ 주석 참고: 매번 다시
    // 만들면 체크 상태가 canvas_->addWaypointMode()와 어긋난다).
    wpListTitleLabel_->setText(QStringLiteral("웨이포인트 %1").arg(waypoints_.size()));

    // 구분선으로만 나뉜 단순한 한 줄짜리 행: ID · 이름 · (충전이면 "초기 위치" 배지,
    // 아니면 우측 정렬된 "x, y, yaw°"). 행 자체를 누르면 편집 팝오버가 열린다
    // (eventFilter). 예전엔 이 목록 전체를 흰 배경+테두리로 한 번 더 감쌌는데(wpListCard_
    // 자신이 이미 QFrame#card로 감싸져 있어 "테두리 안에 테두리"였다 — 경로 카드
    // (buildRouteCard())의 routeOrderList_는 이런 이중 테두리 없이 카드 배경 위에 바로
    // 놓여 있다), 그 감싸는 테두리를 없애 경로 카드와 같은 단순한 모양으로 맞춘다.
    // 행 사이 구분선(위 주석의 "구분선")은 그대로 유지한다.
    auto* listFrame = new QFrame(wpListCard_);
    auto* listLayout = new QVBoxLayout(listFrame);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);

    for (int i = 0; i < waypoints_.size(); ++i) {
        const WaypointCanvas::Waypoint& w = waypoints_[i];

        auto* row = new QFrame(listFrame);
        // 지금 수동 주행 중인 그 웨이포인트만 배경을 얹어 하이라이트한다 — 팝오버의
        // "주행 취소" 토글(refreshPopoverDriveButton)과 같은 drivingWaypointId_를
        // 기준으로 삼아, 목록만 봐도 "지금 어디로 가는 중인지" 알 수 있게 한다.
        const bool isDrivingThis = !drivingWaypointId_.isEmpty() && w.id == drivingWaypointId_;
        QString rowStyle = isDrivingThis ? QStringLiteral("background:#E9F2FB;") : QString();
        if (i + 1 < waypoints_.size()) {
            rowStyle += QStringLiteral("border-bottom:1px solid #E3E8ED;");
        }
        if (!rowStyle.isEmpty()) {
            row->setStyleSheet(rowStyle);
        }
        row->setCursor(Qt::PointingHandCursor);
        row->setProperty("wpId", w.id);
        row->installEventFilter(this);

        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 5, 8, 5);
        rowLayout->setSpacing(6);

        const bool isCharge = w.kind == QStringLiteral("charge");
        const bool isEmergency = w.kind == QStringLiteral("emergency");

        auto* idLabel = new QLabel(w.id, row);
        idLabel->setStyleSheet(
            isCharge ? QStringLiteral("font-family:'JetBrains Mono','D2Coding',monospace; "
                                       "color:#0B7A6E; font-size:11px; font-weight:500;")
            : isEmergency ? QStringLiteral("font-family:'JetBrains Mono','D2Coding',monospace; "
                                            "color:#D93B47; font-size:11px; font-weight:500;")
                          : QStringLiteral("font-family:'JetBrains Mono','D2Coding',monospace; "
                                           "color:#5A6774; font-size:11px;"));
        rowLayout->addWidget(idLabel);

        auto* nameLabel = new QLabel(w.name, row);
        nameLabel->setStyleSheet(QStringLiteral("color:#16202B; font-size:11px;"));
        rowLayout->addWidget(nameLabel);

        if (isDrivingThis) {
            auto* drivingBadge = new QLabel(QStringLiteral("주행 중"), row);
            drivingBadge->setStyleSheet(QStringLiteral(
                "color:#2563A8; background:#C2D9F0; font-size:10px; padding:1px 5px; border-radius:3px;"));
            rowLayout->addWidget(drivingBadge);
        }
        rowLayout->addStretch();

        if (isCharge) {
            auto* badge = new QLabel(QStringLiteral("초기 위치"), row);
            badge->setStyleSheet(QStringLiteral(
                "color:#0B7A6E; background:#E0F5F2; font-size:10px; padding:1px 5px; border-radius:3px;"));
            rowLayout->addWidget(badge);
        } else {
            if (isEmergency) {
                auto* badge = new QLabel(QStringLiteral("긴급 버튼"), row);
                badge->setStyleSheet(QStringLiteral(
                    "color:#D93B47; background:#FDECEE; font-size:10px; padding:1px 5px; border-radius:3px;"));
                rowLayout->addWidget(badge);
            }
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
            // 목록 행을 누르면 캔버스에서 마커를 클릭한 것과 동일하게 편집 팝오버를
            // 연다 — 실제 주행 요청은 그 팝오버의 "경로 이동" 버튼이 담당한다.
            handleCanvasEditRequested(wpId.toString());
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

void P6PatrolPage::rebuildRouteSelector() {
    // currentIndexChanged가 항목 재구성 중에 엉뚱한 인덱스로 튀는 걸 막는다 — 실제
    // 전환은 이 함수가 끝난 뒤 아래에서 currentRoute_ 기준으로 다시 맞춘다.
    const QSignalBlocker blocker(routeSelectCombo_);
    routeSelectCombo_->clear();
    routeSelectCombo_->addItems(routes_.keys());
    const int idx = routeSelectCombo_->findText(currentRoute_);
    if (idx >= 0) {
        routeSelectCombo_->setCurrentIndex(idx);
    } else if (routeSelectCombo_->count() > 0) {
        // currentRoute_가 없어졌으면(경로 삭제 등) 첫 항목으로 대신한다.
        currentRoute_ = routeSelectCombo_->itemText(0);
        routeSelectCombo_->setCurrentIndex(0);
    }
}

void P6PatrolPage::rebuildRouteOrderList() {
    // 목록을 통째로 다시 그리는 시점엔 편집 중이던 드래그/추가/삭제가 이미 routes_에
    // 반영돼 있거나(rowsMoved 등) 무의미해진(맵/경로 전환, 웨이포인트 삭제) 상태다 —
    // 매번 안전하게 "편집 꺼짐"으로 되돌려 다음에 다시 "편집"을 눌러야 고칠 수 있게
    // 한다(서버 저장·취소 스냅샷 복원 둘 다 하지 않음 — resetRouteOrderEditUi 참고).
    resetRouteOrderEditUi();
    populateRouteOrderListItems();
}

void P6PatrolPage::populateRouteOrderListItems() {
    routeOrderList_->clear();

    if (currentRoute_.isEmpty()) {
        routeStatsLabel_->setText(QStringLiteral("경로가 없습니다 — \"+ 새 경로\"로 만들어보세요."));
        return;
    }

    for (const QString& id : routes_.value(currentRoute_)) {
        const WaypointCanvas::Waypoint* wp = canvas_->findWaypoint(id);
        const QString text = wp ? QStringLiteral("%1 %2").arg(id, wp->name) : id;
        auto* listItem = new QListWidgetItem(routeOrderList_);
        listItem->setData(Qt::UserRole, id);

        if (!routeOrderEditing_) {
            listItem->setText(text);
            continue;
        }

        // 편집 중엔 텍스트 대신 "이름 + 삭제(×)" 위젯을 붙인다 — rebuildRouteEditList()의
        // 삭제 버튼과 같은 패턴(SimpleIcons::x).
        auto* rowWidget = new QWidget(routeOrderList_);
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(4, 2, 4, 2);
        rowLayout->setSpacing(4);
        auto* nameLabel = new QLabel(text, rowWidget);
        nameLabel->setStyleSheet(QStringLiteral("font-size:11px;"));
        rowLayout->addWidget(nameLabel, /*stretch=*/1);
        auto* removeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_MUTED, 10), QString(), rowWidget);
        removeBtn->setProperty("variant", "ghost");
        removeBtn->setFixedSize(18, 18);
        removeBtn->setIconSize(QSize(10, 10));
        connect(removeBtn, &QPushButton::clicked, this, [this, id] { removeWaypointFromCurrentRouteOrder(id); });
        rowLayout->addWidget(removeBtn);
        listItem->setSizeHint(rowWidget->sizeHint());
        routeOrderList_->setItemWidget(listItem, rowWidget);
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

    rebuildRouteSelector();
    rebuildRouteOrderList();
    refreshCanvasPreview();
}

void P6PatrolPage::handleRouteWaypointClicked(const QString& id) {
    // "편집"(routeOrderList_ 기반) 화면이 지금 열려 있으면 여기서 routes_[currentRoute_]를
    // 직접 토글한다 — routeEditSelection_/rebuildRouteEditList()는 "새 경로"(캔버스
    // 클릭으로 처음부터 순서를 고르는 화면) 전용이라 이쪽에서는 건드리지 않는다.
    // 이 분기가 없으면 canvas_->routeEditMode_가 꺼진 채로 남아(enterRouteOrderEdit
    // 참고) 마커 클릭이 그냥 편집 팝오버(waypointEditRequested)로 새서 실제 웨이포인트가
    // 수정돼버리는 문제가 있었다("경로 편집 중에 웨이포인트가 실제로 수정된다" 리포트).
    if (routeOrderEditing_) {
        QVector<QString> order = routes_.value(currentRoute_);
        if (order.contains(id)) {
            order.removeAll(id);
        } else {
            order.append(id);
        }
        routes_[currentRoute_] = order;
        canvas_->setRouteEditSelection(order);
        populateRouteOrderListItems();
        refreshRouteOrderAddCombo();
        refreshCanvasPreview();
        return;
    }

    if (routeEditSelection_.contains(id)) {
        routeEditSelection_.removeAll(id);
    } else {
        routeEditSelection_.append(id);
    }
    canvas_->setRouteEditSelection(routeEditSelection_);
    rebuildRouteEditList();
}

void P6PatrolPage::startNewRoute() {
    // "복약 전달 경로" 체크박스가 있던 시절엔 QInputDialog에 필드를 추가할 수 없어
    // 직접 QDialog를 만들었는데, 그 체크박스가 "주행 스케줄" 쪽으로 옮겨가면서
    // (onScheduleMedicineToggled 참고) 이름 하나만 받으면 되니 다시 QInputDialog로
    // 되돌린다.
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("새 경로"), QStringLiteral("경로 이름"),
                                                QLineEdit::Normal, QStringLiteral("새 경로"), &ok)
                              .trimmed();
    if (!ok || name.isEmpty()) return;
    enterRouteEditMode(name);
}

void P6PatrolPage::resetRouteOrderEditUi() {
    routeOrderEditing_ = false;
    routeOrderList_->setDragDropMode(QAbstractItemView::NoDragDrop);
    routeOrderEditToggleBtn_->show();
    routeOrderSaveBtn_->hide();
    routeOrderCancelBtn_->hide();
    routeOrderAddRow_->hide();
    // "새 경로"(enterRouteEditMode, 캔버스 클릭으로 처음부터 순서를 고르는 화면)가
    // 지금 따로 열려 있으면(routeEditMode_) 그쪽의 canvas_->setRouteEditMode(true)를
    // 여기서 되돌리면 안 된다 — 이 함수는 이 리스트 편집(routeOrderEditing_) 쪽만
    // 책임진다. 예: 새 경로 편집 중 마커 우클릭 삭제(handleCanvasDeleteRequested)가
    // rebuildRouteOrderList()를 무조건 부르는데, 그때 이 함수가 새 경로 편집까지
    // 꺼버리면 "삭제 한 번에 편집 화면이 튕긴다" 버그가 생긴다.
    if (!routeEditMode_) canvas_->setRouteEditMode(false);
}

void P6PatrolPage::enterRouteOrderEdit() {
    if (currentRoute_.isEmpty()) return;
    routeOrderEditing_ = true;
    // 취소 시 드래그·추가·삭제를 전부 무시하고 되돌릴 기준점.
    routeOrderEditSnapshot_ = routes_.value(currentRoute_);

    routeOrderList_->setDragDropMode(QAbstractItemView::InternalMove);
    routeOrderEditToggleBtn_->hide();
    routeOrderSaveBtn_->show();
    routeOrderCancelBtn_->show();
    routeOrderAddRow_->show();

    // 캔버스도 "경로 편집" 모드로 켠다 — 안 켜면 마커 클릭이 routeWaypointClicked가
    // 아니라 waypointEditRequested로 새서 편집 팝오버가 뜨고, 거기서 저장하면 실제
    // 웨이포인트가 수정돼버린다("경로 편집 중 웨이포인트가 실제로 수정된다" 리포트).
    // 켜두면 마커 클릭이 handleRouteWaypointClicked(routeOrderEditing_ 분기)로 가서
    // 추가/삭제로만 동작하고, charge는 chargeClickBlocked로 막힌다.
    canvas_->setRouteEditMode(true);
    canvas_->setRouteEditSelection(routes_.value(currentRoute_));

    refreshRouteOrderAddCombo();
    populateRouteOrderListItems();  // 편집 중 표시(삭제 버튼)로 다시 그린다.
}

void P6PatrolPage::saveRouteOrderEdit() {
    // 지금 routeOrderList_/routes_[currentRoute_] 순서(드래그·추가·삭제가 이미 다
    // 반영된 상태)를 서버(patrol_route.steps)에 반영한다. 예전엔 드래그로 순서를
    // 바꿔도(rowsMoved) routes_(화면)에만 반영되고 서버에는 전혀 안 실렸다 —
    // "경로 순서를 바꿔도 DB에 안 들어간다" 리포트(2026-09-10).
    if (currentRoute_.isEmpty() || currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) {
        resetRouteOrderEditUi();
        populateRouteOrderListItems();
        return;
    }
    MapData& current = maps_[currentMapIndex_];
    const int routeId = current.routeServerIds.value(currentRoute_, -1);
    if (routeId <= 0) {
        QMessageBox::warning(this, QStringLiteral("경로 저장 실패"),
                              QStringLiteral("이 경로가 아직 서버에 저장되지 않았습니다. \"+ 새 경로\"로 다시 "
                                             "만들어 저장한 뒤 편집해 주세요."));
        return;
    }
    QJsonArray stepsArray;
    for (const QString& code : routes_.value(currentRoute_)) {
        const int wpId = current.waypointServerIds.value(code, -1);
        if (wpId > 0) stepsArray.append(wpId);
    }
    apiClient_->updatePatrolRoute(routeId, QJsonObject{{"steps", stepsArray}},
                                   [this](bool ok, const ApiClient::PatrolRouteInfo&, const QString& err) {
                                       if (!ok) {
                                           QMessageBox::warning(
                                               this, QStringLiteral("경로 저장 실패"),
                                               err.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다.") : err);
                                       }
                                   });
    resetRouteOrderEditUi();
    populateRouteOrderListItems();  // 편집 표시(삭제 버튼) 없이 다시 그린다.
}

void P6PatrolPage::cancelRouteOrderEdit() {
    // 드래그·추가·삭제를 전부 무시하고 편집 진입 시점 상태로 되돌린다 — 서버 호출 없음.
    if (!currentRoute_.isEmpty()) {
        routes_[currentRoute_] = routeOrderEditSnapshot_;
    }
    resetRouteOrderEditUi();
    populateRouteOrderListItems();
    refreshCanvasPreview();
}

void P6PatrolPage::addWaypointToCurrentRouteOrder() {
    if (currentRoute_.isEmpty()) return;
    const QString id = routeOrderAddCombo_->currentData().toString();
    if (id.isEmpty()) return;

    QVector<QString> order = routes_.value(currentRoute_);
    if (order.contains(id)) return;  // 콤보가 이미 제외했어야 하지만 방어적으로.
    order.append(id);
    routes_[currentRoute_] = order;

    // handleRouteWaypointClicked(routeOrderEditing_ 분기)와 같은 이유 — 캔버스가 들고
    // 있는 선택 상태도 같이 갱신해야 지도 위 마커 색·순번 배지가 즉시 바뀐다. 이걸
    // 빠뜨리면 리스트/콤보는 바뀌는데 지도는 그대로라 "추가/삭제가 지도에 안 보인다"
    // 는 문제가 생긴다(2026-09-11 리포트).
    canvas_->setRouteEditSelection(order);
    populateRouteOrderListItems();
    refreshRouteOrderAddCombo();  // 방금 추가한 항목을 후보에서 뺀다.
    refreshCanvasPreview();
}

void P6PatrolPage::removeWaypointFromCurrentRouteOrder(const QString& id) {
    if (currentRoute_.isEmpty()) return;
    QVector<QString> order = routes_.value(currentRoute_);
    order.removeAll(id);
    routes_[currentRoute_] = order;

    // addWaypointToCurrentRouteOrder와 같은 이유 — 캔버스 선택 상태 동기화.
    canvas_->setRouteEditSelection(order);
    populateRouteOrderListItems();
    refreshRouteOrderAddCombo();  // 방금 뺀 항목을 다시 추가 후보에 올린다.
    refreshCanvasPreview();
}

void P6PatrolPage::refreshRouteOrderAddCombo() {
    routeOrderAddCombo_->clear();
    if (!currentRoute_.isEmpty()) {
        const QVector<QString> inRoute = routes_.value(currentRoute_);
        for (const WaypointCanvas::Waypoint& w : waypoints_) {
            // charge(충전 스테이션)는 자동 출발·복귀 전용이라 경로에 못 넣는다
            // (server/app/api/v1/patrol.py::_reject_charge_in_steps와 같은 규칙).
            // patrol/emergency는 둘 다 후보 — emergency도 patrol처럼 경로에 넣을 수 있다.
            if (w.kind == QStringLiteral("charge")) continue;
            if (inRoute.contains(w.id)) continue;
            routeOrderAddCombo_->addItem(QStringLiteral("%1 %2").arg(w.id, w.name), w.id);
        }
    }
    routeOrderAddBtn_->setEnabled(routeOrderAddCombo_->count() > 0);
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

    rebuildRouteSelector();
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
                                      err.isEmpty() ? QStringLiteral("로봇 ws 연결을 확인해주세요.") : err);
                return;
            }
            if (!run.robotConnected) {
                QMessageBox::information(this, QStringLiteral("순찰 시작"),
                                          QStringLiteral("로봇 ws 연결을 확인해주세요."));
                return;
            }
            // 실제로 로봇에 명령이 나갔다 — patrol_done(WS)이 올 때까지 이 로봇에
            // 새 주행 명령(순찰 시작/경로 이동)을 막는다. patrol_task.py의 액션
            // 서버가 동시 목표를 REJECT하므로 안 막아도 로봇이 두 개를 같이 하진
            // 않지만, 그 전에 버튼을 잠가 "왜 안 되는지" 미리 알린다.
            isPatrolling_ = true;
            activePatrolTrigger_ = QStringLiteral("manual");  // 이 버튼이 보내는 건 항상 수동 트리거.
            updateRobotStatusBanner();
            updateDrivingButtonsEnabled();
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
                              [this, id](bool ok, const QString& err) {
                                  if (!ok) {
                                      QMessageBox::warning(
                                          this, QStringLiteral("이동 실패"),
                                          err.isEmpty() ? QStringLiteral("로봇 ws 연결을 확인해주세요.") : err);
                                      return;
                                  }
                                  // runTestRun()과 같은 이유 — 이 "이동"도 patrol_task.py
                                  // 액션을 그대로 쓰므로(run_id=0) patrol_done이 올 때까지
                                  // 같은 방식으로 잠근다. run_id=0이라 patrol_run 행 자체가
                                  // 없어 서버에 물어볼 trigger가 없다 — 대시보드에서 직접
                                  // 누른 동작이니 "manual"로 둔다.
                                  isPatrolling_ = true;
                                  activePatrolTrigger_ = QStringLiteral("manual");
                                  drivingWaypointId_ = id;
                                  updateRobotStatusBanner();
                                  updateDrivingButtonsEnabled();  // refreshPopoverDriveButton()도 여기서 같이 불린다.
                                  rebuildWpList();
                              });
}

void P6PatrolPage::cancelCurrentDrive(const QString& id) {
    // 팝오버가 "주행 취소"를 보내는 시점엔 id == drivingWaypointId_가 항상 성립하지만
    // (같은 팝오버가 그 상태를 보고 토글을 만든 것이므로), 방어적으로 파라미터 자체는
    // 안 쓰고 drivingWaypointId_ 기준으로 정리한다 — 취소는 "지금 이 로봇이 하고 있는
    // 주행 전체"를 멈추는 것이지, 특정 웨이포인트 id에 종속된 개념이 아니다.
    Q_UNUSED(id);
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const QString robotCode = maps_[currentMapIndex_].robotId;
    if (robotCode.isEmpty()) return;

    apiClient_->cancelPatrolRun(robotCode, [this](bool ok, const QString& err) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("주행 취소 실패"),
                                  err.isEmpty() ? QStringLiteral("로봇 ws 연결을 확인해주세요.") : err);
            return;
        }
        // 서버는 로봇 연결 성공 여부와 무관하게 DB의 running 기록을 항상 정리하므로
        // (patrol.py::cancel_patrol_run 참고), 대시보드도 robot_connected 값과 무관하게
        // "주행 중" 상태를 여기서 확정적으로 해제한다.
        isPatrolling_ = false;
        activePatrolTrigger_.clear();
        drivingWaypointId_.clear();
        updateRobotStatusBanner();
        updateDrivingButtonsEnabled();
        rebuildWpList();
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

    // 방금 지정한 자리를 바로 보여준다(응답을 기다리지 않음) — 로봇이 실제로 이
    // 좌표를 받아 AMCL 재수렴을 마치기까지 걸리는 동안 들어오는 robot_position은
    // 파티클이 다시 모이는 중간값이라 마커가 흔들려 보인다. handleWsMessage가
    // pos_std_m(위치 표준편차)으로 실제 수렴을 확인할 때까지 그 중간값들을
    // 무시하고 이 자리를 그대로 유지한다 — poseCaptureSettling_ 주석 참고.
    canvas_->setCurrentRobotPosition(canvasPos, headingDeg);
    poseCaptureSettling_ = true;
    poseCaptureBtn_->setEnabled(false);
    poseCaptureBtn_->setText(QStringLiteral("위치 설정 중..."));
    poseCaptureSettleTimer_->start(kPoseCaptureSettleFallbackMs);

    const QPointF meters = canvas_->canvasToMapMeters(canvasPos);
    apiClient_->setInitialPose(current.robotId, current.serverMapId, meters.x(), meters.y(),
                                CanvasGeometry::canvasYawDegToMapYawRad(headingDeg),
                                [this](bool ok, const QString& err) {
                                    if (!ok) {
                                        // 실패(로봇 미연결 등)했으면 실제로 재수렴이 일어나지
                                        // 않으므로 굳이 기다릴 이유가 없다 — 바로 되돌린다.
                                        finishPoseCaptureSettling();
                                        QMessageBox::warning(
                                            this, QStringLiteral("초기 위치 설정 실패"),
                                            err.isEmpty() ? QStringLiteral("로봇 ws 연결을 확인해주세요.") : err);
                                    }
                                });
}

void P6PatrolPage::finishPoseCaptureSettling() {
    poseCaptureSettleTimer_->stop();
    poseCaptureSettling_ = false;
    poseCaptureBtn_->setEnabled(true);
    poseCaptureBtn_->setText(QStringLiteral("초기 위치 설정"));
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
        // 새로 여는 폼은 항상 "복약 아님"에서 시작한다 — setChecked(false)가 이미
        // false인 상태로도 조용히 넘어가므로, 방금 닫은 폼이 체크돼 있었어도 안전하다.
        scheduleMedicineCheckbox_->blockSignals(true);
        scheduleMedicineCheckbox_->setChecked(false);
        scheduleMedicineCheckbox_->blockSignals(false);
        scheduleMedLabel_->hide();
        scheduleMedCombo_->hide();
        currentRobotMedSchedules_.clear();
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
    scheduleRouteCombo_->clear();
    scheduleRouteCombo_->addItems(routes_.keys());
    const int routeIdx = scheduleRouteCombo_->findText(sched.routeName);
    if (routeIdx >= 0) scheduleRouteCombo_->setCurrentIndex(routeIdx);
    // PUT /patrol-schedules는 route_id 변경을 지원하지 않아 편집 중엔 경로를 고정한다.
    scheduleRouteCombo_->setEnabled(false);
    const QTime t = QTime::fromString(sched.timeText, QStringLiteral("HH:mm"));
    scheduleTimeEdit_->setTime(t.isValid() ? t : QTime(10, 0));
    for (int i = 0; i < scheduleDayChips_.size(); ++i) scheduleDayChips_[i]->setChecked(sched.days.contains(i));
    scheduleSaveBtn_->setText(QStringLiteral("수정 저장"));

    // 체크박스가 onScheduleMedicineToggled를 그대로 다시 태우면 scheduleEditIndex_
    // 기준 매칭 대신 "선택 안 함"부터 다시 조회하게 되므로, 세팅만 하고 로딩은 아래에서
    // 직접(수정 모드용 matchId를 넘겨) 한 번만 한다.
    scheduleMedicineCheckbox_->blockSignals(true);
    scheduleMedicineCheckbox_->setChecked(sched.medicationScheduleId > 0);
    scheduleMedicineCheckbox_->blockSignals(false);
    scheduleMedLabel_->setVisible(sched.medicationScheduleId > 0);
    scheduleMedCombo_->setVisible(sched.medicationScheduleId > 0);
    if (sched.medicationScheduleId > 0) {
        loadMedicationSchedulesForCurrentRobot(sched.medicationScheduleId);
    } else {
        currentRobotMedSchedules_.clear();
    }
}

void P6PatrolPage::onScheduleMedicineToggled(bool checked) {
    scheduleMedLabel_->setVisible(checked);
    scheduleMedCombo_->setVisible(checked);
    if (!checked) {
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

    // "복약 스케줄" 체크박스가 켜져 있으면 지금 콤보 선택 상태가 그대로 매칭 결과다 —
    // "선택 안 함"이면 매칭 해제(0/null), 실제 항목이면 그 medication_schedule.id.
    // 추가·수정 둘 다 이 값 그대로를 저장한다(patrol_schedule.medication_schedule_id,
    // 경로가 아니라 이 회차 자체의 속성).
    int medicationScheduleId = 0;
    if (scheduleMedicineCheckbox_->isChecked()) {
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
            // medication_schedule_id는 체크박스를 끈 경우(0)에도 항상 같이 보내야
            // 서버에 남아있던 이전 매칭이 null로 지워진다 — 예전엔 경로 이름이
            // 고정돼 있어 체크박스를 편집 중 끌 수 없었지만, 지금은 스케줄 하나하나의
            // 속성이라 편집 중에도 켰다 끌 수 있다.
            QJsonObject fields{{"run_time", timeText + QStringLiteral(":00")}, {"days_of_week", daysBitmask}};
            fields["medication_schedule_id"] =
                medicationScheduleId > 0 ? QJsonValue(medicationScheduleId) : QJsonValue();
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

    // 맵(=로봇)이 바뀌면 이전 로봇의 온라인·주행 상태를 그대로 들고 있으면 안 된다 —
    // 실제 값을 다시 받아올 때까지는 안전하게 오프라인/대기로 되돌려 둔다.
    robotOnline_ = false;
    isPatrolling_ = false;
    activePatrolTrigger_.clear();
    drivingWaypointId_.clear();
    slamActive_ = false;
    navRunning_ = false;
    updateRobotStatusBanner();
    updateDrivingButtonsEnabled();
    // 카메라도 마찬가지 — 이전 로봇의 마지막 프레임이 남아있으면 새 로봇 화면인데
    // 정지된 이전 영상 + "연결 끊김" 배지가 뜬다. 이 로봇의 camera_frame이 새로
    // 도착하기 전까지는 "카메라 연결 중" placeholder로 되돌려 둔다.
    canvas_->clearCameraFrame();
    // WS 이벤트만 기다리면 페이지를 새로 열었을 때(또는 이 로봇으로 막 전환했을 때)
    // "지금 실제로 어떤 상태인지"를 알 길이 없다 — DB/실시간 연결 기준으로 다시
    // 맞춘다(새로고침 버튼과 같은 함수).
    refreshRobotState();
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
    // 이전 로봇(맵)의 "현재 위치" 마커를 먼저 지운다 — 아래에서 이 맵에 충전
    // 웨이포인트가 있으면 바로 다시 채워지므로 화면상 깜빡임은 없다. 이걸 안 하면
    // 이 맵에 아직 충전 웨이포인트가 없는 경우(findWaypoint(chargeId)==nullptr) 아래
    // 블록이 스킵되어 이전 로봇의 마커가 좌표 그대로 남아있는다(오프라인 로봇으로
    // 전환했는데 엉뚱한 위치에 로봇이 있는 것처럼 보이던 버그, 2026-09-14 리포트).
    canvas_->clearRobotPosition();
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
    rebuildRouteSelector();
    rebuildRouteOrderList();
    rebuildScheduleList();
    refreshCanvasPreview();
    updateRefreshBadge();
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
            {QStringLiteral("WP_01"), QStringLiteral("충전 스테이션"), QStringLiteral("charge"),
             QPointF(90, 260), 0}};
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

void P6PatrolPage::updateRefreshBadge() {
    const bool stale = currentMapIndex_ >= 0 && currentMapIndex_ < maps_.size() &&
                        staleMapIds_.contains(maps_[currentMapIndex_].serverMapId);
    refreshStaleBadge_->setVisible(stale);
}

void P6PatrolPage::reloadCurrentMapWaypointsAndRoutes() {
    if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
    const int mapId = maps_[currentMapIndex_].serverMapId;
    if (mapId <= 0) return;

    // 서버 값으로 통째로 교체하는 이상 로컬 편집과의 델타 계산은 무의미하다 —
    // cancelRouteOrderEdit/exitRouteEditMode(false) 둘 다 서버 호출 없이 편집 UI만
    // 끈다(switchToMap이 맵 전환 시 이미 같은 방식으로 쓰는 패턴).
    if (routeOrderEditing_) cancelRouteOrderEdit();
    if (routeEditMode_) exitRouteEditMode(false);

    apiClient_->fetchWaypoints(mapId, [this, mapId](bool ok, const QVector<ApiClient::WaypointInfo>& serverWaypoints) {
        // 응답이 오는 사이 다른 맵으로 전환했을 수 있다 — loadServerMapImageDone과
        // 같은 이유로 매번 다시 확인한다.
        if (!ok || currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        if (maps_[currentMapIndex_].serverMapId != mapId) return;

        QVector<WaypointCanvas::Waypoint> newWaypoints;
        QMap<QString, int> newWaypointServerIds;
        QMap<int, QString> codeById;
        int nextSeq = 1;
        for (const ApiClient::WaypointInfo& w : serverWaypoints) {
            const QPointF meters(w.posX, w.posY);
            // 이미 떠 있는 현재 맵이라 designMapMetersToCanvas(초기 로딩 전용 폴백)
            // 대신 canvas_의 live 변환을 그대로 쓸 수 있다.
            newWaypoints.append(WaypointCanvas::Waypoint{
                w.code, w.name, w.kind, canvas_->mapMetersToCanvas(meters),
                CanvasGeometry::mapYawRadToCanvasYawDeg(w.yaw), meters});
            newWaypointServerIds[w.code] = w.id;
            codeById[w.id] = w.code;
            if (w.code.startsWith(QStringLiteral("WP_"))) {
                bool convOk = false;
                const int n = w.code.mid(3).toInt(&convOk);
                if (convOk) nextSeq = qMax(nextSeq, n + 1);
            }
        }

        apiClient_->fetchPatrolRoutes(
            mapId, [this, mapId, newWaypoints, newWaypointServerIds, codeById, nextSeq](
                       bool routesOk, const QVector<ApiClient::PatrolRouteInfo>& serverRoutes) {
                if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
                if (maps_[currentMapIndex_].serverMapId != mapId) return;
                MapData& md = maps_[currentMapIndex_];

                QMap<QString, QVector<QString>> newRoutes;
                QMap<QString, int> newRouteServerIds;
                if (routesOk) {
                    for (const ApiClient::PatrolRouteInfo& r : serverRoutes) {
                        QVector<QString> codes;
                        for (int waypointId : r.steps) codes.append(codeById.value(waypointId));
                        newRoutes[r.name] = codes;
                        newRouteServerIds[r.name] = r.id;
                    }
                }

                waypoints_ = newWaypoints;
                nextWpSeq_ = nextSeq;
                md.waypoints = newWaypoints;
                md.waypointServerIds = newWaypointServerIds;
                md.nextWpSeq = nextSeq;

                // routesOk가 false면(조회 실패) 기존 routes_를 그대로 둔다 — 실패를
                // "경로가 전부 삭제됨"으로 잘못 해석하면 안 되므로.
                if (routesOk) {
                    routes_ = newRoutes;
                    md.routes = newRoutes;
                    md.routeServerIds = newRouteServerIds;
                    if (newRoutes.contains(currentRoute_)) {
                        // 그대로 유지
                    } else if (!newRoutes.isEmpty()) {
                        currentRoute_ = newRoutes.firstKey();
                    } else {
                        currentRoute_.clear();
                    }
                    md.currentRoute = currentRoute_;
                }

                canvas_->setWaypoints(waypoints_);
                canvas_->requestMetersResync();
                rebuildWpList();
                rebuildRouteSelector();
                rebuildRouteOrderList();
                refreshCanvasPreview();

                staleMapIds_.remove(mapId);
                updateRefreshBadge();
            });
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
                const QPointF meters(w.posX, w.posY);
                md.waypoints.append(WaypointCanvas::Waypoint{
                    w.code, w.name, w.kind, designMapMetersToCanvas(md, meters),
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
        const QString robotCode = message.value(QStringLiteral("robot_code")).toString();
        refreshMapForRobot(robotCode);
        if (currentMapIndex_ >= 0 && currentMapIndex_ < maps_.size() &&
            maps_[currentMapIndex_].robotId == robotCode) {
            slamActive_ = false;
            updateRobotStatusBanner();
            updateDrivingButtonsEnabled();
        }
        return;
    }

    if (type == QStringLiteral("map_changed")) {
        // 다른 대시보드가 "파일로 등록"으로 이 로봇의 맵을 새로 올렸을 수 있다 —
        // slam_done과 같은 이유·같은 경로로 반영한다.
        refreshMapForRobot(message.value(QStringLiteral("robot_code")).toString());
        return;
    }

    if (type == QStringLiteral("robot_recipient_changed")) {
        // 담당 대상자가 재배정됐을 수 있다 — 맵 메뉴에 곁들여 보여주는 대상자
        // 이름(rebuildMapMenu가 쓰는 recipientName)을 새로 받아온다. robot_id만
        // 오므로 maps_에서 같은 로봇의 코드를 찾아 slam_done과 같은 경로로 새로고침한다.
        const int robotId = message.value(QStringLiteral("robot_id")).toInt();
        for (const MapData& m : maps_) {
            if (m.serverRobotId == robotId) {
                refreshMapForRobot(m.robotId);
                break;
            }
        }
        return;
    }

    if (type == QStringLiteral("waypoints_changed") || type == QStringLiteral("patrol_routes_changed")) {
        // 다른 대시보드가 이 맵의 웨이포인트/경로를 저장했다 — 자동으로 캔버스를
        // 갈아치우지 않고(reloadCurrentMapWaypointsAndRoutes 주석 참고) 배지만 띄운다.
        const int mapId = message.value(QStringLiteral("map_id")).toInt();
        if (mapId > 0) {
            staleMapIds_.insert(mapId);
            updateRefreshBadge();
        }
        return;
    }

    if (type == QStringLiteral("slam_progress")) {
        // 지금은 "SLAM 매핑 중" 상태를 refreshRobotState()의 GET /slam-jobs 조회로
        // 이미 잡아내지만(페이지를 열 때/맵 전환/새로고침), P5에서 이 페이지를 열기
        // 전에 이미 SLAM을 시작한 경우 patrol_progress와 같은 이유로 아직 못 잡았을
        // 수 있다 — 1Hz로 오는 이 메시지가 그 공백을 메운다.
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        if (maps_[currentMapIndex_].robotId != message.value(QStringLiteral("robot_code")).toString()) return;
        if (!slamActive_) {
            slamActive_ = true;
            updateRobotStatusBanner();
            updateDrivingButtonsEnabled();
        }
        return;
    }

    if (type == QStringLiteral("nav_status")) {
        // downlink_node._send_nav_status — Nav2가 실제로 준비됐는지/내려갔는지를
        // 알려주는 유일한 신호다(DB에 남지 않는 실시간 전용 메시지라 초기 동기화는
        // nav_prepare 응답에 기댄다 — nav_status 주석 참고).
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        if (maps_[currentMapIndex_].robotId != message.value(QStringLiteral("robot_code")).toString()) return;
        navRunning_ = message.value(QStringLiteral("running")).toBool();
        updateRobotStatusBanner();
        updateDrivingButtonsEnabled();
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
        if (!robotOnline_) {
            // 오프라인이 되면 이 로봇의 주행 상태를 더 이상 신뢰할 수 없다(patrol_done을
            // 영영 못 받을 수 있다) — 다시 온라인이 되면 사용자가 재시도할 수 있도록
            // 안전하게 "주행 중 아님"으로 되돌린다.
            isPatrolling_ = false;
            activePatrolTrigger_.clear();
            drivingWaypointId_.clear();
            slamActive_ = false;
            navRunning_ = false;
            rebuildWpList();
        }
        updateRobotStatusBanner();
        updateDrivingButtonsEnabled();
        return;
    }

    if (type == QStringLiteral("patrol_done")) {
        // 서버 ws/robot_ws.py가 patrol_task.py의 patrol_done을 그대로 중계한 것 —
        // 진행 상황이 DB(patrol_run.status)에는 이미 기록되고 있었지만, 이 메시지
        // 자체를 대시보드가 받아본 적이 없어서 화면(상태 바·버튼 잠금)엔 전혀
        // 반영되지 않았다.
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        if (maps_[currentMapIndex_].robotId != message.value(QStringLiteral("robot_code")).toString()) return;
        isPatrolling_ = false;
        activePatrolTrigger_.clear();
        drivingWaypointId_.clear();
        updateRobotStatusBanner();
        updateDrivingButtonsEnabled();
        rebuildWpList();
        return;
    }

    if (type == QStringLiteral("patrol_progress")) {
        // 지금은 "주행 중" 상태를 startPatrolRun/driveToPoint 성공 시점에 이미 true로
        // 켜두므로(그 사이 공백을 없애기 위해) 이 메시지로 새로 켤 건 없다 — 다만 혹시
        // 그 시작 시점을 놓쳤을 경우(예: 페이지를 새로 연 사이 이미 진행 중이던 순찰,
        // 또는 스케줄 자동 트리거처럼 이 페이지가 직접 시작하지 않은 주행)를 대비해
        // 반영해둔다. 이 메시지 자체엔 trigger가 안 실려 있어(robot_ws.py 참고)
        // "manual"/"schedule" 문구를 정확히 가르려면 refreshRobotState()로 DB를
        // 다시 조회해야 한다.
        if (currentMapIndex_ < 0 || currentMapIndex_ >= maps_.size()) return;
        if (maps_[currentMapIndex_].robotId != message.value(QStringLiteral("robot_code")).toString()) return;
        if (!isPatrolling_) refreshRobotState();
        return;
    }

    if (type != QStringLiteral("robot_position") && type != QStringLiteral("camera_frame") &&
        type != QStringLiteral("patrol_path_update") && type != QStringLiteral("slam_scan_update"))
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

    if (type == QStringLiteral("slam_scan_update")) {
        // uplink_node.py가 P5(SLAM)뿐 아니라 map->base_link TF가 존재하는 동안(=AMCL로
        // 순찰 중일 때도) 계속 보내는 라이다 스캔 오버레이 — P5MapPage::handleWsMessage의
        // 같은 처리와 동일하게 파싱해 캔버스에 그대로 넘긴다. 로봇 초기 위치(AMCL)를
        // 아직 안 잡았으면 로봇 쪽에서 TF 조회가 실패해 이 메시지 자체가 안 오므로,
        // "초기 위치 설정 후 자연히 나타난다"는 동작은 이 페이지에서 별도 처리 없이 그대로 이어진다.
        const QJsonArray flat = message.value(QStringLiteral("points")).toArray();
        QVector<QPointF> scanMeters;
        scanMeters.reserve(flat.size() / 2);
        for (int i = 0; i + 1 < flat.size(); i += 2) {
            scanMeters.append(QPointF(flat.at(i).toDouble(), flat.at(i + 1).toDouble()));
        }
        canvas_->setLiveScan(scanMeters);
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

    // "위치 보정" 직후 정착 구간(poseCaptureSettling_) 동안은 pos_std_m(위치
    // 표준편차, 미터)이 kPoseConvergedStdMeters 이하로 내려온 값이 올 때까지
    // AMCL이 재수렴 중인 중간값을 무시한다 — handleInitialPoseCaptured가 이미
    // 사용자가 지정한 자리를 보여주고 있으므로, 여기서 신뢰 못 할 값으로 계속
    // 갱신하면 그 자리가 흔들리는 것처럼 보인다. pos_std_m이 없는 값(예: 필드
    // 유실)도 아직 못 믿는 값으로 취급해 계속 무시한다 — 안전판 타이머
    // (poseCaptureSettleTimer_)가 그런 경우를 대비한다.
    if (poseCaptureSettling_) {
        const QJsonValue posStd = message.value(QStringLiteral("pos_std_m"));
        if (!posStd.isDouble() || posStd.toDouble() > kPoseConvergedStdMeters) return;
        // 충분히 수렴했다 — 정착을 끝내고 이 값을 그대로 마커에 반영한다(아래로 계속).
        finishPoseCaptureSettling();
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
