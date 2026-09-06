#pragma once
#include <QFrame>
#include <QSet>

class QLabel;
class QPushButton;
class QSlider;
class QDoubleSpinBox;
class QTimer;

// P5 원격 조작(텔레옵) 패널 — 방향키패드 + 속도 프리셋 + 선속도/각속도
// 슬라이더·스핀박스 양방향 동기화. CareGuard_DESIGN_SYSTEM.md §6(P5 항목) 참고.
// 실제 키보드 입력(↑↓←→ / Space)은 부모(P5MapPage)가 받아 handleKeyPress/Release로 전달한다
// (버튼 클릭에도 눌림 하이라이트 없이 즉시 이동하도록 별도 처리).
class TeleopPanel : public QFrame {
    Q_OBJECT
public:
    explicit TeleopPanel(QWidget* parent = nullptr);

    void handleKeyPress(int key);
    void handleKeyRelease(int key);

signals:
    void velocityChanged(qreal linearMps, qreal angularRadps);

private:
    void buildKeypad(class QVBoxLayout* root);
    void buildPresets(class QVBoxLayout* root);
    void buildSpeedRow(class QVBoxLayout* root, const QString& label, const QString& unit,
                        QDoubleSpinBox*& outSpin, QSlider*& outSlider, double max, double step);
    void setDirectionActive(int key, bool active);
    void recomputeVelocity();
    void applyPreset(qreal linear, qreal angular);
    void syncSpinToSlider(QDoubleSpinBox* spin, QSlider* slider, double max);

    QPushButton* upBtn_;
    QPushButton* downBtn_;
    QPushButton* leftBtn_;
    QPushButton* rightBtn_;
    QLabel* spaceLabel_;
    QLabel* currentInputLabel_;

    QPushButton* presetFine_;
    QPushButton* presetNormal_;
    QPushButton* presetFast_;

    QDoubleSpinBox* linearSpin_;
    QSlider* linearSlider_;
    QDoubleSpinBox* angularSpin_;
    QSlider* angularSlider_;

    QSet<int> pressedKeys_;
    // 로봇(limo_base) 쪽에 "명령이 일정 시간 안 오면 정지"하는 안전장치가 있어서,
    // 방향키를 누르고 있는 동안 velocityChanged를 한 번만 보내면 로봇이 가다가 저절로
    // 멈춘다. 눌려있는 동안 이 타이머로 계속 재전송해 멈추지 않게 한다.
    QTimer* holdTimer_;
};
