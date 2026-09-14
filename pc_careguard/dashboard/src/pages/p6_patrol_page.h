#pragma once
#include <QImage>
#include <QMap>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <memory>

#include "net/api_client.h"
#include "widgets/waypoint_canvas.h"

class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
class CareGuardComboBox;
class QCheckBox;
class QListWidget;
class QTimeEdit;
class QVBoxLayout;
class QHBoxLayout;
class QStackedLayout;
class WaypointPopover;
class QKeyEvent;
class QShowEvent;
class QMenu;
class QFrame;
class WsClient;
class QJsonObject;
class TeleopPanel;
class QKeyEvent;

// P6 순찰·경로 — 웨이포인트 편집 캔버스 + WP 목록/경로/스케줄.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 5), §7 참고.
class P6PatrolPage : public QWidget {
    Q_OBJECT
public:
    void setAuthToken(const QString& token) { apiClient_->setAuthToken(token); }
    explicit P6PatrolPage(QWidget* parent = nullptr);

public slots:
    // robotId가 이미 있는 맵이면 이름·대상자·배경 이미지·서버 id·좌표 환산 기준을
    // 갱신하고, 없으면 새 맵으로 추가한다. serverMapId/resolution/originMeters를 같이
    // 갱신해야 한다 — 예전엔 이 값들을 안 건드려서(이름·이미지만 갱신) "파일로 등록"
    // 으로 다른 지도를 새로 불러와도 로봇은 여전히 예전 map_id로 알고 있어 Nav2(그
    // 안의 rviz2)가 안 바뀌는 버그가 있었다("파일로 등록으로 다른 파일 불러오면
    // rviz2가 안 바뀐다" 리포트, 2026-09-08). 이 맵이 지금 화면에 떠 있는 맵이면
    // prepareNavigationForCurrentMap()으로 로봇에 새 map_id를 알려 Nav2를 다시 띄우게 한다.
    void upsertMap(const QImage& mapImage, const QString& mapName, const QString& robotId,
                    const QString& recipientName, int serverMapId, int serverRobotId, double resolution,
                    QPointF originMeters);
    // GET /maps/active에서 이 robotCode의 최신 맵 정보를 다시 조회해 upsertMap()으로
    // 반영한다 — slam_done(새 SLAM 맵 저장)과 P5 "파일로 등록" 둘 다 이걸 거친다.
    void refreshMapForRobot(const QString& robotCode);

protected:
    // 웨이포인트 목록의 각 행(QFrame, "wpId" 프로퍼티로 표시)을 클릭하면 편집 팝오버를
    // 연다. p6_patrol_route_wireframe.html의 목록 행에는 별도 액션 아이콘이 없어서,
    // 대신 행 전체를 눌러 편집으로 들어가는 방식으로 그 상호작용을 유지한다.
    bool eventFilter(QObject* watched, QEvent* event) override;
    // 이 페이지가 QStackedWidget에서 현재 화면으로 뜰 때마다 호출된다(MainWindow가
    // 페이지를 전환할 때, 즉 사용자가 P6 탭으로 넘어올 때). "순찰 시작"/"이동"을
    // 누르기 전에도 로봇 현재 위치를 먼저 보고 싶다는 요청 — 여기서 Nav2를 미리
    // 준비시킨다(순찰 명령은 안 보냄).
    void showEvent(QShowEvent* event) override;
    // P5MapPage와 같은 이유·같은 패턴 — 방향키(↑↓←→)/Space로 teleopPanel_을
    // 조작한다. 포커스를 받으려면 생성자에서 setFocusPolicy(Qt::StrongFocus)가
    // 필요하다.
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    struct Schedule {
        QString routeName;
        QString timeText;
        QVector<int> days;  // 0=월 .. 6=일
        bool enabled;
        int serverId = -1;  // POST /patrol-schedules로 만든 서버 행 id. 로컬 전용이면 -1
        // 이 스케줄이 "복약 스케줄" 체크박스로 매칭된 medication_schedule.id.
        // 0=복약 스케줄이 아니거나 매칭 안 함(경로 이름과는 무관하다).
        int medicationScheduleId = 0;
    };

