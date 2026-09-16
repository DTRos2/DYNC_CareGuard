#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// 서버 API 클라이언트. loginwindow.cpp의 QNetworkAccessManager 사용 패턴을 따른다 —
// 서버 주소 결정 방식은 net/server_config.h::resolveServerUrl() 참고.
class ApiClient : public QObject {
    Q_OBJECT
public:
    // server/app/schemas.py::RobotItem 대응. GET /maps/active와 달리 아직 맵이
    // 없는(SLAM 전) 로봇도 포함한다 — P5MapPage의 로봇 선택 드롭다운이 쓴다.
    struct RobotInfo {
        int id = 0;
        QString code;   // 예: "R-01"
        QString name;   // 예: "1호기"
        QString model;  // 예: "limo"
        QString ip;     // 비어있으면 없음
        // 서버가 매 호출마다 실시간으로 계산해 내려준다(robots.py::is_connected) —
        // 예전엔 이 필드를 파싱 안 하고 버렸다(P6PatrolPage::refreshRobotState가
        // 새로고침 시점의 실제 온라인 여부를 알아야 해서 추가, 2026-09-10).
        bool isOnline = false;
        // robot_ws.py::nav_running 캐시(마지막으로 받은 nav_status WS 메시지) — P6의
        // navRunning_이 WS 메시지 하나만 놓쳐도 새로고침으로 복구가 안 되던 문제를
        // 고치려고 추가(2026-09-10). 로봇이 한 번도 nav_status를 안 보냈으면 false.
        bool isNavRunning = false;
    };

    // server/app/schemas.py::MapItem 대응.
    struct MapInfo {
        int id = 0;
        int robotId = 0;
        QString robotCode;       // robot.code, 예: "R-01"
        QString name;            // 맵 이름
        QString recipientName;   // 담당 대상자. 미배정이면 빈 문자열
        bool isActive = false;
        int imageFileId = 0;     // 0이면 등록된 맵 이미지 없음
        QString imageUrl;        // "/media/files/{file_id}". 비어있으면 없음
        // 캔버스 픽셀 <-> 실제 map 프레임 미터 환산에 필요 (map_server .yaml과 같은 값).
        // 서버는 이미 내려주고 있었는데 예전엔 여기서 안 읽고 캔버스 쪽이 고정 배율
        // (kPxPerMeter=40)을 썼다 — 실제 지도 해상도(대개 0.05)와 달라 "이동" 목표가
        // 화면에서 클릭한 지점과 실제로 다른 곳이 되는 원인이었다.
        double resolution = 0.0;  // m/px, 0이면 서버가 아직 못 줌(예: 미등록 맵)
        double originX = 0.0;
        double originY = 0.0;
    };

    // server/app/schemas.py::WaypointItem 대응.
    struct WaypointInfo {
        int id = 0;
        int mapId = 0;
        QString code;  // 예: WP_01, CHARGE
        QString name;
        QString kind;  // "patrol" / "charge" / "emergency"
        double posX = 0.0;  // 미터 단위
        double posY = 0.0;
        double yaw = 0.0;   // 라디안
    };

    // server/app/schemas.py::PatrolRouteItem 대응.
    struct PatrolRouteInfo {
        int id = 0;
        int mapId = 0;
        QString name;
        QVector<int> steps;  // 방문할 waypoint id 순서
    };

    // server/app/schemas.py::PatrolScheduleItem 대응.
    struct PatrolScheduleInfo {
        int id = 0;
        int routeId = 0;
        QString runTime;    // "HH:mm"
        int daysOfWeek = 0; // 요일 비트마스크. 월=1 화=2 수=4 목=8 금=16 토=32 일=64
        bool enabled = true;
        // 이 회차가 매칭된 medication_schedule.id. 0 = 매칭 안 함(서버는 null로 내려줌).
        int medicationScheduleId = 0;
    };

    // server/app/schemas.py::SlamJobItem 대응.
    struct SlamJobInfo {
        int id = 0;
        QString status;     // "queued" / "running" / "saved" / "failed" / "cancelled"
        bool robotConnected = false;
    };

