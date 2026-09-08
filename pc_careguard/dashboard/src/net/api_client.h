#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// 서버 API 클라이언트. loginwindow.cpp의 QNetworkAccessManager 사용 패턴을 따른다 —
// 서버 주소는 환경변수 CAREGUARD_SERVER_URL, 없으면 http://127.0.0.1:8000.
class ApiClient : public QObject {
    Q_OBJECT
public:
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
        QString kind;  // "patrol" / "charge"
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
        QString status;  // "running" / "completed" / "aborted" / "failed"
        int waypointsTotal = 0;
        int waypointsDone = 0;
        bool robotConnected = false;
    };

    // server/app/schemas.py::MissionItem 대응.
    struct MissionInfo {
        int id = 0;
        QString status;  // "dispatched" 등 mission.status 값
        bool robotConnected = false;
    };

    template <typename T>
    using ListCallback = std::function<void(bool success, const QVector<T>& items)>;
    template <typename T>
    using ItemCallback = std::function<void(bool success, const T& item, const QString& errorDetail)>;
    using SimpleCallback = std::function<void(bool success, const QString& errorDetail)>;

    explicit ApiClient(QObject* parent = nullptr);

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
    void createPatrolSchedule(int routeId, const QString& runTime, int daysOfWeek,
                               ItemCallback<PatrolScheduleInfo> callback);
    void updatePatrolSchedule(int scheduleId, const QJsonObject& fields,
                               ItemCallback<PatrolScheduleInfo> callback);
    void deletePatrolSchedule(int scheduleId, SimpleCallback callback);

    // POST /slam-jobs — 매핑 시작. robots.py가 이번 스코프 밖이라 robot.code로 보낸다.
    void startSlamJob(const QString& robotCode, const QString& mapName, ItemCallback<SlamJobInfo> callback);
    // POST /slam-jobs/{id}/cancel — 매핑 취소("매핑 재시작"은 이걸로 취소 후 startSlamJob을 다시 부른다).
    void cancelSlamJob(int jobId, ItemCallback<SlamJobInfo> callback);
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

    // POST /missions — P3 "복약 재안내" 버튼. medication_scheduler가 없어 스케줄을 안 거치고
    // 즉시 mission을 만들어 로봇에 다운링크를 보낸다(robots.py 스코프 밖이라 robot.code로 보냄).
    // 로봇 쪽은 아직 이 명령을 실행하지 않는다 — mission 기록·다운링크 전송까지만 실동작.
    void startMedicationMission(const QString& robotCode, ItemCallback<MissionInfo> callback);

    // POST /patrol-runs/drive-to-point — P6 "이동"(단일 웨이포인트 주행). 순찰과 달리
    // patrol_run을 기록하지 않고, 도착해도 충전소로 복귀하지 않는다(제자리에 머묾).
    // mapId는 downlink_node가 Nav2를 자동으로 준비하는 데 필요하다(맵 다운로드+런치+
    // 초기 위치).
    void driveToPoint(const QString& robotCode, int mapId, const QString& code, double xMeters,
                       double yMeters, double yawRad, SimpleCallback callback);

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

    static WaypointInfo parseWaypoint(const QJsonObject& o);
    static MapInfo parseMap(const QJsonObject& o);
    static PatrolRouteInfo parsePatrolRoute(const QJsonObject& o);
    static PatrolScheduleInfo parsePatrolSchedule(const QJsonObject& o);

    QNetworkAccessManager* netManager_;
    QString serverUrl_;
};