    // 맵마다 웨이포인트·경로·스케줄이 독립적이다. 현재 맵의 상태는 waypoints_/routes_/...
    // 멤버에 "펼쳐진" 채로 들고 있다가, 맵을 전환할 때 이 구조체로 저장·복원한다.
    struct MapData {
        QString mapName;
        QString robotId;
        QString recipientName;
        QVector<WaypointCanvas::Waypoint> waypoints;
        QMap<QString, QVector<QString>> routes;
        QString currentRoute;
        int nextWpSeq = 1;
        QVector<Schedule> schedules;
        QImage backgroundImage;  // P5에서 파일로 등록했거나 서버에서 내려받은 실제 pgm 맵 (없으면 null)
        QString imageUrl;  // 서버 맵의 image_url("/media/files/{id}"). loadServerMapDetails가 이걸로
                            // backgroundImage를 채운 뒤엔 더 안 쓴다 — 로컬 데모 맵이면 빈 문자열
        int serverMapId = -1;    // GET /maps/active로 받아온 맵이면 서버 map.id, 로컬 데모 맵이면 -1
        int serverRobotId = -1;  // 서버 robot.id (POST /patrol-runs 등에 필요). 로컬 데모 맵이면 -1
        // 캔버스 픽셀<->실제 미터 환산 기준(서버 map.resolution/origin_x/origin_y).
        // resolution<=0이면 서버에서 아직 못 받음(로컬 데모 맵 등) — WaypointCanvas/
        // designMapMetersToCanvas가 이 경우 고정 배율로 폴백한다.
        double resolution = 0.0;
        QPointF originMeters;
        // 마지막으로 서버와 동기화됐을 때의 코드/이름 -> 서버 PK. "저장·배포"가 델타 계산에 쓴다.
        QMap<QString, int> waypointServerIds;  // waypoint.code -> waypoint.id
        QMap<QString, int> routeServerIds;     // route 이름 -> patrol_route.id
    };

    QWidget* buildHeaderRow();
    // WsClient가 끊기면(체크리스트 ⑩) 보여줄 배너 — P5MapPage::buildConnectionBanner()와
    // 같은 패턴(QFrame#banner, level="attention"). 재연결 시도 중임을 알린다.
    QFrame* buildConnectionBanner();
    // 로봇(downlink_node)이 서버와 끊겼을 때 보여줄 배너 — connectionBanner_와 달리
    // "PC는 서버에 붙어있는데 로봇만 명령을 못 받는" 상황을 알린다.
    QFrame* buildRobotOfflineBanner();
    QWidget* buildCanvasCard();
    QWidget* buildRightColumn();
    QWidget* buildWpListCard();
    QWidget* buildRouteCard();
    QWidget* buildScheduleCard();