    // server/app/schemas.py::PatrolRunItem 대응.
    struct PatrolRunInfo {
        int id = 0;
        int robotId = 0;  // robot.id — P1OverviewPage가 robot_id 없이 조회할 때(전체
                           // 로봇 대상 status=running) 어느 로봇의 실행인지 구분하는 용도.
        QString status;  // "running" / "paused"(ESP32 SOS가 밀어냄) / "completed" / "aborted" / "failed"
        int waypointsTotal = 0;
        int waypointsDone = 0;
        bool robotConnected = false;
        // "manual"(대시보드 버튼) / "schedule"(services/scheduler.py 자동 트리거) —
        // P6PatrolPage가 상태 바에 "스케줄 주행 중"을 구분해 보여주는 데 쓴다
        // (수동 주행 버튼은 트리거 종류와 무관하게 이미 막혀 있었지만, 왜 막혔는지
        // 문구로는 구분이 안 됐다).
        QString trigger;
    };

    // server/app/schemas.py::MissionItem 대응.
    struct MissionInfo {
        int id = 0;
        QString status;  // "dispatched" 등 mission.status 값
        bool robotConnected = false;
    };

    // server/app/schemas.py::GuardianItem 대응.
    struct GuardianInfo {
        int id = 0;
        QString name;
        QString phone;
        QString relation;
        int priority = 1;
        bool notifyEnabled = true;
    };

    // server/app/schemas.py::MedicationScheduleItem 대응.
    struct MedicationScheduleInfo {
        int id = 0;
        int slotNo = 1;
        QString drugName;
        QString doseTime;    // "HH:mm" (서버는 "HH:MM:SS"로 내려주며 초는 잘라낸다)
        int daysOfWeek = 127;  // 요일 비트마스크. 월=1 화=2 수=4 목=8 금=16 토=32 일=64
    };

    // server/app/schemas.py::MedicationTodayItem 대응. 오늘 요일에 스케줄된 슬롯만 담긴다
    // (대상자마다 슬롯 수가 다를 수 있어 항상 3개가 아님).
    struct MedicationTodayInfo {
        int slotNo = 1;
        QString state;  // "done" / "missed" / "pending"
    };

    // server/app/schemas.py::MedicationHistoryDay 대응.
    struct MedicationHistoryInfo {
        QString date;          // "YYYY-MM-DD"
        double adherence = -1.0;  // 1.0 정상 · 0.0 미수령 · -1.0 그날 스케줄 없음(빈칸)
    };

    // server/app/schemas.py::RecipientItem 대응.
    struct RecipientInfo {
        int id = 0;
        QString code;         // "C-001"
        QString name;
        int robotId = 0;      // 0 = 미배정
        QString robotCode;    // 미배정이면 빈 문자열
        QString gender;       // "male" / "female" / "other". 미상이면 빈 문자열
        QString birthDate;    // "YYYY-MM-DD". 미상이면 빈 문자열
        QString homeAddress;
        QString diseases;
        QString note;
        int operatorId = 0;    // 담당 관제담당자 계정 id. 0 = 미배정
        QString operatorName;  // 미배정이면 빈 문자열
        // "normal"/"attention"/"emergency" — 로봇 오프라인 여부는 안 실려 있다(WS robot_status로
        // 대시보드가 별도 판정 후 이 값과 조합).
        QString level = QStringLiteral("normal");
        QString statusSummary;
        QVector<MedicationTodayInfo> medsToday;
    };

    // 대상자 상세/등록 모달의 "관리 담당자" 콤보를 채우는 후보 목록. server/app/accounts.py::
    // list_accounts()가 반환하는 AccountItem 중 role_code == "operator"인 것만 추린 것 —
    // 계정관리(P7)처럼 담당 배정 전용 엔드포인트를 새로 만들지 않고 이미 있는 GET /accounts를
    // 그대로 재사용한다(계정 수는 적어 필터 비용이 무시할 만하다).
    struct OperatorInfo {
        int id = 0;
        QString name;  // careguard_user.display_name
    };

