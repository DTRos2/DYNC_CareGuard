#pragma once
#include <QFrame>
#include <QImage>

class QLabel;

// P5 SLAM 캔버스·P6 웨이포인트 캔버스 우측 상단에 띄우는 카메라 라이브 화면 자리.
// 자식 위젯이라 부모의 확대/패닝(QPainter translate/scale) 영향을 받지 않고 항상
// 화면 고정 위치에 떠 있다. (원래 이 자리에 그려지던 "미탐색" 회색 박스가 실은 이
// 카메라 화면 자리였다.)
//
// 로봇의 /camera/color/image_raw/compressed(sensor_msgs/CompressedImage, JPEG)를
// uplink_node.py가 그대로(재인코딩 없이) base64로 실어 보내고, 서버는 순수 중계만
// 한다 — slam_map_update와 같은 패턴. setFrame()이 그 디코딩된 프레임을 받는다.
class CameraLiveView : public QFrame {
    Q_OBJECT
public:
    explicit CameraLiveView(QWidget* parent = nullptr);

    // 새 프레임이 도착할 때마다 호출 — placeholder(아이콘+문구)를 감추고 영상으로 교체한다.
    void setFrame(const QImage& frame);
    // WS 연결이 끊기는 등 다시 placeholder로 돌아가야 할 때.
    void clearFrame();

private:
    QLabel* imageLabel_;
    QLabel* placeholderIcon_;
    QLabel* placeholderText_;
};