    void rebuildWpList();
    // routeSelectCombo_의 항목을 routes_ 키 목록으로 다시 채운다 — 예전엔 체크 가능한
    // "칩" 버튼 행이었다(콤보박스로 통일, 2026-09-10).
    void rebuildRouteSelector();
    void rebuildRouteOrderList();
    // rebuildRouteOrderList()가 "편집 상태 초기화 + 목록 다시 그리기"를 한 번에 하던
    // 것에서, 목록을 다시 그리는 부분만 떼어낸 것 — 추가/삭제(addWaypointToCurrentRouteOrder/
    // removeWaypointFromCurrentRouteOrder)는 편집 상태를 유지한 채로 목록만 다시
    // 그려야 하므로(resetRouteOrderEditUi를 부르면 편집 모드가 꺼져버린다) 이 함수를
    // 직접 쓴다. routeOrderEditing_이 true면 각 행에 삭제 버튼을 붙인다.
    void populateRouteOrderListItems();
    void rebuildRouteEditList();
    void rebuildScheduleList();
    void refreshCanvasPreview();
    // "편집" 클릭 — 지금 routes_[currentRoute_]를 routeOrderEditSnapshot_에 스냅샷하고
    // (취소용), 드래그 재정렬을 허용하고, 추가용 콤보(routeOrderAddCombo_)를 채우고,
    // 버튼을 편집(저장/취소 표시)으로 전환한다.
    void enterRouteOrderEdit();
    // "저장" 클릭 — 지금 routes_[currentRoute_](드래그 재정렬·추가·삭제가 이미 다
    // 반영된 상태) 순서를 PUT /patrol-routes/{id}로 서버에 반영하고 편집 UI를 끈다.
    void saveRouteOrderEdit();
    // "취소" 클릭 — routes_[currentRoute_]를 routeOrderEditSnapshot_으로 되돌리고
    // (드래그·추가·삭제를 전부 무시) 서버 호출 없이 편집 UI를 끈다.
    void cancelRouteOrderEdit();
    // 편집 중 "+ 추가" — routeOrderAddCombo_에서 고른 웨이포인트를 routes_[currentRoute_]
    // 끝에 추가한다. charge kind는애초에 콤보에 없다(refreshRouteOrderAddCombo 참고).
    void addWaypointToCurrentRouteOrder();
    // 편집 중 각 행의 삭제(×) 버튼 — routes_[currentRoute_]에서 이 id를 제거한다.
    void removeWaypointFromCurrentRouteOrder(const QString& id);
    // routeOrderAddCombo_를 "현재 맵의 patrol/emergency 웨이포인트 중 이 경로에 아직
    // 없는 것"으로 채운다 — 편집 진입 시, 그리고 추가/삭제로 그 집합이 바뀔 때마다 부른다.
    void refreshRouteOrderAddCombo();
    // 버튼·드래그 잠금 상태만 "편집 꺼짐"으로 되돌린다(서버 반영 여부는 호출자가 이미
    // 결정한 뒤다) — 경로를 바꾸거나(routeSelectCombo_) 목록을 다시 그릴 때
    // (rebuildRouteOrderList) 쓴다.
    void resetRouteOrderEditUi();

    QString nextWaypointId() const;
    QString chargingWaypointId() const;

    // loadServerMapImageDone이 아직 canvas_에 올라가지 않은(currentMapIndex_가 아닌)
    // 맵의 서버 웨이포인트(미터)를 캔버스 좌표로 미리 배치할 때 쓴다 — canvas_는
    // "현재" 맵 하나만 표시하므로 그 live 상태(canvas_->mapMetersToCanvas)를 쓸 수
    // 없어, showEvent가 쓰는 것과 같은 설계 기준 캔버스(380x330)로 직접 계산한다.
    QPointF designMapMetersToCanvas(const MapData& md, QPointF meters) const;

    void handleCanvasAddRequested(QPointF pos, qreal headingDeg);
    void handleCanvasEditRequested(const QString& id);
    // popover_를 anchor 근처(오른쪽 아래)에 띄우되, 창 오른쪽·아래 경계를 넘지 않게
    // clamp한다 — handleCanvasAddRequested/handleCanvasEditRequested가 공유한다.
    void movePopoverNear(const QPoint& anchor);
    void handleCanvasDeleteRequested(const QString& id);
    void handleCanvasMoved(const QString& id, QPointF pos, qreal headingDeg);
    void handlePopoverSave(const QString& id, const QString& name, const QString& kind, qreal yaw, QPointF pos);

