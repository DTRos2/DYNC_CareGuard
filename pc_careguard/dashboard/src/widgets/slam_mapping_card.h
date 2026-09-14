#pragma once
#include <QWidget>
#include <QFrame>
#include <QVector>
#include <QPointF>
#include <QSize>
#include <QImage>
#include <optional>

class QLabel;
class QTimer;
class QPainter;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class QMouseEvent;
class CameraLiveView;

// P5 SLAM 실시간 맵 캔버스 — 실제 로봇이 만드는 지도·위치·라이다 스캔을 rviz2 스타일로
// 겹쳐 그린다(격자 + 점유격자 배경 + 빨간 화살표 + 색상 스캔 점).
// p5_slam_mapping_wireframe_standalone.html의 <svg> 영역에 대응.
//
// 예전에는 텔레옵 입력(선속도·각속도)을 매 tick 적분해 캔버스 위에서 로봇을 "가짜로"
// 움직이는 목업 시뮬레이션이었다 — 실제 로봇 위치/스캔 데이터가 없던 시절의 임시
// 구현이었고, 실제 로봇과 무관하게 움직여 화면이 실제 상황과 달라 보이는 원인이었다.
// 지금은 서버가 실시간으로 내려주는 실제 map(m) 좌표(로봇 위치·스캔점)를 그대로 받아
// 그리므로 그 로컬 시뮬레이션은 제거했다.
class SlamMapView : public QFrame {
    Q_OBJECT
public:
    explicit SlamMapView(QWidget* parent = nullptr);

    void resetMapping();

    // 파일로 등록된 실제 맵 이미지를 배경으로 표시한다 (목업 평면도 대신).
    void setBackgroundImage(const QImage& image);
    // 배경 이미지(occupancy grid)의 미터<->픽셀 환산 기준. slam_map_update와 함께 온다
    // (resolution: m/px, origin: 이미지가 나타내는 map 좌표계 원점). 로봇 위치·스캔점을
    // 이미지 위 올바른 자리에 그리려면 반드시 필요하다.
    void setMapMeta(qreal resolutionMPerPx, const QPointF& originMeters, const QSize& imagePixelSize);
    // 실제 로봇 위치(map 프레임, 미터)·방향(라디안). uplink_node의 map->base_link TF 기반.
    void setLivePose(qreal xMeters, qreal yMeters, qreal yawRad);
    // 우상단 카메라 라이브 화면에 실제 프레임을 흘려보낸다(camera_frame WS 메시지).
    void setCameraFrame(const QImage& frame);
    // 로봇 전환 시 호출 — 이전 로봇의 마지막 프레임이 남아있지 않도록 카메라
    // 화면을 "카메라 연결 중" placeholder로 되돌린다.
    void clearCameraFrame();
    // 실시간 라이다 스캔점(map 프레임, 미터 좌표 목록).
    void setLiveScan(const QVector<QPointF>& pointsMeters);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QRectF exploredBounds() const;
    // meters(map 프레임) -> 캔버스 논리 좌표(px). setMapMeta로 받은 기준이 아직 없으면
    // std::nullopt.
    std::optional<QPointF> mapMetersToCanvas(const QPointF& meters) const;
    void drawGrid(QPainter& p, const QRectF& bounds) const;
    void drawLiveTrajectory(QPainter& p) const;
    void drawLiveScan(QPainter& p) const;
    void drawLivePose(QPainter& p) const;

    QImage backgroundImage_;
    CameraLiveView* cameraView_;

    // setMapMeta로 받은 값. mapResolution_<=0이면 아직 맵 메타가 없다는 뜻(= 로봇 위치·
    // 스캔점을 그릴 좌표 기준이 없어 아무것도 안 그린다).
    qreal mapResolution_ = 0.0;
    QPointF mapOriginMeters_{0.0, 0.0};
    QSize mapPixelSize_;

    bool hasLivePose_ = false;
    QPointF livePoseMeters_;
    qreal livePoseYaw_ = 0.0;
    QVector<QPointF> liveTrajectoryMeters_;  // 실제 이동 궤적(점선) — 위치 갱신마다 누적

    QVector<QPointF> liveScanMeters_;

    // WaypointCanvas와 같은 방식의 휠 확대/축소 + 드래그 패닝. 더블클릭으로 초기화.
    qreal zoomScale_ = 1.0;
    QPointF panOffset_{0.0, 0.0};
    bool panning_ = false;
    QPointF panStartMouse_;
    QPointF panStartOffset_;
};

// SLAM 매핑 카드 전체 — 상태 배지 헤더 + SlamMapView + 하단 액션 버튼.
class SlamMappingCard : public QFrame {
    Q_OBJECT
public:
    explicit SlamMappingCard(QWidget* parent = nullptr);

    SlamMapView* view() const { return view_; }

public slots:
    void setVelocity(qreal linearMps, qreal angularRadps);
    // 파일로 등록된 맵을 반영: 배지·라벨을 "맵 등록됨"으로 바꾸고 배경 이미지를 표시한다.
    void applyRegisteredMap(const QImage& image, const QString& mapName, const QString& robotId);
    // P5 로봇 전환 시 호출 — view_를 비우고(resetMapping) 아직 이 로봇에 맵이 없다는
    // 초기 상태로 배지·라벨·경과시간을 되돌린다. 전환 직후 registeredMaps_에 이미지가
    // 있으면 이어서 applyRegisteredMap()이 다시 채운다.
    void resetForRobot(const QString& robotId);
    // "매핑 시작"이 서버에 실제로 접수돼 slam_job이 생긴 시점에만 P5MapPage가
    // 호출한다 — 배지를 "SLAM 매핑 중"으로 바꾼다. 예전엔 이 문구가 생성자
    // 기본값이라 실제로 매핑 중이 아닐 때도(페이지에 막 들어왔을 때 등) 항상
    // 떠 있었다.
    void markMappingStarted();
    // 서버가 릴레이한 실제 slam_progress(로봇의 slam_progress_publisher가 발행한
    // 경과시간·occupancy grid 탐색면적)를 반영한다. elapsedTimer_는 다음 서버 갱신
    // 전까지 화면을 매끄럽게 이어 보여주는 로컬 보간용이고, 값 자체의 기준(진실)은
    // 이 슬롯으로 들어오는 서버 값이다 — 호출될 때마다 로컬 카운터를 덮어써 드리프트를
    // 바로잡는다.
    void applyProgress(int elapsedSec, qreal areaM2);

private:
    void tickElapsed();
    void updateStatsLabel();

    QLabel* badge_;
    QLabel* targetLabel_;
    SlamMapView* view_;
    QLabel* elapsedLabel_;
    QLabel* areaLabel_;
    QTimer* elapsedTimer_;
    int elapsedSeconds_;
    qreal areaM2_;
    bool moving_;
};
