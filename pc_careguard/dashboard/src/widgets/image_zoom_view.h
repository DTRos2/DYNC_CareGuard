#pragma once
#include <QGraphicsView>

class QGraphicsScene;
class QGraphicsPixmapItem;
class QWheelEvent;
class QShowEvent;
class QMouseEvent;

// 정적 이미지를 휠로 확대/축소하고 드래그로 이동해서 볼 수 있는 뷰어.
// QGraphicsView 기반이라 확대/패닝을 직접 구현할 필요 없이 프레임워크가 처리한다.
// MapPreviewDialog의 맵 미리보기에 쓰인다 — 그 밖의 이미지 미리보기에도 재사용 가능.
class ImageZoomView : public QGraphicsView {
    Q_OBJECT
public:
    explicit ImageZoomView(QWidget* parent = nullptr);

    // 이미지를 설정하고, 뷰 폭에 맞춰 "전체 보기" 배율로 되돌린다.
    void setImage(const QImage& image);
    // 더블클릭과 같은 동작 — 확대/패닝을 "전체 보기"로 리셋한다.
    void resetZoom();

protected:
    void wheelEvent(QWheelEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void fitToView();

    QGraphicsScene* scene_;
    QGraphicsPixmapItem* pixmapItem_;
    // fitToView()가 마지막으로 계산한 "전체 보기" 배율 — 휠 확대/축소의 상한·하한을
    // 이 값에 대한 상대 배율(1배~8배)로 잰다. 이미지 원본 해상도에 따라 fitInView가
    // 계산하는 절대 배율 자체는 들쭉날쭉해서(큰 pgm은 0.1배, 작은 이미지는 3배 등)
    // 그 절대값을 직접 기준으로 삼을 수 없다.
    qreal fitScale_ = 1.0;
    // 처음 화면에 보일 때(showEvent) 딱 한 번만 "전체 보기"로 맞춘다 — 매 resizeEvent마다
    // 다시 맞추면(이전 구현의 버그) 플랫폼이 보내는 사소한 리사이즈 하나에도 사용자가
    // 방금 확대한 배율이 조용히 원래대로 되돌아가 버린다.
    bool hasFitOnce_ = false;
};