    void enterRouteEditMode(const QString& routeName);
    void exitRouteEditMode(bool save);
    void handleRouteWaypointClicked(const QString& id);
    void startNewRoute();
    void deleteCurrentRoute();
    void runTestRun();
    void driveToSingleWaypoint(const QString& id);
    // 팝오버의 "주행 취소"(토글된 "경로 이동") — POST /patrol-runs/cancel로 지금 이
    // 로봇이 하고 있는 주행(순찰/이동 공통)을 멈춘다. id는 신호 시그니처를 맞추기 위한
    // 것일 뿐 실제로는 drivingWaypointId_ 기준으로 동작한다(cancelCurrentDrive 정의부 참고).
    void cancelCurrentDrive(const QString& id);
    // "위치 보정" — 캔버스에서 클릭+드래그로 잡은 위치·방향을 서버에 보내 AMCL 초기
    // 위치를 즉석에서 다시 잡는다(자동 세팅이 충전 스테이션 좌표를 가정하는 것과 달리,
    // 로봇이 실제로 있는 임의의 위치를 사용자가 직접 지정할 때 쓴다).
    void handleInitialPoseCaptured(QPointF canvasPos, qreal headingDeg);
    void toggleScheduleForm(bool show);
    void saveScheduleFromForm();
    // scheduleListLayout_의 편집 아이콘 클릭 — schedules_[index]로 스케줄 폼을 채우고
    // 연다. 저장 시 saveScheduleFromForm()이 scheduleEditIndex_로 추가/수정을 구분한다.
    // 경로(route_id)는 PUT /patrol-schedules가 지원하지 않아 편집 중엔 콤보를 잠근다.
    void startEditSchedule(int index);
    // "주행 스케줄" 폼의 "복약 스케줄" 체크박스 — 추가 폼에서 사용자가 직접 클릭하거나,
    // startEditSchedule이 프로그램적으로 세팅할 때(수정, schedules_[i].medicationScheduleId>0
    // 이면 체크된 것으로 간주) 뜬다. 켜면 복약 스케줄 콤보(scheduleMedCombo_)를 보이고
    // 채운다. 수정 모드일 땐 지금 편집 중인 schedules_[scheduleEditIndex_].medicationScheduleId를
    // loadMedicationSchedulesForCurrentRobot에 넘겨 그 id 그대로 자동 선택시킨다
    // (patrol_schedule.medication_schedule_id — 값 비교가 아니라 id 매칭이라 이름·시각이
    // 같은 다른 스케줄과 혼동될 일이 없다). 예전엔 경로 이름이 "medicine"인지로 복약
    // 여부를 판별했는데(routeMedicineCheckbox_), 그러면 경로 하나당 복약 여부가
    // 고정돼버려 "이 경로의 이 시각 스케줄만 복약용" 같은 조합이 안 됐다 — 요청 사양대로
    // 스케줄(patrol_schedule) 하나하나의 속성으로 옮겼다.
    void onScheduleMedicineToggled(bool checked);
    // 지금 맵(currentMapIndex_)의 로봇에 배정된 대상자를 찾아 그 복약 스케줄로
    // scheduleMedCombo_를 채운다. GET /recipients로 로봇코드 매칭 -> GET /recipients/{id}로
    // schedules 조회, 둘 다 이미 있는 API를 재사용한다(서버 변경 없음). 콤보 0번은
    // 항상 "선택 안 함"이고 실제 항목은 1번부터(currentRobotMedSchedules_[i]는 콤보의
    // i+1번). matchMedicationScheduleId>0이면(수정 모드) 그 id를 가진 항목을 찾아 자동
    // 선택한다 — 없으면(예: 그 사이 삭제됨) "선택 안 함"에 남는다.
    void loadMedicationSchedulesForCurrentRobot(int matchMedicationScheduleId = 0);
    // scheduleMedCombo_에서 항목을 고르면 그 복약 스케줄의 시각·요일로 scheduleTimeEdit_/
    // scheduleDayChips_를 자동으로 채운다. comboIndex 0("선택 안 함")이면 아무 것도 안 한다.
    void applyMedicationScheduleSelection(int comboIndex);
    // 확인 다이얼로그 후 schedules_[index]를 제거하고, 서버에 등록된 행이면(serverId>0)
    // DELETE /patrol-schedules/{id}도 함께 보낸다(소프트 삭제).
    void deleteScheduleAt(int index);

    void saveCurrentMapData();
    void loadMapData(int index);
    void switchToMap(int index);
    void rebuildMapMenu();

