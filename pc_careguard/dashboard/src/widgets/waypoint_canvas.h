#pragma once
#include <QFrame>
#include <QVector>
#include <QPointF>
#include <QString>
#include <QImage>

class QMouseEvent;
class QWheelEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QContextMenuEvent;
class CameraLiveView;

// P6 웨이포인트 편집 캔버스 — p6_patrol_route_wireframe.html의 <svg> 영역.
// 맵 클릭으로 웨이포인트 추가, 드래그로 이동, 우클릭으로 삭제. 경로 편집 모드에서는
// 클릭 순서대로 웨이포인트를 골라 경로를 구성한다 (충전 스테이션은 클릭 불가).
// 마우스 휠로 확대/축소, 빈 공간 드래그로 패닝, 마커 밖 더블클릭으로 보기 초기화.
class WaypointCanvas : public QFrame {
    Q_OBJECT
public:
    enum class InteractionMode { AddWaypoint, MoveWaypoint };

    struct Waypoint {
        QString id;
        QString name;
        bool charging = false;
        QPointF pos;     // 캔버스 논리 좌표
        qreal yawDeg = 0.0;
        // 서버에서 받은 실제 map 프레임 미터 좌표(map.resolution/origin_x/origin_y
        // 기준) — P6PatrolPage::loadServerMapImageDone만 채운다. showEvent()가 이
        // 위젯이 처음으로 유효한 실제 크기를 갖게 되는 시점에 pos를 이 값으로부터
        // 다시 정확히 계산하는 데 쓴다. 그 전까지(맵을 아직 한 번도 안 보여준 상태로
        // 서버 데이터가 비동기로 먼저 도착하면) pos는 그 시점의 위젯 크기가 아직
        // 확정되지 않아 잘못 계산됐을 수 있다 — 실제로 겪은 버그(2026-09-08): 웨이포인트가
        // 화면 왼쪽 위 구석에 뭉쳐 보임. 로컬에서 직접 추가/편집한 웨이포인트는 이미
        // 화면이 떠 있는 상태에서만 만들어지므로(그 위젯의 showEvent가 이미 지난 뒤라
        // 이 재계산 경로를 다시 타지 않는다) 기본값(0,0)으로 둬도 문제없다.
        QPointF metersPos;
    };

    explicit WaypointCanvas(QWidget* parent = nullptr);

    void setWaypoints(const QVector<Waypoint>& wps);
    QVector<Waypoint> waypoints() const { return waypoints_; }
    const Waypoint* findWaypoint(const QString& id) const;

    // 현재 선택된 경로(순서대로)를 점선 미리보기로 그린다.
    void setRoutePreview(const QVector<QString>& orderedIds);

    void setRouteEditMode(bool on);
    // 경로 편집 모드에서 지금까지 클릭으로 고른 순서.
    void setRouteEditSelection(const QVector<QString>& orderedIds);

    // "WP 추가"/"이동" 툴바 토글 — 배타적으로 하나만 적용된다.
    void setInteractionMode(InteractionMode mode);
    InteractionMode interactionMode() const { return mode_; }

    // "위치 보정" 모드 — AMCL 초기 위치를 지도 위 클릭+드래그(위치+방향)로 직접
    // 잡아 보정한다. WP 추가/이동 모드와 배타적으로 이 모드가 켜져 있는 동안은
    // 우선 처리된다(routeEditMode_보다도 먼저 검사). 드래그 한 번 끝나면(release)
    // 자동으로 꺼진다(단발성) — 호출자가 다시 setPoseCaptureMode(true)로 재진입해야 한다.
    void setPoseCaptureMode(bool on);
    bool poseCaptureMode() const { return poseCaptureMode_; }

    // 실제 map 프레임 미터<->캔버스 좌표 환산 기준(서버 map.resolution/origin_x/origin_y).
    // 맵을 전환할 때마다(P6PatrolPage::loadMapData) 그 맵의 값으로 갱신해야 한다.
    // resolutionMPerPx<=0이면 "아직 모른다"는 뜻 — canvasToMapMeters/mapMetersToCanvas가
    // 예전의 고정 배율(kLegacyPxPerMeter) 폴백으로 돌아간다(서버 미등록 로컬 데모 맵 등).
    void setMapMeta(qreal resolutionMPerPx, QPointF originMeters);

