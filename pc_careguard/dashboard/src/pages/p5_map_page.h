#pragma once
#include <QWidget>
#include <QImage>
#include <QMap>

class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
class SlamMappingCard;
class TeleopPanel;
class MappingStatusOverlay;
class QKeyEvent;
class ApiClient;
class WsClient;
class QJsonObject;

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
    // 등록 화면(openFileRegisterDialog)에서 실제로 업로드된 맵 이미지를 robotId별로
    // 보관해 둔다 — MappingStatusOverlay는 이미지를 안 들고 텍스트 행만 관리하므로,
    // 미리보기 모달에 넘길 실제 이미지의 유일한 출처는 여기다.
    struct RegisteredMapInfo {
        QImage image;
        QString mapName;
        QString recipientName;
    };

    QWidget* buildHeaderRow();
    QWidget* buildSaveInfoCard();
    void openFileRegisterDialog();
    void openMapPreviewDialog(const QString& robotId);
    void handleEmergencyStop();
    // 서버에 이미 배포된 활성 맵 목록을 조회해 매핑 현황 오버레이에 반영한다
    // (GET /maps/active). 새로 파일로 등록/SLAM 저장한 맵은 이 경로를 타지 않고
    // openFileRegisterDialog()에서 바로 markMapDeployed()를 부른다.
    void loadActiveMapsFromServer();
    // "매핑 시작" 버튼 — POST /slam-jobs로 현재 로봇의 SLAM 매핑을 시작시킨다
    // (체크리스트 ⑧). 화면상 텔레옵·SlamMapView 시뮬레이션은 서버 연동 여부와 무관하게
    // 그대로 동작한다 — 이 호출은 실제 로봇에게 명령을 전달하는 별도 경로다.
    void startMappingOnServer();
    // /ws/dashboard 메시지 — slam_progress/slam_done을 현재 로봇 기준으로 반영한다
    // (체크리스트 ⑨).
    void handleWsMessage(const QJsonObject& message);

    MappingStatusOverlay* statusOverlay_;
    SlamMappingCard* slamCard_;
    TeleopPanel* teleopPanel_;
    QPushButton* robotChipBtn_;
    QMap<QString, RegisteredMapInfo> registeredMaps_;
    ApiClient* apiClient_;
    WsClient* wsClient_;
    // 헤더의 로봇 칩은 지금 "R-04" 고정 표시다(로봇 목록 API는 이번 스코프 밖) — 매핑
    // 시작·WS 필터링에 쓰는 로봇 코드도 이 상수를 그대로 따른다.
    QString currentRobotCode_ = QStringLiteral("R-04");
};