    // 서버에 등록된 활성 맵 목록(GET /maps/active)으로 maps_를 교체한다. 서버가 비어
    // 있거나 응답하지 않으면 maps_는 빈 채로 남는다(더미 데이터로 대체하지 않는다).
    void loadMapsFromServer();
    // maps 배열의 index번째 맵부터 순서대로 배경 이미지(.pgm)·웨이포인트·경로·스케줄을
    // 채운다. 다 채우면 maps_ = *maps로 교체하고 화면을 다시 그린다.
    void loadServerMapDetails(std::shared_ptr<QVector<MapData>> maps, int index);
    // loadServerMapDetails가 배경 이미지까지 받은 뒤(또는 image_url이 없어 건너뛴 뒤)
    // 웨이포인트·경로·스케줄 조회로 이어간다.
    void loadServerMapImageDone(std::shared_ptr<QVector<MapData>> maps, int index);
    // loadServerMapDetails가 경로 목록까지 받은 뒤, 경로별 스케줄(GET /patrol-schedules)을
    // 순서대로 채운다. 다 채우면 다음 맵(index+1)으로 넘어간다.
    void loadServerRouteSchedules(std::shared_ptr<QVector<MapData>> maps, int mapIndex,
                                   QVector<ApiClient::PatrolRouteInfo> routes, int routeIndex);
    // 지금 보고 있는 맵(currentMapIndex_)의 로봇에 nav_prepare를 보낸다 —
    // showEvent(페이지 전환)와 loadMapData(맵 전환) 둘 다에서 부른다. serverMapId가
    // 아직 없는(서버 미동기화) 맵이면 조용히 아무 것도 안 한다.
    void prepareNavigationForCurrentMap();

    // /ws/dashboard로 들어온 메시지 처리 — robot_position이 현재 맵의 로봇이면 캔버스의
    // "현재 위치" 마커를 실시간으로 갱신한다(체크리스트 ⑤). slam_done이면 해당 로봇의
    // 맵을 새로 조회해 드롭다운(맵 메뉴)에 반영한다. patrol_schedules_changed면
    // refreshSchedulesForRoute로 그 경로의 스케줄을 다시 받아온다(대상자 페이지에서
    // 매칭된 복약 스케줄이 바뀐 경우 — P3에서 저장하면 서버가 이 메시지를 보낸다).
    void handleWsMessage(const QJsonObject& message);
    // GET /patrol-schedules?route_id=로 이 경로의 스케줄을 다시 받아 해당 맵(지금 보고
    // 있는 맵이면 schedules_, 아니면 그 MapData::schedules)에 있던 이 경로의 기존 항목을
    // 통째로 교체한다. 대시보드를 껐다 켜지 않아도 P3에서 바뀐 복약 스케줄 시각·요일이
    // 반영되게 하는 게 목적 — 로컬 편집 중(scheduleEditIndex_>=0)인 그 폼 자체는 안 건드린다.
    void refreshSchedulesForRoute(int routeId);
    // 다른 대시보드가 이 맵의 웨이포인트/경로를 저장했을 수 있다(waypoints_changed/
    // patrol_routes_changed WS) — 자동으로 캔버스를 갈아치우면 편집 중인 상태를 덮어쓸
    // 위험이 있어(캔버스가 아직 안 뜬 시점의 좌표 변환 문제도 있고), 대신 새로고침
    // 버튼 옆에 배지만 띄워두고 실제 재조회는 사용자가 그 버튼을 눌렀을 때만 한다.
    // GET /waypoints + GET /patrol-routes로 지금 맵(currentMapIndex_)만 다시 받아
    // waypoints_/routes_에 반영한다 — loadServerMapImageDone의 초기 로딩 경로와 같은
    // 변환을 쓰되, 이미 떠 있는 캔버스라 designMapMetersToCanvas 대신 canvas_->
    // mapMetersToCanvas()(live)를 쓴다.
    void reloadCurrentMapWaypointsAndRoutes();
    // refreshRobotStateBtn_ 옆 "웨이포인트/경로 변경됨" 배지를 staleMapIds_ 기준으로
    // 보이거나 숨긴다 — 맵 전환(loadMapData)·WS 수신·재조회 완료 시점마다 부른다.
    void updateRefreshBadge();