    // 다음 paintEvent에서 모든 웨이포인트의 pos를 metersPos로부터 다시 계산하게
    // 예약한다. P6PatrolPage::loadMapData()가 이 맵으로 전환할 때마다 부른다 —
    // showEvent 시점에 위젯이 "언젠가 한 번" 유효한 크기를 갖는지에 기대는 대신,
    // 실제로 그리는(=반드시 유효한 크기를 가진) 시점에 맞춰 다시 계산하는 게
    // 유일하게 항상 안전하다(실제로 겪은 버그: 페이지를 아직 한 번도 안 보여준
    // 상태에서 서버 웨이포인트가 도착 -> 그 시점 위젯 크기가 아직 확정 전이라 잘못
    // 계산됨 -> 그런데 이미 그 전에 이 위젯이 "한 번은" 보여진 적이 있어서(hasFitInitialView_
    // 가 이미 true) showEvent의 1회성 보정 훅이 다시 실행되지 않아 안 고쳐짐).
    // 드래그로 옮기는 중인 마커는 이 재계산 대상이 아니다 — 그 마커는 pos만 직접
    // 바꾸고 metersPos는 그대로라, 다음 paint에서 옛 자리로 되돌아가 버리기
    // 때문에 이 함수는 "새 맵을 불러왔을 때"만 불러야 한다.
    void requestMetersResync() { pendingMetersResync_ = true; }
    // 캔버스 좌표 -> 실제 map 프레임 미터. "이동"/"위치 보정" 등 로봇에 좌표를 보내는
    // 모든 곳이 이걸 거쳐야 한다 — 예전엔 각자 고정 배율(kPxPerMeter=40)로 직접
    // 나눠서, 실제 지도 해상도(대개 0.05, 즉 20px/m)와 달라 클릭한 지점과 로봇이
    // 실제로 가는 곳이 어긋나는 원인이었다.
    QPointF canvasToMapMeters(QPointF canvasPos) const;
    // 실제 map 프레임 미터 -> 캔버스 좌표. 서버에서 받은 웨이포인트/로봇 위치(미터)를
    // 화면에 그릴 때 쓴다.
    QPointF mapMetersToCanvas(QPointF meters) const;

    // canvasToMapMeters/mapMetersToCanvas가 배경 이미지를 배치하는 데 쓰는 것과 같은
    // "그리기 가능 영역". P6PatrolPage가 지금 canvas_에 표시 중이 아닌(맵 전환 전)
    // 다른 맵의 웨이포인트 좌표를 미리 계산해둘 때, 이 위젯의 실제 현재 크기 기준으로
    // 계산해야 나중에 그 맵으로 전환했을 때 자리가 어긋나지 않는다(고정된 와이어프레임
    // 설계 크기(380x330)를 썼다가 실제 창 크기와 안 맞아 전부 왼쪽 위 구석에 뭉쳐
    // 보이던 버그가 있었다 — 2026-09-08 실사용 리포트로 확인).
    QRectF displayBounds() const { return floorBounds(); }

    // 순찰(Nav2) 중 로봇이 실제로 따라가려는 전역 경로(rviz2의 "가려는 길" 선과
    // 같음) — patrol_path_update WS 메시지의 map 프레임 미터 좌표를 그대로 받는다.
    // 좌표 변환은 매번 paintEvent에서 하므로(포인트 수가 많지 않아 비용이 작다)
    // 여기서 캔버스 좌표로 미리 바꿔둘 필요가 없다 — 웨이포인트처럼 "위젯이 아직
    // 유효한 크기를 안 가졌을 때" 문제도 자연히 피해간다. 빈 벡터면 선을 지운다.
    void setPlannedPath(const QVector<QPointF>& pathMeters);

    // 로봇의 실제 실시간 위치(로봇이 보낸 robot_position)를 지도 위에 표시한다.
    void setCurrentRobotPosition(QPointF canvasPos, qreal headingDeg);
    // 로봇이 아직 실시간 위치를 한 번도 안 보냈을 때, 충전 스테이션 위치를 임시로
    // 대신 보여준다(초기 추정값) — setCurrentRobotPosition과 달리, 다음 paintEvent의
    // metersPos 재계산 때 이 위치도 같이 다시 맞춰준다(충전 웨이포인트 자체가 그
    // 시점에 아직 잘못 배치돼 있었을 수 있어서 — WaypointCanvas::showEvent 주석 참고).
    // 실시간 위치가 실제로 도착하면(setCurrentRobotPosition) 이 추적을 그만둔다.
    void setCurrentRobotPositionToChargingStation(QPointF canvasPos);
    // 우상단 카메라 라이브 화면에 실제 프레임을 흘려보낸다(camera_frame WS 메시지).
    void setCameraFrame(const QImage& frame);

    // P5에서 파일로 등록한 실제 pgm 맵 이미지를 배경으로 표시한다 (없으면 목업 평면도).
    void setBackgroundImage(const QImage& image);

    // backgroundImage_가 있을 때, 캔버스 좌표가 점유(벽/장애물) 픽셀 위인지 확인한다.
    // 배경 이미지가 없으면 항상 false (목업 모드에서는 제한 없음).
    bool isOccupiedAt(QPointF canvasPos) const;
    // canvasPos가 점유 지점이면 근처에서 가장 가까운 비점유(자유 공간) 좌표를 찾아 반환한다.
    // 찾지 못하면 원래 좌표를 그대로 반환한다.
    QPointF nearestFreePoint(QPointF canvasPos) const;

