#pragma once
#include <QImage>
#include <QMap>
#include <QVector>
#include <QWidget>
#include <memory>

#include "net/api_client.h"
#include "widgets/waypoint_canvas.h"

class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
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
    explicit P6PatrolPage(QWidget* parent = nullptr);

public slots:
    // P5MapPage::mapRegistered를 MainWindow가 중계해서 부른다. robotId가 이미 있는 맵이면
    // 이름·대상자·배경 이미지를 갱신하고, 없으면 새 맵으로 추가한다.
    void upsertMap(const QImage& mapImage, const QString& mapName, const QString& robotId,
                    const QString& recipientName);

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
    void rebuildRouteChips();
    void rebuildRouteOrderList();
    void rebuildRouteEditList();
    void rebuildScheduleList();
    void refreshCanvasPreview();

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
    void handlePopoverSave(const QString& id, const QString& name, bool charging, qreal yaw, QPointF pos);

    void enterRouteEditMode(const QString& routeName);
    void exitRouteEditMode(bool save);
    void handleRouteWaypointClicked(const QString& id);
    void startNewRoute();
    void deleteCurrentRoute();
    void runTestRun();
    void driveToSingleWaypoint(const QString& id);
    // "위치 보정" — 캔버스에서 클릭+드래그로 잡은 위치·방향을 서버에 보내 AMCL 초기
    // 위치를 즉석에서 다시 잡는다(자동 세팅이 충전 스테이션 좌표를 가정하는 것과 달리,
    // 로봇이 실제로 있는 임의의 위치를 사용자가 직접 지정할 때 쓴다).
    void handleInitialPoseCaptured(QPointF canvasPos, qreal headingDeg);
    // "로봇 종료" 버튼 — 확인 후 apiClient_->shutdownRobot()으로 이 로봇에서 켜진
    // 모든 것(베이스 구동·Nav2/SLAM·rviz2·업링크·다운링크)을 한 번에 끈다.
    void shutdownCurrentRobot();
    // "비상정지" 버튼/Space — P5MapPage::handleEmergencyStop과 같은 이유·패턴.
    void handleEmergencyStop();

    void toggleScheduleForm(bool show);
    void addScheduleFromForm();

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
    // slam_done(다른 화면에서 SLAM 맵 저장이 끝남) 수신 시 이 robotCode의 맵 하나만
    // 새로 조회해 upsertMap()으로 병합한다. loadMapsFromServer()를 통째로 다시 부르면
    // maps_ 전체가 서버 스냅샷으로 교체되면서 사용자가 지금 편집 중인 다른 맵의
    // 웨이포인트·경로가 날아가므로 쓰지 않는다.
    void refreshMapForRobot(const QString& robotCode);
    // 지금 보고 있는 맵(currentMapIndex_)의 로봇에 nav_prepare를 보낸다 —
    // showEvent(페이지 전환)와 loadMapData(맵 전환) 둘 다에서 부른다. serverMapId가
    // 아직 없는(서버 미동기화) 맵이면 조용히 아무 것도 안 한다.
    void prepareNavigationForCurrentMap();

    // "저장 · 로봇에 배포" 버튼 — 현재 맵의 웨이포인트·경로를 서버에 반영한다. 서버에
    // 등록된 맵(serverMapId > 0)에서만 동작한다. 기존에 동기화된 경로·웨이포인트를 모두
    // 지우고 지금 화면 상태로 다시 만드는 방식(delete-then-recreate)이라 서버 쪽 id는
    // 매번 새로 발급된다 — 스케줄은 route_id를 참조해야 해서 이 흐름과 분리해 addScheduleFromForm/
    // 토글 시점에 바로 서버에 반영한다(아래 참고).
    void deployCurrentMap();
    void deployDeleteRoutes(std::shared_ptr<QVector<QString>> routeNames, int index);
    void deployDeleteWaypoints(std::shared_ptr<QVector<QString>> codes, int index);
    void deployCreateWaypoints(std::shared_ptr<QVector<WaypointCanvas::Waypoint>> list, int index,
                                std::shared_ptr<bool> hadError);
    void deployCreateRoutes(std::shared_ptr<QVector<QString>> routeNames, int index,
                             std::shared_ptr<bool> hadError);
    void finishDeploy(bool hadError);

    // /ws/dashboard로 들어온 메시지 처리 — robot_position이 현재 맵의 로봇이면 캔버스의
    // "현재 위치" 마커를 실시간으로 갱신한다(체크리스트 ⑤). slam_done이면 해당 로봇의
    // 맵을 새로 조회해 드롭다운(맵 메뉴)에 반영한다.
    void handleWsMessage(const QJsonObject& message);

    QPushButton* mapChipBtn_;
    QMenu* mapMenu_;
    QVector<MapData> maps_;
    int currentMapIndex_;

    QWidget* wpListCard_;
    QVBoxLayout* wpListLayout_;

    QStackedLayout* routeStack_;
    QHBoxLayout* routeChipsLayout_;
    QListWidget* routeOrderList_;
    QLabel* routeStatsLabel_;
    QLineEdit* routeEditNameEdit_;
    QVBoxLayout* routeEditListLayout_;

    QVBoxLayout* scheduleListLayout_;
    QWidget* scheduleForm_;
    QComboBox* scheduleRouteCombo_;
    QTimeEdit* scheduleTimeEdit_;
    QVector<QPushButton*> scheduleDayChips_;

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
    QFrame* robotOfflineBanner_;
    // 서버로부터 이 로봇의 robot_status를 아직 못 받았으면 안전하게 오프라인으로
    // 간주한다 — 그래서 기본값이 false다.
    bool robotOnline_ = false;

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
