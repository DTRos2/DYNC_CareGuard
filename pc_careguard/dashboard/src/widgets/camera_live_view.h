#pragma once
#include <QFrame>

// P5 SLAM 캔버스·P6 웨이포인트 캔버스 우측 상단에 띄우는 카메라 라이브 화면 자리.
// 실제 RTSP/카메라 파이프라인이 붙기 전까지의 placeholder — 자식 위젯이라 부모의
// 확대/패닝(QPainter translate/scale) 영향을 받지 않고 항상 화면 고정 위치에 떠 있다.
// (원래 이 자리에 그려지던 "미탐색" 회색 박스가 실은 이 카메라 화면 자리였다.)
class CameraLiveView : public QFrame {
    Q_OBJECT
public:
    explicit CameraLiveView(QWidget* parent = nullptr);
};