    // server/app/schemas.py::RecipientDetailItem 대응.
    struct RecipientDetailInfo {
        RecipientInfo recipient;
        QVector<GuardianInfo> guardians;
        QVector<MedicationScheduleInfo> schedules;
        QVector<MedicationHistoryInfo> history;  // 최근 28일. 목록(RecipientInfo)에는 없음
    };

    // server/app/schemas.py::EventItem 대응.
    struct EventInfo {
        QString eventUid;
        QString severity;        // "info" / "attention" / "emergency"
        QString eventType;       // care_event.event_type 원본 코드
        QString eventTypeLabel;  // 한글 라벨(서버 event.py::EVENT_TYPE_LABELS로 변환됨)
        QString summary;
        QDateTime occurredAt;
        bool resolved = false;
        QString robotCode;      // 로봇 없는 이벤트면 빈 문자열
        QString recipientName;  // 대상자 없는 시스템 이벤트면 빈 문자열
    };

    template <typename T>
    using ListCallback = std::function<void(bool success, const QVector<T>& items)>;
    template <typename T>
    using ItemCallback = std::function<void(bool success, const T& item, const QString& errorDetail)>;
    using SimpleCallback = std::function<void(bool success, const QString& errorDetail)>;

    explicit ApiClient(QObject* parent = nullptr);

    // GET /robots — 등록된 전체 로봇 목록(맵 유무 무관). P5 로봇 선택 드롭다운용.
    void fetchRobots(ListCallback<RobotInfo> callback);
    // GET /maps/active — 로봇별 현재 활성 맵 목록.
    void fetchActiveMaps(ListCallback<MapInfo> callback);
    // GET /waypoints?map_id= — 맵 위 활성 웨이포인트 목록.
    void fetchWaypoints(int mapId, ListCallback<WaypointInfo> callback);
    // GET /patrol-routes?map_id= — 순찰 경로 목록. mapId<=0이면 전체 조회.
    void fetchPatrolRoutes(int mapId, ListCallback<PatrolRouteInfo> callback);
    // GET /patrol-schedules?route_id= — 경로 실행 예약 목록.
    void fetchPatrolSchedules(int routeId, ListCallback<PatrolScheduleInfo> callback);

    // GET <path>의 원본 바이트를 그대로 받는다 — 맵 이미지(.pgm)처럼 JSON이 아닌 응답용.
    // MapInfo::imageUrl("/media/files/{id}") 같은 상대 경로를 그대로 넘기면 된다.
    void fetchBytes(const QString& path, std::function<void(bool, const QByteArray&)> callback);

    // POST/PUT/DELETE /waypoints — 웨이포인트 등록·수정·삭제.
    void createWaypoint(int mapId, const QString& code, const QString& name, const QString& kind,
                         double posX, double posY, double yawRad, ItemCallback<WaypointInfo> callback);
    void updateWaypoint(int waypointId, const QJsonObject& fields, ItemCallback<WaypointInfo> callback);
    void deleteWaypoint(int waypointId, SimpleCallback callback);

    // POST/PUT/DELETE /patrol-routes — 순찰 경로 등록·수정·삭제.
    void createPatrolRoute(int mapId, const QString& name, const QVector<int>& steps,
                            ItemCallback<PatrolRouteInfo> callback);
    void updatePatrolRoute(int routeId, const QJsonObject& fields, ItemCallback<PatrolRouteInfo> callback);
    void deletePatrolRoute(int routeId, SimpleCallback callback);

    // POST/PUT/DELETE /patrol-schedules — 순찰 스케줄 등록·수정(요일/시각/enabled 토글)·삭제.
    // medicationScheduleId>0이면 이 회차가 매칭된 medication_schedule.id를 같이 보낸다
    // (0이면 안 보냄 — "복약 전달" 경로가 아니거나 매칭 안 한 경우).
    void createPatrolSchedule(int routeId, const QString& runTime, int daysOfWeek,
                               ItemCallback<PatrolScheduleInfo> callback, int medicationScheduleId = 0);
    void updatePatrolSchedule(int scheduleId, const QJsonObject& fields,
                               ItemCallback<PatrolScheduleInfo> callback);
    void deletePatrolSchedule(int scheduleId, SimpleCallback callback);

