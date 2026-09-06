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

signals:
    void emergencyStopRequested();
    // "매핑 재시작"/"매핑 취소" 클릭 — 화면(view_) 초기화는 이 카드가 바로 처리하지만,
    // 로봇에게 실제로 SLAM 프로세스를 멈추고(취소)/다시 시작하라고(재시작) 알리는 건
    // 서버 API 호출이 필요해 P5MapPage가 담당한다.
    void mappingRestartRequested();
    void mappingCancelRequested();

public slots:
    void setVelocity(qreal linearMps, qreal angularRadps);
    // 파일로 등록된 맵을 반영: 배지·라벨을 "맵 등록됨"으로 바꾸고 배경 이미지를 표시한다.
    void applyRegisteredMap(const QImage& image, const QString& mapName, const QString& robotId);

private:
    void tickElapsed();

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