    // robotOfflineBanner_의 아이콘·문구·색을 (robotOnline_, isPatrolling_) 기준으로
    // 다시 그린다 — 오프라인(빨강) / 주행 중(청록, 온라인이지만 새 명령 불가) /
    // 온라인+대기(숨김) 세 상태. robot_status·patrol_progress·patrol_done 수신,
    // 맵 전환(loadMapData) 등 두 값이 바뀌는 모든 지점에서 부른다.
    void updateRobotStatusBanner();
    // robotOnline_/isPatrolling_/slamActive_/navRunning_ 네 값을 한 곳에서만 조합한다 —
    // updateDrivingButtonsEnabled()와 refreshPopoverDriveButton()이 예전엔 이 조건을
    // 각자 따로 들고 있어서, 조건을 하나 더 추가하는 변경이 한쪽에만 반영되면 툴바
    // 버튼과 팝오버 버튼이 서로 다른 판단을 내릴 수 있었다.
    bool canDrive() const;
    // testBtn_("경로 주행")·팝오버의 "경로 이동" 버튼처럼 로봇에 새 주행 명령을 보내는
    // 버튼들의 활성화 여부를 canDrive() 기준으로 맞춘다. 팝오버는 열려 있는 동안
    // 상태가 바뀌어도 실시간으로 안 따라가고, 다음에 열 때(handleCanvasEditRequested)
    // 그 시점 값으로 다시 맞춘다.
    void updateDrivingButtonsEnabled();
    // updateDrivingButtonsEnabled()가 매번 같이 부른다 — 팝오버가 떠 있고 지금
    // 편집 중인 웨이포인트가 drivingWaypointId_와 같으면 "주행 취소"로, 아니면
    // "경로 이동"(활성화 여부는 canDrive() 기준)으로 토글을 맞춘다.
    void refreshPopoverDriveButton();
    // "지금 이 로봇이 실제로 어떤 상태인지"를 서버에 다시 물어 robotOnline_(GET
    // /robots의 is_online)·isPatrolling_(GET /patrol-runs?status=running 결과 유무)을
    // 맞추고 배너·버튼을 갱신한다. patrol_progress/patrol_done WS만 기다리면 페이지를
    // 새로 열었을 때나(맵 전환 포함) WS 메시지를 놓쳤을 때 실제 상태를 알 길이 없어서
    // 생겼다 — loadMapData()와 헤더의 새로고침 버튼(refreshRobotStateBtn_) 둘 다 이걸
    // 그대로 쓴다. 두 조회는 독립적으로 비동기라 응답이 각자 도착하는 대로 반영된다.
    void refreshRobotState();

    QPushButton* mapChipBtn_;
    QMenu* mapMenu_;
    // 헤더에 항상 보이는 새로고침 — 클릭하면 refreshRobotState()를 그대로 호출한다.
    // 오프라인/주행 중 표시가 실제 상태와 어긋난 채로 안 바뀌는 경우(WS 메시지를
    // 놓쳤거나 patrol_done을 영영 못 받은 경우 등) 사용자가 직접 다시 맞출 수 있게 한다.
    QPushButton* refreshRobotStateBtn_;
    // "웨이포인트/경로 변경됨" — refreshRobotStateBtn_ 옆에만 보이는 상태 배지
    // (updateRefreshBadge()가 staleMapIds_ 기준으로 show/hide한다).
    QLabel* refreshStaleBadge_;
    QVector<MapData> maps_;
    int currentMapIndex_;
    // waypoints_changed/patrol_routes_changed WS로 "저장된 웨이포인트/경로가 이 맵
    // (map_id)에서 바뀌었다"고 들은 서버 map id들 — 새로고침 버튼을 눌러야만 비워진다
    // (자동 반영은 안 함, reloadCurrentMapWaypointsAndRoutes 주석 참고).
    QSet<int> staleMapIds_;

    QWidget* wpListCard_;
    // "웨이포인트 N" — rebuildWpList()가 개수만 바꿔 다시 그린다(경로 카드의 정적인
    // "경로 · Trajectory" 타이틀과 달리 이 라벨은 개수가 바뀔 때마다 갱신돼야 한다).
    QLabel* wpListTitleLabel_;
    // "+ 추가" — 경로 카드의 "+ 새 경로"와 같은 자리·스타일. 체크 상태가
    // canvas_->addWaypointMode()와 그대로 연결되므로, rebuildWpList()가 매번 다시
    // 만들면(다른 위젯들처럼) 체크 상태가 사라져 캔버스 쪽과 어긋난다 — 그래서 이
    // 버튼만은 buildWpListCard()에서 한 번만 만들고 절대 다시 만들지 않는다.
    QPushButton* addWpBtn_;
    QVBoxLayout* wpListLayout_;
    // "유형: 순찰 지점 · 충전" — 경로 카드의 routeStatsLabel_처럼 목록 밑에 둔다
    // (예전엔 헤더 행에 있었다).
    QLabel* wpTypeHintLabel_;

