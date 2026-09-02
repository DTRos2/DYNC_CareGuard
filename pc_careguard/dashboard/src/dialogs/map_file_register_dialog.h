#pragma once
#include <QDialog>
#include <QImage>

class QFrame;
class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;

// P5 "파일로 맵 등록" 모달 — p5_file_register_modal_wireframe.html 대응.
// .pgm/.yaml 한 쌍을 업로드해야 등록·배포 버튼이 활성화된다 (§6 P5 모달 항목).
// .pgm은 실제로 파싱해 미리보기에 렌더링하고, 등록·배포 확정 시 mapRegistered로
// 이미지·맵 이름·로봇 ID를 전달해 P5MapPage가 SLAM 캔버스/매핑 현황에 반영하게 한다.
class MapFileRegisterDialog : public QDialog {
    Q_OBJECT
public:
    explicit MapFileRegisterDialog(QWidget* parent = nullptr);

signals:
    void mapRegistered(const QImage& mapImage, const QString& mapName, const QString& robotId,
                        const QString& recipientName);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QFrame* buildHeader();
    QFrame* buildFooter();
    void pickPgmFile();
    void pickYamlFile();
    void updateRegisterEnabled();
    void updatePreview();
    void handleRegisterClicked();

    QFrame* pgmDropzone_;
    QLabel* pgmIcon_;
    QLabel* pgmLabel_;
    QLabel* pgmSubLabel_;
    QFrame* yamlDropzone_;
    QLabel* yamlIcon_;
    QLabel* yamlLabel_;
    QLabel* yamlSubLabel_;
    QFrame* warningBanner_;
    QLabel* previewHint_;
    QLabel* previewImage_;
    QLineEdit* nameEdit_;
    QComboBox* recipientCombo_;
    QPushButton* registerBtn_;
    QLabel* footerStatus_;
    bool pgmLoaded_;
    bool yamlLoaded_;
    QImage pgmImage_;
};
