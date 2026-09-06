#include "net/api_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QUrl>
#include <QUrlQuery>

ApiClient::ApiClient(QObject* parent) : QObject(parent) {
    serverUrl_ = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_URL"), QStringLiteral("http://127.0.0.1:8000"));
    netManager_ = new QNetworkAccessManager(this);
}

void ApiClient::getJsonArray(const QString& path, std::function<void(bool, const QByteArray&)> callback) {
    QNetworkRequest request(QUrl(serverUrl_ + path));
    QNetworkReply* reply = netManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = statusCode == 200;
        callback(ok, ok ? reply->readAll() : QByteArray());
        reply->deleteLater();
    });
}

void ApiClient::fetchBytes(const QString& path, std::function<void(bool, const QByteArray&)> callback) {
    // 원본 바이트를 그대로 넘기면 되는 요청이라 내부적으로 getJsonArray()와 동일하게 동작한다.
    getJsonArray(path, callback);
}

namespace {
// 실패 응답 본문에서 FastAPI 표준 에러 형식({"detail": "..."})을 뽑아낸다. 파싱 실패
// 시엔 원본 바이트를 그대로 문자열로 보여준다(디버깅용).
QString extractErrorDetail(const QByteArray& body) {
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (o.contains(QStringLiteral("detail"))) return o.value(QStringLiteral("detail")).toString();
    return QString::fromUtf8(body);
}
}  // namespace

void ApiClient::postJson(const QString& path, const QJsonObject& body,
                          std::function<void(bool, const QByteArray&, const QString&)> callback) {
    QNetworkRequest request(QUrl(serverUrl_ + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [reply, callback] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray respBody = reply->readAll();
        const bool ok = statusCode >= 200 && statusCode < 300;
        callback(ok, respBody, ok ? QString() : extractErrorDetail(respBody));
        reply->deleteLater();
    });
}

void ApiClient::putJson(const QString& path, const QJsonObject& body,
                         std::function<void(bool, const QByteArray&, const QString&)> callback) {
    QNetworkRequest request(QUrl(serverUrl_ + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PUT", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [reply, callback] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray respBody = reply->readAll();
        const bool ok = statusCode >= 200 && statusCode < 300;
        callback(ok, respBody, ok ? QString() : extractErrorDetail(respBody));
        reply->deleteLater();
    });
}

void ApiClient::sendDelete(const QString& path, SimpleCallback callback) {
    QNetworkRequest request(QUrl(serverUrl_ + path));
    QNetworkReply* reply = netManager_->deleteResource(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray respBody = reply->readAll();
        const bool ok = statusCode >= 200 && statusCode < 300;
        callback(ok, ok ? QString() : extractErrorDetail(respBody));
        reply->deleteLater();
    });
}

void ApiClient::fetchActiveMaps(ListCallback<MapInfo> callback) {
    getJsonArray(QStringLiteral("/maps/active"), [callback](bool ok, const QByteArray& body) {
        QVector<MapInfo> maps;
        if (!ok) {
            callback(false, maps);
            return;
        }
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            const QJsonObject o = v.toObject();
            MapInfo m;
            m.id = o.value(QStringLiteral("id")).toInt();
            m.robotId = o.value(QStringLiteral("robot_id")).toInt();
            m.robotCode = o.value(QStringLiteral("robot_code")).toString();
            m.name = o.value(QStringLiteral("name")).toString();
            m.recipientName = o.value(QStringLiteral("recipient_name")).toString();
            m.isActive = o.value(QStringLiteral("is_active")).toBool();
            m.imageFileId = o.value(QStringLiteral("image_file_id")).toInt();
            m.imageUrl = o.value(QStringLiteral("image_url")).toString();
            maps.append(m);
        }
        callback(true, maps);
    });
}

ApiClient::WaypointInfo ApiClient::parseWaypoint(const QJsonObject& o) {
    WaypointInfo w;
    w.id = o.value(QStringLiteral("id")).toInt();
    w.mapId = o.value(QStringLiteral("map_id")).toInt();
    w.code = o.value(QStringLiteral("code")).toString();
    w.name = o.value(QStringLiteral("name")).toString();
    w.kind = o.value(QStringLiteral("kind")).toString();
    w.posX = o.value(QStringLiteral("pos_x")).toDouble();
    w.posY = o.value(QStringLiteral("pos_y")).toDouble();
    w.yaw = o.value(QStringLiteral("yaw")).toDouble();
    return w;
}

