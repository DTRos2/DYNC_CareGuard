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
class QTimer;
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

    // 로봇의 현재 위치를 지도 위에 표시한다 (초기값: 충전 스테이션).
    void setCurrentRobotPosition(QPointF canvasPos, qreal headingDeg);

    // P5에서 파일로 등록한 실제 pgm 맵 이미지를 배경으로 표시한다 (없으면 목업 평면도).
    void setBackgroundImage(const QImage& image);

    // "테스트 실행" — 충전 스테이션→경로 순서를 따라 현재위치 마커를 움직인다.
    // returnToCharging=false면 목적지에 도착한 뒤 복귀하지 않고 그대로 머문다
    // (단일 웨이포인트로 "이동"할 때 쓴다).
    void startTestRun(const QVector<QString>& orderedIds, const QString& chargingId,
                       bool returnToCharging = true);

    // backgroundImage_가 있을 때, 캔버스 좌표가 점유(벽/장애물) 픽셀 위인지 확인한다.
    // 배경 이미지가 없으면 항상 false (목업 모드에서는 제한 없음).
    bool isOccupiedAt(QPointF canvasPos) const;
    // canvasPos가 점유 지점이면 근처에서 가장 가까운 비점유(자유 공간) 좌표를 찾아 반환한다.
    // 찾지 못하면 원래 좌표를 그대로 반환한다.
    QPointF nearestFreePoint(QPointF canvasPos) const;

    // 마우스 휠 확대/축소 + 드래그 패닝 상태를 기본값(1배, 원점)으로 되돌린다.
    void resetView();

signals:
    void testRunFinished();
    void waypointAddRequested(QPointF canvasPos);
    void waypointEditRequested(const QString& id);
    void waypointDeleteRequested(const QString& id);
    void waypointMoved(const QString& id, QPointF newPos);
    // "이동" 모드에서 드래그 없이 웨이포인트를 클릭하면(= 그냥 클릭) 그 지점으로 주행 요청.
    void waypointDriveRequested(const QString& id);
    void routeWaypointClicked(const QString& id);
    void chargingClickBlocked();
    // 창 크기 변경으로 웨이포인트/로봇 위치를 지도에 맞춰 다시 배치했다 — 호출자가
    // 들고 있는 사본(P6PatrolPage::waypoints_)도 waypoints()로 다시 읽어 동기화해야 한다.
    void viewRescaled();

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

    void updateRobotAlongTestPath();
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

    qreal zoomScale_;
    QPointF panOffset_;
    bool panning_;
    bool panMoved_;
    QPointF panStartMouse_;
    QPointF panStartOffset_;

    QPointF currentRobotPos_;
    qreal currentRobotHeadingDeg_;
    bool hasRobotPos_;

    QImage backgroundImage_;

    QTimer* testRunTimer_;
    QVector<QPointF> testRunPath_;
    qreal testRunT_;

    // 이 페이지가 처음 화면에 보일 때(showEvent) 딱 한 번, 웨이포인트를 설계 기준
    // 캔버스(380x330)에서 지금 실제 크기로 리매핑해 화면 전체를 채우도록 확대해 둔다.
    bool hasFitInitialView_ = false;

    CameraLiveView* cameraView_;
};
