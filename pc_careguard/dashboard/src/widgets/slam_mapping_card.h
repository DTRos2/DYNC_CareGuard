#pragma once
#include <QWidget>
#include <QFrame>
#include <QVector>
#include <QPointF>
#include <QImage>

class QLabel;
class QTimer;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class QMouseEvent;
class CameraLiveView;

// P5 SLAM 실시간 맵 캔버스 — 탐색 영역, 로봇 위치, 궤적을 직접 그린다.
// p5_slam_mapping_wireframe_standalone.html의 <svg> 영역에 대응.
// 좌표는 캔버스 내부 논리 좌표(px)로 다루며, 텔레옵 입력(선속도·각속도)을
// 매 tick 적분해 로봇을 실제로 움직인다 (SLAM 알고리즘 시뮬레이션은 아님).
class SlamMapView : public QFrame {
    Q_OBJECT
public:
    explicit SlamMapView(QWidget* parent = nullptr);

    // linearMps: 전진(+)/후진(-) m/s, angularRadps: 좌회전(+)/우회전(-) rad/s
    void setVelocity(qreal linearMps, qreal angularRadps);
    void resetMapping();

    // 파일로 등록된 실제 맵 이미지를 배경으로 표시한다 (목업 평면도 대신).
    void setBackgroundImage(const QImage& image);

signals:
    void distanceTraveled(qreal deltaMeters);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void tick();
    QRectF exploredBounds() const;

    QTimer* simTimer_;
    QPointF robotPos_;   // 캔버스 논리 좌표
    qreal heading_;      // 라디안, 0 = 오른쪽
    qreal linearMps_;
    qreal angularRadps_;
    QVector<QPointF> trajectory_;
    QImage backgroundImage_;
    CameraLiveView* cameraView_;

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