    // POST /slam-jobs — 매핑 시작. robots.py가 이번 스코프 밖이라 robot.code로 보낸다.
    void startSlamJob(const QString& robotCode, const QString& mapName, ItemCallback<SlamJobInfo> callback);
    // POST /slam-jobs/{id}/cancel — P5 "초기화"(매핑 취소). "매핑 시작"은 이후 사용자가
    // 별도로 다시 눌러야 하는 독립된 동작이다(자동 재시작 체인 없음).
    void cancelSlamJob(int jobId, ItemCallback<SlamJobInfo> callback);
    // GET /slam-jobs?robot_id=&status= — P6PatrolPage::refreshRobotState()가 "이 로봇이
    // 지금 SLAM 매핑 중인지"를 DB 기준으로 맞추는 데 쓴다(status="queued"로 걸면
    // 결과 유무만 보면 됨 — slam_job.status는 시작 시 'queued'로 들어간 뒤 취소/저장
    // 으로만 바뀌므로 'queued'가 곧 "진행 중"이다). GET /patrol-runs와 같은 패턴.
    void fetchSlamJobs(int robotId, const QString& status, ListCallback<SlamJobInfo> callback);
    // POST /slam-jobs/{id}/save — "SLAM 맵 저장·배포". mapName은 "매핑 시작" 때 넣은
    // 임시 이름을 실제 저장할 이름으로 덮어쓴다(비워두면 기존 이름 유지). 이 응답은
    // "로봇에 명령을 보냈다"는 뜻이지 저장이 끝났다는 뜻이 아니다 — 실제 완료는 나중에
    // WS로 오는 slam_done을 봐야 한다.
    void saveSlamJob(int jobId, const QString& mapName, ItemCallback<SlamJobInfo> callback);

    // POST /media/upload-map(멀티파트) — "파일로 등록" 모달이 로컬에서 고른 .pgm을
    // 실제로 서버에 올린다. 성공하면 files.id(file_id)를 콜백에 넘긴다.
    void uploadMapFile(const QString& robotCode, const QString& pgmFilePath, ItemCallback<int> callback);

    // POST /maps — 위 업로드로 받은 file_id + .yaml에서 읽은 메타데이터로 맵을 확정한다.
    // server/app/schemas.py::MapCreate 대응.
    struct MapCreateParams {
        QString robotCode;
        QString mapName;
        int fileId = 0;
        double resolution = 0.0;
        double originX = 0.0;
        double originY = 0.0;
        double originYaw = 0.0;
        int widthPx = 0;
        int heightPx = 0;
        QJsonObject rawYaml;
    };
    void createMap(const MapCreateParams& params, ItemCallback<MapInfo> callback);

    // POST /patrol-runs — 순찰 시작.
    void startPatrolRun(int robotId, int routeId, const QString& trigger, ItemCallback<PatrolRunInfo> callback);
    // GET /patrol-runs?robot_id=&status= — P6PatrolPage::refreshRobotState()가 페이지
    // 로드·맵 전환·새로고침 버튼 시점에 "이 로봇이 지금 실제로 주행 중인지"를 DB
    // 기준으로 다시 맞추는 데 쓴다(status="running"으로 걸면 결과 유무만 보면 됨).
    void fetchPatrolRuns(int robotId, const QString& status, ListCallback<PatrolRunInfo> callback);

    // POST /missions — P3 "복약 재안내" 버튼. medication_scheduler가 없어 스케줄을 안 거치고
    // 즉시 mission을 만들어 로봇에 다운링크를 보낸다(robots.py 스코프 밖이라 robot.code로 보냄).
    // 로봇 쪽은 아직 이 명령을 실행하지 않는다 — mission 기록·다운링크 전송까지만 실동작.
    void startMedicationMission(const QString& robotCode, ItemCallback<MissionInfo> callback);

