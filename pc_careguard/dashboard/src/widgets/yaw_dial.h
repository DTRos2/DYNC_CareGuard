#pragma once
#include <QWidget>

// P6 웨이포인트 팝오버의 yaw(도착 방향) 다이얼 — 드래그로 0~359도를 설정한다.
class YawDial : public QWidget {
    Q_OBJECT
public:
    explicit YawDial(QWidget* parent = nullptr);

    int angleDeg() const { return angleDeg_; }
    void setAngleDeg(int deg);

signals:
    void angleChanged(int deg);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void updateFromPos(const QPointF& pos);

    int angleDeg_;
};
