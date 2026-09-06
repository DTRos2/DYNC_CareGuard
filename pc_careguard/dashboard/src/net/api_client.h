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

    // POST /patrol-runs — 순찰 시작.
    void startPatrolRun(int robotId, int routeId, const QString& trigger, ItemCallback<PatrolRunInfo> callback);

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
    static PatrolRouteInfo parsePatrolRoute(const QJsonObject& o);
    static PatrolScheduleInfo parsePatrolSchedule(const QJsonObject& o);

    QNetworkAccessManager* netManager_;
    QString serverUrl_;
};