    // GET /recipients — P3 대상자 목록. 위험도(level)·상태 요약·복약 배지는 care_event
    // 연동이 아직 없어 이 응답에 없다 — 호출부가 등록 직후 기본값(정상/미확인)을 채운다.
    // guardianUserId>0이면 이 보호자 계정이 연결된 대상자만 받는다(role=guardian 대시보드 필터).
    void fetchRecipients(ListCallback<RecipientInfo> callback, int guardianUserId = 0);
    // GET /recipients/{id} — P3 상세 모달이 여는 대상자 1명의 보호자·복약 스케줄까지.
    void fetchRecipientDetail(int recipientId, ItemCallback<RecipientDetailInfo> callback);

    // POST /recipients — "대상자 등록" 모달. server/app/schemas.py::RecipientCreate 대응.
    struct GuardianCreateParams {
        QString name;
        QString phone;
        QString relation;
        int priority = 1;
        bool notifyEnabled = true;
    };
    struct MedicationScheduleCreateParams {
        int slotNo = 1;
        QString drugName;
        QString doseTime;  // "HH:mm", 반드시 유효한 시각이어야 한다(호출 전 검증)
        int daysOfWeek = 127;
    };
    struct RecipientCreateParams {
        QString name;
        QString gender;  // "male" / "female" / "other" / 빈 문자열(미상)
        QString birthDate;  // "YYYY-MM-DD". 비어 있으면 서버에 안 보낸다
        QString homeAddress;
        QString diseases;
        QString note;
        QString robotCode;  // 비어 있으면 미배정
        int operatorId = 0;  // 0이면 서버에 안 보낸다 — RecipientCreate.operator_id가 None이 되어 미배정으로 저장됨
        QVector<GuardianCreateParams> guardians;
        QVector<MedicationScheduleCreateParams> schedules;
    };
    void createRecipient(const RecipientCreateParams& params, ItemCallback<RecipientDetailInfo> callback);
    // PUT /recipients/{id} — P3 대상자 상세 모달의 "저장". 필드 구성은 등록과 동일 —
    // 보호자·복약 스케줄은 지금 보낸 목록으로 서버가 통째로 교체한다.
    void updateRecipient(int recipientId, const RecipientCreateParams& params,
                          ItemCallback<RecipientDetailInfo> callback);
    // POST /recipients/{id}/deactivate — P3 상세 모달의 "대상자 비활성화". 하드 삭제가 아니라
    // recipient.deregistered_at을 찍어 목록·조회에서만 제외한다(서버 쪽 주석 참고).
    void deactivateRecipient(int recipientId, SimpleCallback callback);

    // GET /accounts — 대상자 등록/상세 모달의 "관리 담당자" 콤보 후보. role_code=="operator"
    // 이면서 활성 계정인 것만 추려서 넘긴다(비활성 계정에 새로 배정하는 건 막는다).
    void fetchOperators(ListCallback<OperatorInfo> callback);

    // GET /events — P2 이벤트 관제 큐. 기간·검색·미처리 필터는 대시보드가 이미 로컬로
    // 하고 있어(EventFilterProxyModel) 서버는 취소되지 않은 전체를 내려준다.
    void fetchEvents(ListCallback<EventInfo> callback);
    // POST /events/{event_uid}/resolve — 상세 모달 "RESOLVED 전환"·일괄 완료 처리가 공유하는
    // 진입점(둘 다 이벤트 하나씩 이 호출을 쓴다).
    void resolveEvent(const QString& eventUid, ItemCallback<EventInfo> callback);

    // POST /patrol-runs/drive-to-point — P6 "이동"(단일 웨이포인트 주행). 순찰과 달리
    // patrol_run을 기록하지 않고, 도착해도 충전소로 복귀하지 않는다(제자리에 머묾).
    // mapId는 downlink_node가 Nav2를 자동으로 준비하는 데 필요하다(맵 다운로드+런치+
    // 초기 위치).
    void driveToPoint(const QString& robotCode, int mapId, const QString& code, double xMeters,
                       double yMeters, double yawRad, SimpleCallback callback);

    // POST /patrol-runs/cancel — P6 웨이포인트 팝오버 "주행 취소". robot_code만 보내면
    // 서버가 로봇에 patrol_stop을 시도하고, 그 성공 여부와 무관하게 해당 로봇의
    // status='running' patrol_run 기록을 정리한다(robot_connected:false로 와도 "그냥
    // 실패"가 아니라 "로봇엔 못 갔지만 DB는 정리됨" — 정상 결과 중 하나).
    void cancelPatrolRun(const QString& robotCode, SimpleCallback callback);