    QStackedLayout* routeStack_;
    // 저장된 경로 선택 — 예전엔 체크 가능한 "칩" 버튼 행(routeChipsLayout_)이었다.
    // 대상자 페이지 모달·P5 로봇 선택과 같은 CareGuardComboBox로 통일했다.
    CareGuardComboBox* routeSelectCombo_;
    // "편집" — 클릭하면 enterRouteOrderEdit()로 진입, 편집 중엔 숨고 대신 아래
    // routeOrderSaveBtn_/routeOrderCancelBtn_ 쌍이 보인다.
    QPushButton* routeOrderEditToggleBtn_;
    QPushButton* routeOrderSaveBtn_;
    QPushButton* routeOrderCancelBtn_;
    QListWidget* routeOrderList_;
    // 편집 중에만 보이는 "+ 추가" 행(콤보 + 버튼) — refreshRouteOrderAddCombo()가 채운다.
    QWidget* routeOrderAddRow_;
    QComboBox* routeOrderAddCombo_;
    QPushButton* routeOrderAddBtn_;
    // routeOrderList_가 지금 편집 가능(드래그+추가+삭제) 상태인지 — populateRouteOrderListItems()가
    // 이 값을 보고 각 행에 삭제 버튼을 붙일지 정한다.
    bool routeOrderEditing_ = false;
    // enterRouteOrderEdit()가 찍어두는 스냅샷 — cancelRouteOrderEdit()가 드래그·추가·
    // 삭제를 전부 무시하고 여기로 되돌린다.
    QVector<QString> routeOrderEditSnapshot_;
    QLabel* routeStatsLabel_;
    QLineEdit* routeEditNameEdit_;
    QVBoxLayout* routeEditListLayout_;

    QVBoxLayout* scheduleListLayout_;
    QWidget* scheduleForm_;
    QComboBox* scheduleRouteCombo_;
    // "복약 스케줄" 체크박스 — 켜면 scheduleMedLabel_/scheduleMedCombo_가 나타난다
    // (onScheduleMedicineToggled 참고).
    QCheckBox* scheduleMedicineCheckbox_;
    QLabel* scheduleMedLabel_;
    QComboBox* scheduleMedCombo_;
    // scheduleMedCombo_의 각 항목이 어느 복약 스케줄인지 — applyMedicationScheduleSelection이
    // comboIndex로 그대로 인덱싱한다.
    QVector<ApiClient::MedicationScheduleInfo> currentRobotMedSchedules_;
    QTimeEdit* scheduleTimeEdit_;
    QVector<QPushButton*> scheduleDayChips_;
    QPushButton* scheduleSaveBtn_;
    // schedules_에서 지금 폼으로 편집 중인 인덱스. -1이면 "추가" 모드.
    int scheduleEditIndex_ = -1;

