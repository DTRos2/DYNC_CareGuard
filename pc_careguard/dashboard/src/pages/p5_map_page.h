#pragma once
#include <QWidget>
#include <QImage>
#include <QMap>
#include <QVector>

#include "net/api_client.h"

class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
class QMenu;
class SlamMappingCard;
class TeleopPanel;
class MappingStatusOverlay;
class QKeyEvent;
class QShowEvent;
class QFrame;
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
    // 방향키가 keyPressEvent까지 오려면 이 위젯이 실제로 키보드 포커스를 갖고 있어야
    // 한다 — setFocusPolicy(Qt::StrongFocus)는 "포커스를 받을 수 있다"는 허락일 뿐,
    // 실제로 포커스를 가져오는 건 아니다. QStackedWidget이 이 페이지로 전환될 때마다
    // showEvent가 불리는 걸 이용해 그때마다 setFocus()를 호출한다.
    void showEvent(QShowEvent* event) override;

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
    // WsClient가 끊기면(체크리스트 ⑩) 화면 상단에 보여줄 배너 — 재연결 시도 중임을
    // 알린다. WsClient 자체는 내부적으로 재연결을 이미 하고 있었지만, 그 상태를 화면에
    // 표시하는 곳이 없어 "화면이 멈췄는데 이유를 모르는" 증상으로만 보였다.
    QFrame* buildConnectionBanner();
    // 로봇(downlink_node)이 서버와 끊겼을 때 보여줄 배너 — connectionBanner_와 달리
    // "PC는 서버에 붙어있는데 로봇만 명령을 못 받는" 상황(실제로 겪은 버그)을 알린다.
    // teleop 실패가 조용히 무시되도록 짜여있어서, 이게 없으면 로봇이 안 움직이는데도
    // 이유를 모른 채 계속 조작하게 되는 안전 문제였다.
    QFrame* buildRobotOfflineBanner();
    void openFileRegisterDialog();
    void openMapPreviewDialog(const QString& robotId);
    void handleEmergencyStop();
    // 서버에 이미 배포된 활성 맵 목록을 조회해 매핑 현황 오버레이에 반영한다
    // (GET /maps/active). 새로 파일로 등록/SLAM 저장한 맵은 이 경로를 타지 않고
    // openFileRegisterDialog()에서 바로 markMapDeployed()를 부른다.
    void loadActiveMapsFromServer();
    // GET /robots — 맵 유무와 무관하게 등록된 전체 로봇 목록을 받아 헤더의 로봇
    // 드롭다운을 채운다(예전엔 "R-04" 고정 표시였다 — 2026-09-09 요청). 아직 맵이
    // 없는 로봇도 여기 나와야 매핑을 시작할 수 있어 GET /maps/active로는 못 만든다.
    void loadRobotsFromServer();
    void rebuildRobotMenu();
    // robotChipBtn_ 텍스트를 "코드 · 온라인/오프라인" 형태로 다시 그린다 —
    // switchToRobot()과 robot_status WS 처리(handleWsMessage) 둘 다에서 부른다.
    void updateRobotChipText();
    // 로봇 드롭다운에서 다른 로봇을 고르면 페이지 전체(SLAM 캔버스·온라인 배너·저장
    // 정보 카드·텔레옵/비상정지 대상)가 그 로봇 기준으로 바뀐다. P6PatrolPage::
    // switchToMap과 같은 패턴 — 다만 P5는 로봇당 편집 가능한 상태(웨이포인트 등)가
    // 없어서 저장·복원 없이 바로 로봇 코드만 바꾸고 캔버스를 새로 채운다.
    void switchToRobot(int index);
    // "매핑 시작" 버튼 — POST /slam-jobs로 현재 로봇의 SLAM 매핑을 시작시킨다
    // (체크리스트 ⑧). 화면상 텔레옵·SlamMapView 시뮬레이션은 서버 연동 여부와 무관하게
    // 그대로 동작한다 — 이 호출은 실제 로봇에게 명령을 전달하는 별도 경로다.
    void startMappingOnServer();
    // "매핑 취소" — 진행 중인 slam_job을 서버에 취소 요청한다(로봇의 cartographer
    // 프로세스도 같이 종료됨). "매핑 재시작"도 내부적으로 이걸 먼저 부른 뒤
    // startMappingOnServer()로 새 job을 다시 시작한다.
    void cancelMappingOnServer();
    void restartMappingOnServer();
    // /ws/dashboard 메시지 — slam_progress/slam_done을 현재 로봇 기준으로 반영한다
    // (체크리스트 ⑨).
    void handleWsMessage(const QJsonObject& message);

    QFrame* connectionBanner_;
    QFrame* robotOfflineBanner_;
    // 서버로부터 robot_status를 아직 못 받았으면 "모른다"가 아니라 안전하게
    // 오프라인으로 간주한다 — 그래서 기본값이 false다.
    bool robotOnline_ = false;
    MappingStatusOverlay* statusOverlay_;
    SlamMappingCard* slamCard_;
    TeleopPanel* teleopPanel_;
    QPushButton* robotChipBtn_;
    QMenu* robotMenu_;
    QVector<ApiClient::RobotInfo> robots_;
    int currentRobotIndex_ = -1;
    // "맵 저장 정보" 카드의 맵 이름 입력칸 — "SLAM 맵 저장·배포" 클릭 시 이 값을 실제
    // 저장될 이름으로 서버에 넘겨야 해서 멤버로 들고 있는다("매핑 시작" 시점엔 아직
    // 로봇이 지도를 다 그리기 전이라 이름이 확정 안 됐을 수 있어, 이름 확정은 저장
    // 시점 기준으로 한다).
    QLineEdit* mapNameEdit_;
    QMap<QString, RegisteredMapInfo> registeredMaps_;
    ApiClient* apiClient_;
    WsClient* wsClient_;
    // 진행 중인 slam_job id. 0이면 진행 중인 매핑이 없다는 뜻(취소/재시작 버튼이
    // 서버에 뭘 취소해야 할지 알아야 해서 필요) — startMappingOnServer 성공 시 채우고,
    // cancelMappingOnServer 성공 시 0으로 되돌린다.
    int currentSlamJobId_ = 0;
    // 헤더 로봇 드롭다운에서 선택된 로봇 — GET /robots 응답이 오기 전 기본값은 "R-04"
    // (기존 하드코딩 값과 동일하게 유지). 매핑 시작·WS 필터링·텔레옵 전송 대상이
    // 전부 이 값을 따른다. switchToRobot()이 바뀔 때마다 갱신한다.
    QString currentRobotCode_ = QStringLiteral("R-04");
};