    // 통화 시작/종료 결과 콜백 — robotConnected는 서버가 실제로 로봇 다운링크에
    // call_start/call_end를 전달할 수 있었는지(응답 body의 robot_connected)다. ok는
    // HTTP 요청 자체의 성공 여부라 둘을 구분해서 넘긴다(cancelPatrolRun과 같은 이유 —
    // 로봇이 오프라인이어도 요청 자체는 200으로 끝날 수 있다).
    using CallCallback = std::function<void(bool ok, bool robotConnected, const QString& errorDetail)>;

    // POST /calls/start, POST /calls/end — 이벤트 상세 모달 "스피커 통화" 버튼. 실제
    // 오디오 바이트는 이 REST 왕복과 무관하게 CallAudioClient가 별도 WebSocket으로
    // 주고받는다(audio_relay.py 참고) — 이 두 메서드는 로봇에 call_start/call_end
    // 신호만 전달한다.
    void startCall(const QString& robotCode, CallCallback callback);
    void endCall(const QString& robotCode, CallCallback callback);

    // POST /patrol-runs/prepare-navigation — 실제 순찰/이동 목표 없이 Nav2만 미리
    // 준비시킨다(P6 페이지를 열거나 다른 로봇의 맵으로 전환할 때 호출). 로봇이 아직
    // 안 켜져 있어도 에러가 아니라 그냥 robot_connected:false로 조용히 알려줄
    // 뿐이라, 이 호출은 실패해도 팝업을 띄우지 않는다(SimpleCallback 결과를 무시해도
    // 되게 설계) — 그래서 콜백 없는 오버로드를 둔다.
    void prepareNavigation(const QString& robotCode, int mapId);

    // POST /patrol-runs/set-initial-pose — "위치 보정"(캔버스 클릭+드래그로 위치·방향
    // 직접 지정). Nav2를 새로 켜지 않고, 이미 떠 있는 AMCL의 초기 위치 추정치만
    // 다시 잡는다 — 자동 세팅(충전 스테이션 좌표 가정)이 틀렸을 때 쓴다.
    void setInitialPose(const QString& robotCode, int mapId, double xMeters, double yMeters, double yawRad,
                         SimpleCallback callback);

    // POST /patrol-runs/shutdown-robot — "로봇 종료" 버튼. 이 로봇에서 켜져 있는
    // 베이스 구동·Nav2/SLAM·rviz2·업링크·다운링크를 전부 내린다.
    void shutdownRobot(const QString& robotCode, SimpleCallback callback);

    // POST /teleop/{robot_code} — 방향키 패널이 계산한 목표 속도를 로봇에 그대로 전달한다.
    void sendTeleop(const QString& robotCode, double linearMps, double angularRadps);

private:
    void getJsonArray(const QString& path, std::function<void(bool, const QByteArray&)> callback);
    void postJson(const QString& path, const QJsonObject& body,
                  std::function<void(bool, const QByteArray&, const QString&)> callback);
    void putJson(const QString& path, const QJsonObject& body,
                 std::function<void(bool, const QByteArray&, const QString&)> callback);
    void sendDelete(const QString& path, SimpleCallback callback);

    static RobotInfo parseRobot(const QJsonObject& o);
    static WaypointInfo parseWaypoint(const QJsonObject& o);
    static MapInfo parseMap(const QJsonObject& o);
    static PatrolRouteInfo parsePatrolRoute(const QJsonObject& o);
    static PatrolRunInfo parsePatrolRun(const QJsonObject& o);
    static SlamJobInfo parseSlamJob(const QJsonObject& o);
    static PatrolScheduleInfo parsePatrolSchedule(const QJsonObject& o);
    static RecipientInfo parseRecipient(const QJsonObject& o);
    static RecipientDetailInfo parseRecipientDetail(const QJsonObject& o);
    static EventInfo parseEvent(const QJsonObject& o);

    QNetworkAccessManager* netManager_;
    QString serverUrl_;
};