void ApiClient::fetchWaypoints(int mapId, ListCallback<WaypointInfo> callback) {
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("map_id"), QString::number(mapId));
    getJsonArray(QStringLiteral("/waypoints?") + query.toString(QUrl::FullyEncoded),
                 [callback](bool ok, const QByteArray& body) {
                     QVector<WaypointInfo> waypoints;
                     if (!ok) {
                         callback(false, waypoints);
                         return;
                     }
                     for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
                         waypoints.append(parseWaypoint(v.toObject()));
                     }
                     callback(true, waypoints);
                 });
}

void ApiClient::createWaypoint(int mapId, const QString& code, const QString& name, const QString& kind,
                                double posX, double posY, double yawRad,
                                ItemCallback<WaypointInfo> callback) {
    QJsonObject body{{"map_id", mapId}, {"code", code},  {"name", name},
                      {"kind", kind},   {"pos_x", posX}, {"pos_y", posY},
                      {"yaw", yawRad}};
    postJson(QStringLiteral("/waypoints"), body,
             [callback](bool ok, const QByteArray& respBody, const QString& err) {
                 callback(ok, ok ? parseWaypoint(QJsonDocument::fromJson(respBody).object()) : WaypointInfo{}, err);
             });
}

void ApiClient::updateWaypoint(int waypointId, const QJsonObject& fields, ItemCallback<WaypointInfo> callback) {
    putJson(QStringLiteral("/waypoints/%1").arg(waypointId), fields,
            [callback](bool ok, const QByteArray& respBody, const QString& err) {
                callback(ok, ok ? parseWaypoint(QJsonDocument::fromJson(respBody).object()) : WaypointInfo{}, err);
            });
}

void ApiClient::deleteWaypoint(int waypointId, SimpleCallback callback) {
    sendDelete(QStringLiteral("/waypoints/%1").arg(waypointId), callback);
}

ApiClient::PatrolRouteInfo ApiClient::parsePatrolRoute(const QJsonObject& o) {
    PatrolRouteInfo r;
    r.id = o.value(QStringLiteral("id")).toInt();
    r.mapId = o.value(QStringLiteral("map_id")).toInt();
    r.name = o.value(QStringLiteral("name")).toString();
    for (const QJsonValue& step : o.value(QStringLiteral("steps")).toArray()) {
        r.steps.append(step.toInt());
    }
    return r;
}

void ApiClient::fetchPatrolRoutes(int mapId, ListCallback<PatrolRouteInfo> callback) {
    QString path = QStringLiteral("/patrol-routes");
    if (mapId > 0) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("map_id"), QString::number(mapId));
        path += QStringLiteral("?") + query.toString(QUrl::FullyEncoded);
    }
    getJsonArray(path, [callback](bool ok, const QByteArray& body) {
        QVector<PatrolRouteInfo> routes;
        if (!ok) {
            callback(false, routes);
            return;
        }
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            routes.append(parsePatrolRoute(v.toObject()));
        }
        callback(true, routes);
    });
}

void ApiClient::createPatrolRoute(int mapId, const QString& name, const QVector<int>& steps,
                                   ItemCallback<PatrolRouteInfo> callback) {
    QJsonArray stepsArray;
    for (int step : steps) stepsArray.append(step);
    QJsonObject body{{"map_id", mapId}, {"name", name}, {"steps", stepsArray}};
    postJson(QStringLiteral("/patrol-routes"), body,
             [callback](bool ok, const QByteArray& respBody, const QString& err) {
                 callback(ok, ok ? parsePatrolRoute(QJsonDocument::fromJson(respBody).object()) : PatrolRouteInfo{},
                          err);
             });
}

void ApiClient::updatePatrolRoute(int routeId, const QJsonObject& fields,
                                   ItemCallback<PatrolRouteInfo> callback) {
    putJson(QStringLiteral("/patrol-routes/%1").arg(routeId), fields,
            [callback](bool ok, const QByteArray& respBody, const QString& err) {
                callback(ok, ok ? parsePatrolRoute(QJsonDocument::fromJson(respBody).object()) : PatrolRouteInfo{},
                         err);
            });
}

void ApiClient::deletePatrolRoute(int routeId, SimpleCallback callback) {
    sendDelete(QStringLiteral("/patrol-routes/%1").arg(routeId), callback);
}

ApiClient::PatrolScheduleInfo ApiClient::parsePatrolSchedule(const QJsonObject& o) {
    PatrolScheduleInfo s;
    s.id = o.value(QStringLiteral("id")).toInt();
    s.routeId = o.value(QStringLiteral("route_id")).toInt();
    // 서버는 "HH:MM:SS"를 내려준다 — GUI 표시 형식("HH:mm")에 맞춰 초 단위를 자른다.
    s.runTime = o.value(QStringLiteral("run_time")).toString().left(5);
    s.daysOfWeek = o.value(QStringLiteral("days_of_week")).toInt();
    s.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    return s;
}

