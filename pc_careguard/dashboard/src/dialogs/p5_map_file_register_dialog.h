#pragma once
#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QVector>

#include "net/api_client.h"

class QFrame;
class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
class CareGuardComboBox;

// P5 "파일로 맵 등록" 모달 — p5_file_register_modal_wireframe.html 대응.
// .pgm/.yaml 한 쌍을 업로드해야 등록·배포 버튼이 활성화된다 (§6 P5 모달 항목).
// .pgm은 실제로 파싱해 미리보기에 렌더링하고, .yaml에서 resolution/origin도 읽는다
// (예전엔 .yaml을 파일명만 확인하고 내용은 안 읽었다 — 그래서 이 다이얼로그로 등록한
// 맵은 서버에 실제로 저장되지 않는 로컬 전용 미리보기에 불과했다). 등록·배포 확정 시
// mapRegistered로 이미지·맵 이름·로봇 ID를 전달해 P5MapPage가 실제 업로드(POST
// /media/upload-map -> POST /maps)와 SLAM 캔버스/매핑 현황 갱신을 담당한다 — 이
// 다이얼로그 자신은 서버 API를 직접 호출하지 않는다(ApiClient를 몰라도 된다).
class MapFileRegisterDialog : public QDialog {
    Q_OBJECT
public:
    explicit MapFileRegisterDialog(QWidget* parent = nullptr);

    // P5MapPage가 이미 GET /robots로 받아둔 로봇 목록·현재 선택 로봇을 그대로 넘겨준다
    // — 이 다이얼로그는 ApiClient를 직접 호출하지 않으므로(클래스 주석 참고) 호출부가
    // exec() 전에 반드시 불러줘야 한다. 예전엔 로봇이 "R-04"·"● 온라인" 고정값이라
    // 다른 로봇으로는 등록 자체가 불가능했다(2026-09-16 요청으로 실제 목록 연동).
    void setRobots(const QVector<ApiClient::RobotInfo>& robots, const QString& preselectedCode);

    // P5MapPage가 mapRegistered 시그널을 받은 시점에 이 값들로 실제 업로드를 수행한다.
    QString pgmFilePath() const { return pgmFilePath_; }
    double resolution() const { return resolution_; }
    double originX() const { return originX_; }
    double originY() const { return originY_; }
    double originYaw() const { return originYaw_; }
    QJsonObject rawYaml() const { return rawYaml_; }

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
    // map_saver_cli가 만드는 단순한 map.yaml(키: 값, origin은 [x,y,yaw] 배열)만
    // 다룬다 — 범용 YAML 파서가 아니다. resolution/origin이 없으면 실패로 취급한다.
    bool parseMapYaml(const QString& path);
    // .pgm/.yaml 선택 다이얼로그가 처음 열릴 기본 경로. 환경변수 CAREGUARD_MAPS_DIR로
    // 재정의 가능, 없으면 컴파일 타임 기본값(robot_careguard_ws/.../careguard_bringup/maps).
    static QString defaultMapsDir();
    void updateRegisterEnabled();
    void updatePreview();
    void handleRegisterClicked();
    // "코드 (이름) · 온라인/오프라인" 항목 텍스트 — P5MapPage::formatRobotChipText와
    // 같은 형식(디자인 통일, 2026-09-16 요청).
    QString formatRobotChipText(const ApiClient::RobotInfo& robot) const;
    // robotCombo_ 선택이 바뀔 때마다(사용자 선택 또는 setRobots()의 초기 선택) 하단
    // 상태 문구를 그 로봇 기준으로 다시 그린다.
    void updateRobotSelectionUi();

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
    // "로봇 매핑" 행 — GET /robots로 받은 실제 로봇 목록에서 고른다(예전엔 "R-04" 고정
    // QLabel이었다). P5MapPage 헤더의 로봇 선택 콤보와 같은 CareGuardComboBox +
    // objectName("robotChip")을 써서 모양·항목 텍스트 형식을 통일한다(2026-09-16 요청).
    CareGuardComboBox* robotCombo_;
    QVector<ApiClient::RobotInfo> robots_;
    QPushButton* registerBtn_;
    QLabel* footerStatus_;
    bool pgmLoaded_;
    bool yamlLoaded_;
    QImage pgmImage_;
    QString pgmFilePath_;
    double resolution_ = 0.0;
    double originX_ = 0.0;
    double originY_ = 0.0;
    double originYaw_ = 0.0;
    QJsonObject rawYaml_;
};