    WaypointCanvas* canvas_;
    WaypointPopover* popover_;
    QWidget* canvasToolbarRow_;
    QPushButton* poseCaptureBtn_;
    // P5MapPage와 같은 방향키패드+속도 프리셋 위젯 — 순찰 페이지에서도 원격 조종이
    // 필요하다는 요청으로 추가. velocityChanged를 현재 맵의 로봇 코드로
    // apiClient_->sendTeleop()에 그대로 연결한다.
    TeleopPanel* teleopPanel_;
    QFrame* routeEditBanner_;
    QFrame* connectionBanner_;
    // "로봇 상태 바" — 예전엔 오프라인 여부만 표시했지만, 이제 updateRobotStatusBanner()가
    // 오프라인/주행 중/정상(숨김) 세 상태에 맞춰 아이콘·문구·색(level 프로퍼티)까지
    // 다시 그린다. 이름은 예전 그대로 두되(호출부가 많아 리네임 범위가 커짐) 실질적으로는
    // 범용 로봇 상태 바다.
    QFrame* robotOfflineBanner_;
    QLabel* robotStatusIcon_;
    QLabel* robotStatusText_;
    // 서버로부터 이 로봇의 robot_status를 아직 못 받았으면 안전하게 오프라인으로
    // 간주한다 — 그래서 기본값이 false다.
    bool robotOnline_ = false;
    // patrol_progress/patrol_done(WS)과 startPatrolRun/driveToPoint의 HTTP 응답으로
    // 갱신한다 — true인 동안은 이 로봇에게 새 주행 명령(순찰 시작/경로 이동)을 보내면
    // patrol_task.py의 액션 서버가 REJECT하므로, 그 전에 버튼을 잠가 사용자에게
    // "왜 안 되는지" 미리 알린다. DB(patrol_run.status)에는 이미 기록되고 있었지만
    // 대시보드가 그 진행 상태 메시지를 아예 수신하지 않아서(patrol_progress/patrol_done
    // 둘 다 handleWsMessage가 무시했음) 화면엔 전혀 안 드러났었다.
    bool isPatrolling_ = false;
    // isPatrolling_이 true인 동안 그 주행이 "manual"(대시보드 버튼)인지 "schedule"
    // (services/scheduler.py 자동 트리거)인지 — updateRobotStatusBanner()가 "로봇
    // 주행 중"/"스케줄 주행 중" 문구를 가르는 데만 쓴다(차단 자체는 트리거 종류와
    // 무관하게 이미 동일하게 적용된다). isPatrolling_이 false면 빈 문자열.
    QString activePatrolTrigger_;
    // GET /slam-jobs?robot_id=&status=queued(refreshRobotState) + slam_progress/
    // slam_done(WS)으로 갱신한다 — 이 로봇이 P5에서 SLAM 매핑 중이면 true. SLAM 중엔
    // downlink_node._ensure_navigation_running이 Nav2를 안 띄우고 조용히 거부하므로
    // (로그만 남기고 PC엔 전혀 안 알림), 그 실패를 사전에 막기 위해 이동·순찰 버튼을
    // 여기서 먼저 잠근다(서버도 별도로 409 거부한다 — 이건 UI 안내용).
    bool slamActive_ = false;
    // nav_status(WS, downlink_node._send_nav_status) + refreshRobotState()의
    // GET /robots(robot_ws.py::nav_running 캐시)로 갱신한다 — 예전엔 WS뿐이라 그
    // 메시지 하나만 유실/지연돼도 "새로고침"이 전혀 안 먹혔다(2026-09-10에 재동기화
    // 경로 추가). 기본값 false — "로봇 온라인=Nav2도 당연히 떠있다"고 가정하면 안
    // 된다(막 온라인 됐거나, Nav2가 맵 다운로드/AMCL 기동 중이거나, 이전에 기동
    // 실패한 채로 남아있을 수 있음). slamActive_가 아닌데 이것도 false면 "SLAM
    // 중도 아니고 Nav2도 안 뜬" 제3의 상태로 배너에 안내한다.
    bool navRunning_ = false;
    // 지금 수동으로("경로 이동" 팝오버 버튼) 주행 중인 웨이포인트 id — 빈 문자열이면
    // 없음. rebuildWpList()가 이 id의 행을 하이라이트하고, refreshPopoverDriveButton()이
    // 팝오버가 지금 보고 있는 웨이포인트와 비교해 "경로 이동"/"주행 취소" 토글을
    // 정한다. runTestRun()(경로 전체 주행)은 특정 한 지점이 아니라 이 값을 안 건드린다 —
    // isPatrolling_/activePatrolTrigger_만으로 그쪽 배너·버튼 잠금은 충분하다.
    QString drivingWaypointId_;
    // "순찰 시작" — updateDrivingButtonsEnabled()가 활성화 여부를 갱신한다.
    QPushButton* testBtn_;

    QVector<WaypointCanvas::Waypoint> waypoints_;
    QMap<QString, QVector<QString>> routes_;
    QString currentRoute_;
    int nextWpSeq_;

    bool routeEditMode_;
    QString routeEditTargetName_;
    QVector<QString> routeEditSelection_;

    QVector<Schedule> schedules_;

    ApiClient* apiClient_;
    WsClient* wsClient_;
};