    // 마우스 휠 확대/축소 + 드래그 패닝 상태를 기본값(1배, 원점)으로 되돌린다.
    void resetView();

signals:
    // "위치 보정"과 같은 방식 — 빈 곳을 클릭한 채 드래그하면 그 방향이 headingDeg로
    // 실린다(드래그가 너무 짧으면 0도). 예전엔 클릭만으로 위치를 정하고 방향은
    // 팝오버의 다이얼로 따로 설정해야 했다(불편하다는 요청으로 통합).
    void waypointAddRequested(QPointF canvasPos, qreal headingDeg);
    void waypointEditRequested(const QString& id);
    void waypointDeleteRequested(const QString& id);
    // 마커를 드래그해 옮겼을 때 — headingDeg는 드래그 시작점->끝점 방향(위와 같은
    // 규칙). 드래그가 너무 짧으면 기존 yawDeg를 그대로 돌려준다(의미 없는 미세한
    // 떨림으로 방향이 바뀌지 않도록).
    void waypointMoved(const QString& id, QPointF newPos, qreal headingDeg);
    // "이동" 모드에서 드래그 없이 웨이포인트를 클릭하면(= 그냥 클릭) 그 지점으로 주행 요청.
    void waypointDriveRequested(const QString& id);
    void routeWaypointClicked(const QString& id);
    void chargingClickBlocked();
    // 창 크기 변경으로 웨이포인트/로봇 위치를 지도에 맞춰 다시 배치했다 — 호출자가
    // 들고 있는 사본(P6PatrolPage::waypoints_)도 waypoints()로 다시 읽어 동기화해야 한다.
    void viewRescaled();
    // "위치 보정" 모드에서 클릭+드래그(방향 지정)를 마치고 마우스를 뗐다 —
    // canvasPos/headingDeg는 다른 신호들과 같은 캔버스 월드 좌표계·각도 규약을 쓴다
    // (P6PatrolPage가 canvasToMapMeters()로 미터로 바꾸고 qDegreesToRadians()로 API에 보낸다).
    void initialPoseCaptureFinished(QPointF canvasPos, qreal headingDeg);
    // 위치 보정 모드 진입 후 실제로 드래그를 시작하기 전에 우클릭 등으로 취소했다 —
    // 호출자는 툴바 버튼의 체크 상태를 되돌려야 한다.
    void initialPoseCaptureCancelled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    int hitTest(const QPointF& worldPt) const;
    QRectF floorBounds() const;
    // 위젯(화면) 좌표 → 확대/패닝이 적용되지 않은 "월드" 좌표. 웨이포인트 위치·배경
    // 이미지 배치는 전부 월드 좌표계로 다루고, 화면에 그릴 때만 스케일·이동을 적용한다.
    QPointF toWorld(QPointF screenPt) const;

    // 배경 이미지가 bounds 안에 KeepAspectRatio로 그려질 때의 (topLeft, 스케일된 크기)를 구한다.
    bool computeImagePlacement(QPointF* outTopLeft, QSize* outScaledSize) const;

    QVector<Waypoint> waypoints_;
    QVector<QString> routePreview_;
    QVector<QString> routeEditSelection_;
    bool routeEditMode_;
    InteractionMode mode_;

    bool dragging_;
    bool dragMoved_;
    int dragIndex_;
    // 드래그로 마커를 옮길 때 방향(headingDeg)을 "시작점 -> 끝점"으로 계산하려면
    // 시작 위치가 필요하다 — mouseMoveEvent가 waypoints_[dragIndex_].pos를 계속
    // 덮어써서 release 시점엔 이미 원래 위치를 알 수 없어 별도로 들고 있는다.
    QPointF dragStartPos_;

    // "WP 추가" 모드에서 빈 곳을 클릭+드래그해 새 웨이포인트의 위치+방향을 한 번에
    // 잡는 상태 — poseCapturing_과 같은 패턴이다. 이 모드에 있는 동안은 빈 곳
    // 드래그가 더 이상 패닝을 하지 않는다(이동 모드로 바꾸면 패닝은 그대로 된다).
    bool addAiming_;
    QPointF addAimAnchor_;
    QPointF addAimCurrent_;

    qreal zoomScale_;
    QPointF panOffset_;
    bool panning_;
    bool panMoved_;
    QPointF panStartMouse_;
    QPointF panStartOffset_;

    QPointF currentRobotPos_;
    qreal currentRobotHeadingDeg_;
    bool hasRobotPos_;

    bool poseCaptureMode_ = false;
    bool poseCapturing_ = false;
    QPointF poseCaptureAnchor_;
    QPointF poseCaptureCurrent_;

    QVector<QPointF> plannedPathMeters_;
    QImage backgroundImage_;
    qreal mapResolution_ = 0.0;
    QPointF mapOrigin_;
    bool pendingMetersResync_ = false;
    bool robotPosTracksChargingStation_ = false;

    // 이 페이지가 처음 화면에 보일 때(showEvent) 딱 한 번, 웨이포인트를 설계 기준
    // 캔버스(380x330)에서 지금 실제 크기로 리매핑해 화면 전체를 채우도록 확대해 둔다.
    bool hasFitInitialView_ = false;

    CameraLiveView* cameraView_;
};