void ApiClient::fetchPatrolSchedules(int routeId, ListCallback<PatrolScheduleInfo> callback) {
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("route_id"), QString::number(routeId));
    getJsonArray(QStringLiteral("/patrol-schedules?") + query.toString(QUrl::FullyEncoded),
                 [callback](bool ok, const QByteArray& body) {
                     QVector<PatrolScheduleInfo> schedules;
                     if (!ok) {
                         callback(false, schedules);
                         return;
                     }
                     for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
                         schedules.append(parsePatrolSchedule(v.toObject()));
                     }
                     callback(true, schedules);
                 });
}

void ApiClient::createPatrolSchedule(int routeId, const QString& runTime, int daysOfWeek,
                                      ItemCallback<PatrolScheduleInfo> callback) {
    QJsonObject body{
        {"route_id", routeId}, {"run_time", runTime + QStringLiteral(":00")}, {"days_of_week", daysOfWeek}};
    postJson(QStringLiteral("/patrol-schedules"), body,
             [callback](bool ok, const QByteArray& respBody, const QString& err) {
                 callback(ok,
                          ok ? parsePatrolSchedule(QJsonDocument::fromJson(respBody).object())
                             : PatrolScheduleInfo{},
                          err);
             });
}

void ApiClient::updatePatrolSchedule(int scheduleId, const QJsonObject& fields,
                                      ItemCallback<PatrolScheduleInfo> callback) {
    putJson(QStringLiteral("/patrol-schedules/%1").arg(scheduleId), fields,
            [callback](bool ok, const QByteArray& respBody, const QString& err) {
                callback(ok,
                         ok ? parsePatrolSchedule(QJsonDocument::fromJson(respBody).object())
                            : PatrolScheduleInfo{},
                         err);
            });
}

void ApiClient::deletePatrolSchedule(int scheduleId, SimpleCallback callback) {
    sendDelete(QStringLiteral("/patrol-schedules/%1").arg(scheduleId), callback);
}

void ApiClient::startSlamJob(const QString& robotCode, const QString& mapName,
                              ItemCallback<SlamJobInfo> callback) {
    QJsonObject body{{"robot_code", robotCode}, {"map_name", mapName}};
    postJson(QStringLiteral("/slam-jobs"), body,
             [callback](bool ok, const QByteArray& respBody, const QString& err) {
                 if (!ok) {
                     callback(false, SlamJobInfo{}, err);
                     return;
                 }
                 const QJsonObject o = QJsonDocument::fromJson(respBody).object();
                 SlamJobInfo job;
                 job.id = o.value(QStringLiteral("id")).toInt();
                 job.status = o.value(QStringLiteral("status")).toString();
                 job.robotConnected = o.value(QStringLiteral("robot_connected")).toBool();
                 callback(true, job, err);
             });
}

void ApiClient::startPatrolRun(int robotId, int routeId, const QString& trigger,
                                ItemCallback<PatrolRunInfo> callback) {
    QJsonObject body{{"robot_id", robotId}, {"route_id", routeId}, {"trigger", trigger}};
    postJson(QStringLiteral("/patrol-runs"), body,
             [callback](bool ok, const QByteArray& respBody, const QString& err) {
                 if (!ok) {
                     callback(false, PatrolRunInfo{}, err);
                     return;
                 }
                 const QJsonObject o = QJsonDocument::fromJson(respBody).object();
                 PatrolRunInfo run;
                 run.id = o.value(QStringLiteral("id")).toInt();
                 run.status = o.value(QStringLiteral("status")).toString();
                 run.waypointsTotal = o.value(QStringLiteral("waypoints_total")).toInt();
                 run.waypointsDone = o.value(QStringLiteral("waypoints_done")).toInt();
                 run.robotConnected = o.value(QStringLiteral("robot_connected")).toBool();
                 callback(true, run, err);
             });
}

void ApiClient::sendTeleop(const QString& robotCode, double linearMps, double angularRadps) {
    // 방향키를 누르고 있는 동안 계속 호출되는 hot path라 응답을 기다리지 않는다 —
    // 실패해도(로봇 미연결 등) 조용히 무시한다. 위험 상황은 SlamMappingCard의
    // 비상정지가 별도로 처리한다.
    QJsonObject body{{"linear", linearMps}, {"angular", angularRadps}};
    postJson(QStringLiteral("/teleop/%1").arg(robotCode), body, [](bool, const QByteArray&, const QString&) {});
}
