#pragma once
#include <QWidget>
#include <QImage>

class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
class SlamMappingCard;
class TeleopPanel;
class MappingStatusOverlay;
class QKeyEvent;

// P5 맵·로봇 매핑 — SLAM 캔버스 + 텔레옵 + 매핑 현황 오버레이 + 파일 등록 모달.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 4), §7 참고.
class P5MapPage : public QWidget {
    Q_OBJECT
public:
    explicit P5MapPage(QWidget* parent = nullptr);

signals:
    // 파일로 맵을 등록·배포했을 때 — MainWindow가 P6PatrolPage로 중계해 맵 선택지에 반영한다.
    void mapRegistered(const QImage& mapImage, const QString& mapName, const QString& robotId,
                        const QString& recipientName);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    QWidget* buildHeaderRow();
    QWidget* buildSaveInfoCard();
    void openFileRegisterDialog();
    void handleEmergencyStop();

    MappingStatusOverlay* statusOverlay_;
    SlamMappingCard* slamCard_;
    TeleopPanel* teleopPanel_;
    QPushButton* robotChipBtn_;
};
