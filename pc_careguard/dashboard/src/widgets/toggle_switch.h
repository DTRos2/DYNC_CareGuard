#pragma once
#include <QAbstractButton>

// 순찰 스케줄 on/off 등에 쓰는 토글 스위치. 문서 §4-7 스펙(26x14, RADIUS_FULL,
// ON=ACCENT 채움+흰 노브 우측 / OFF=BORDER_STRONG 채움+노브 좌측)을 직접 페인트한다.
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};
