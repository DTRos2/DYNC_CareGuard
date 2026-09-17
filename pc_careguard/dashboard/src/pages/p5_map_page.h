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
class CareGuardComboBox;
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

    // MainWindow가 앱 전체에서 공유하는 WsClient 하나를 내려준다 — 페이지마다 자기
    // WsClient를 따로 만들면 /ws/dashboard 소켓이 페이지 수만큼 중복으로 열린다.
    void setWsClient(WsClient* client);

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
        // 서버 map.id — "초기화" 확인 시 DELETE /maps/{id}를 부르려면 있어야 한다
        // (2026-09-16 요청 전까진 로컬 화면만 지우고 서버 행은 그대로 뒀다). 0이면
        // 아직 서버 응답을 못 받았다는 뜻 — 이 경우 삭제 호출은 건너뛴다.
        int mapId = 0;
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
    // 서버에 이미 배포된 활성 맵 목록을 조회해 매핑 현황 오버레이에 반영한다
    // (GET /maps/active). 새로 파일로 등록/SLAM 저장한 맵은 이 경로를 타지 않고
    // openFileRegisterDialog()에서 바로 markMapDeployed()를 부른다.
    void loadActiveMapsFromServer();
    // GET /patrol-runs?robot_id=&status=running(paused도 같이 잡힌다 — patrol.py
    // 참고)로 "지금 이 로봇이 순찰/이동 중인지"를 물어 robotPatrolling_을 맞추고
    // teleopPanel_을 잠근다. switchToRobot()과 handleWsMessage의 patrol_run_updated/
    // patrol_done 수신 시 부른다 — P6PatrolPage::refreshRobotState와 같은 이유(순찰
    // 중엔 텔레옵으로 로봇을 가로채면 Nav2 주행과 부딪힌다, 요청 사양).
    void refreshPatrolState();
    // GET /robots — 맵 유무와 무관하게 등록된 전체 로봇 목록을 받아 헤더의 로봇
    // 드롭다운을 채운다(예전엔 "R-04" 고정 표시였다 — 2026-09-09 요청). 아직 맵이
    // 없는 로봇도 여기 나와야 매핑을 시작할 수 있어 GET /maps/active로는 못 만든다.
    void loadRobotsFromServer();
    // WS 재연결 시 robots_의 온라인 상태만 다시 맞춘다 — P7SettingsPage::fetchRobots과
    // 같은 이유(재연결 중 놓친 robot_status push를 이걸로 따라잡는다, 2026-09-17
    // 발견: 셀렉트 창이 실제로는 온라인인 로봇을 계속 오프라인으로 보여주는 문제).
    // loadRobotsFromServer()를 그대로 재사용하면 rebuildRobotMenu()가 콤보를 통째로
    // 비웠다 채우면서 switchToRobot()까지 다시 타 currentSlamJobId_·registeredMaps_
    // 등 진행 중이던 화면 상태가 리셋되므로, 온라인 상태 필드만 갱신하는 가벼운
    // 경로를 따로 둔다.
    void refreshRobotOnlineStatus();
    // robots_ 기준으로 robotCombo_의 항목 목록을 다시 채운다 — GET /robots 응답이 올
    // 때만 부른다(로봇 목록 자체가 바뀔 때). 이름은 그대로 두지만 이제 QMenu가 아니라
    // CareGuardComboBox 항목을 채운다(대상자 페이지 모달의 로봇/맵 배정 콤보와 같은
    // 위젯 — 디자인 통일).
    void rebuildRobotMenu();
    // "코드 (이름) · 온라인/오프라인" 형태의 콤보 항목 텍스트를 만든다. rebuildRobotMenu()
    // (전체 로봇, robots_[i].isOnline 기준)와 updateRobotChipText()/handleWsMessage의
    // robot_status 처리(개별 로봇, 최신 값 기준) 양쪽에서 공유한다.
    QString formatRobotChipText(const ApiClient::RobotInfo& robot, bool online) const;
    // robotCombo_에서 지금 선택된 로봇 항목의 텍스트를 robotOnline_ 기준으로 다시
    // 그린다 — switchToRobot()이 로봇 전환 직후 안전하게 "오프라인"으로 강제한 상태를
    // 반영할 때 쓴다. 다른 로봇 항목은 handleWsMessage의 robot_status 처리가 각자
    // robots_[i].isOnline로 직접 갱신한다(2026-09-16부터 — 예전엔 선택된 로봇만
    // 갱신되고 드롭다운을 펼쳐도 다른 로봇 상태가 안 보였다).
    void updateRobotChipText();
    // 로봇 드롭다운에서 다른 로봇을 고르면 페이지 전체(SLAM 캔버스·온라인 배너·저장
    // 정보 카드·텔레옵 대상)가 그 로봇 기준으로 바뀐다. P6PatrolPage::
    // switchToMap과 같은 패턴 — 다만 P5는 로봇당 편집 가능한 상태(웨이포인트 등)가
    // 없어서 저장·복원 없이 바로 로봇 코드만 바꾸고 캔버스를 새로 채운다.
    void switchToRobot(int index);
    // 상단바의 "매핑 시작"/"초기화" 토글 버튼 클릭 — 진행 중인 slam_job이 없으면
    // startMappingOnServer(), 있으면 cancelMappingOnServer()로 라우팅한다. 1차
    // 프로젝트(MediRun) SlamMappingPage::m_startButton과 같은 토글 방식 — 별도의
    // "매핑 재시작"/"매핑 취소"/"비상정지" 버튼을 두지 않는다.
    void handleMappingToggleClicked();
    // "매핑 시작" — POST /slam-jobs로 현재 로봇의 SLAM 매핑을 시작시킨다(체크리스트 ⑧).
    // 화면상 텔레옵·SlamMapView 시뮬레이션은 서버 연동 여부와 무관하게 그대로 동작한다
    // — 이 호출은 실제 로봇에게 명령을 전달하는 별도 경로다.
    void startMappingOnServer();
    // "초기화"(handleMappingToggleClicked가 진행 중일 때 호출) — 진행 중인 slam_job을
    // 서버에 취소 요청한다(로봇의 cartographer 프로세스도 같이 종료됨). 화면(view_)도
    // 바로 지워 새 매핑을 시작할 준비 상태로 되돌린다.
    void cancelMappingOnServer();
    // startMappingBtn_ 텍스트("매핑 시작"/"초기화")와 saveMapBtn_ 활성화 여부를
    // currentSlamJobId_ 기준으로 다시 맞춘다 — 시작/취소/저장 완료(slam_done)/로봇
    // 전환(switchToRobot) 등 currentSlamJobId_가 바뀌는 모든 지점에서 부른다.
    void updateMappingButtonState();
    // /ws/dashboard 메시지 — slam_progress/slam_done을 현재 로봇 기준으로 반영한다
    // (체크리스트 ⑨).
    void handleWsMessage(const QJsonObject& message);

    QFrame* connectionBanner_;
    QFrame* robotOfflineBanner_;
    // 서버로부터 robot_status를 아직 못 받았으면 "모른다"가 아니라 안전하게
    // 오프라인으로 간주한다 — 그래서 기본값이 false다.
    bool robotOnline_ = false;
    // 이 로봇이 지금 순찰/이동(schedule/manual/ESP32 SOS 무관하게 patrol_run이
    // running 또는 paused) 중이면 true — refreshPatrolState()가 갱신한다. 기본값
    // false(로봇 전환 직후 "모른다"를 안전하게 "주행 아님"으로 두는 게 아니라 —
    // robotOnline_과 달리 이건 켜져 있으면 텔레옵을 "잠그는" 쪽이라, 모르는 동안
    // 잘못 잠가서 조작을 막는 것보다 응답이 올 때까지 잠깐 열려 있는 쪽이 안전하다).
    bool robotPatrolling_ = false;
    MappingStatusOverlay* statusOverlay_;
    SlamMappingCard* slamCard_;
    TeleopPanel* teleopPanel_;
    // 로봇 선택 — 대상자 페이지 모달(P3RecipientCreateDialog)의 로봇/맵 배정 콤보와
    // 같은 CareGuardComboBox를 쓴다(디자인 통일). 예전엔 QPushButton+QMenu로 흉내 낸
    // "칩" 버튼이었다.
    CareGuardComboBox* robotCombo_;
    // 상단바의 "매핑 시작"/"초기화" 토글 버튼 — updateMappingButtonState()가
    // currentSlamJobId_ 기준으로 텍스트를 바꾼다.
    QPushButton* startMappingBtn_;
    // "SLAM 맵 저장 · 배포" — updateMappingButtonState()가 currentSlamJobId_ != 0일
    // 때만 활성화한다(매핑 중이 아니면 저장할 대상 자체가 없다).
    QPushButton* saveMapBtn_;
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
    // 진행 중인 slam_job id. 0이면 진행 중인 매핑이 없다는 뜻(토글 버튼이 "초기화"를
    // 눌렀을 때 서버에 뭘 취소해야 할지 알아야 해서 필요) — startMappingOnServer 성공
    // 시 채우고, cancelMappingOnServer/slam_done 수신 시 0으로 되돌린다.
    int currentSlamJobId_ = 0;
    // 헤더 로봇 드롭다운에서 선택된 로봇 — GET /robots 응답이 오기 전 기본값은 "R-04"
    // (기존 하드코딩 값과 동일하게 유지). 매핑 시작·WS 필터링·텔레옵 전송 대상이
    // 전부 이 값을 따른다. switchToRobot()이 바뀔 때마다 갱신한다.
    QString currentRobotCode_ = QStringLiteral("R-04");
};
