#include "net/api_client.h"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHttpPart>
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

ApiClient::RobotInfo ApiClient::parseRobot(const QJsonObject& o) {
    RobotInfo r;
    r.id = o.value(QStringLiteral("id")).toInt();
    r.code = o.value(QStringLiteral("code")).toString();
    r.name = o.value(QStringLiteral("name")).toString();
    r.model = o.value(QStringLiteral("model")).toString();
    r.ip = o.value(QStringLiteral("ip")).toString();
    return r;
}

void ApiClient::fetchRobots(ListCallback<RobotInfo> callback) {
    getJsonArray(QStringLiteral("/robots"), [callback](bool ok, const QByteArray& body) {
        QVector<RobotInfo> robots;
        if (!ok) {
            callback(false, robots);
            return;
        }
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            robots.append(parseRobot(v.toObject()));
        }
        callback(true, robots);
    });
}

ApiClient::MapInfo ApiClient::parseMap(const QJsonObject& o) {
    MapInfo m;
    m.id = o.value(QStringLiteral("id")).toInt();
    m.robotId = o.value(QStringLiteral("robot_id")).toInt();
    m.robotCode = o.value(QStringLiteral("robot_code")).toString();
    m.name = o.value(QStringLiteral("name")).toString();
    m.recipientName = o.value(QStringLiteral("recipient_name")).toString();
    m.isActive = o.value(QStringLiteral("is_active")).toBool();
    m.imageFileId = o.value(QStringLiteral("image_file_id")).toInt();
    m.imageUrl = o.value(QStringLiteral("image_url")).toString();
    m.resolution = o.value(QStringLiteral("resolution")).toDouble(0.0);
    m.originX = o.value(QStringLiteral("origin_x")).toDouble(0.0);
    m.originY = o.value(QStringLiteral("origin_y")).toDouble(0.0);
    return m;
}

void ApiClient::fetchActiveMaps(ListCallback<MapInfo> callback) {
    getJsonArray(QStringLiteral("/maps/active"), [callback](bool ok, const QByteArray& body) {
        QVector<MapInfo> maps;
        if (!ok) {
            callback(false, maps);
            return;
        }
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            maps.append(parseMap(v.toObject()));
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

void ApiClient::cancelSlamJob(int jobId, ItemCallback<SlamJobInfo> callback) {
    postJson(QStringLiteral("/slam-jobs/%1/cancel").arg(jobId), QJsonObject{},
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

void ApiClient::saveSlamJob(int jobId, const QString& mapName, ItemCallback<SlamJobInfo> callback) {
    QJsonObject body;
    if (!mapName.trimmed().isEmpty()) {
        body.insert(QStringLiteral("map_name"), mapName.trimmed());
    }
    postJson(QStringLiteral("/slam-jobs/%1/save").arg(jobId), body,
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

void ApiClient::uploadMapFile(const QString& robotCode, const QString& pgmFilePath, ItemCallback<int> callback) {
    auto* file = new QFile(pgmFilePath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        callback(false, 0, QStringLiteral("파일을 열 수 없습니다: %1").arg(pgmFilePath));
        return;
    }

    auto* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart robotCodePart;
    robotCodePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                             QVariant(QStringLiteral("form-data; name=\"robot_code\"")));
    robotCodePart.setBody(robotCode.toUtf8());
    multiPart->append(robotCodePart);

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QStringLiteral("application/octet-stream")));
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(QFileInfo(pgmFilePath).fileName())));
    file->setParent(multiPart);
    filePart.setBodyDevice(file);
    multiPart->append(filePart);

    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/media/upload-map")));
    QNetworkReply* reply = netManager_->post(request, multiPart);
    multiPart->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [reply, callback] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray respBody = reply->readAll();
        const bool ok = statusCode >= 200 && statusCode < 300;
        if (!ok) {
            const QJsonObject o = QJsonDocument::fromJson(respBody).object();
            const QString detail = o.contains(QStringLiteral("detail")) ? o.value(QStringLiteral("detail")).toString()
                                                                          : QString::fromUtf8(respBody);
            callback(false, 0, detail);
        } else {
            const QJsonObject o = QJsonDocument::fromJson(respBody).object();
            callback(true, o.value(QStringLiteral("file_id")).toInt(), QString());
        }
        reply->deleteLater();
    });
}

void ApiClient::createMap(const MapCreateParams& params, ItemCallback<MapInfo> callback) {
    const QJsonObject body{
        {QStringLiteral("robot_code"), params.robotCode},
        {QStringLiteral("map_name"), params.mapName},
        {QStringLiteral("file_id"), params.fileId},
        {QStringLiteral("resolution"), params.resolution},
        {QStringLiteral("origin_x"), params.originX},
        {QStringLiteral("origin_y"), params.originY},
        {QStringLiteral("origin_yaw"), params.originYaw},
        {QStringLiteral("width_px"), params.widthPx},
        {QStringLiteral("height_px"), params.heightPx},
        {QStringLiteral("raw_yaml"), params.rawYaml},
    };
    postJson(QStringLiteral("/maps"), body, [callback](bool ok, const QByteArray& respBody, const QString& err) {
        if (!ok) {
            callback(false, MapInfo{}, err);
            return;
        }
        callback(true, parseMap(QJsonDocument::fromJson(respBody).object()), err);
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

void ApiClient::startMedicationMission(const QString& robotCode, ItemCallback<MissionInfo> callback) {
    QJsonObject body{{"robot_code", robotCode}, {"kind", QStringLiteral("medication")}};
    postJson(QStringLiteral("/missions"), body,
             [callback](bool ok, const QByteArray& respBody, const QString& err) {
                 if (!ok) {
                     callback(false, MissionInfo{}, err);
                     return;
                 }
                 const QJsonObject o = QJsonDocument::fromJson(respBody).object();
                 MissionInfo mission;
                 mission.id = o.value(QStringLiteral("id")).toInt();
                 mission.status = o.value(QStringLiteral("status")).toString();
                 mission.robotConnected = o.value(QStringLiteral("robot_connected")).toBool();
                 callback(true, mission, err);
             });
}

ApiClient::RecipientInfo ApiClient::parseRecipient(const QJsonObject& o) {
    RecipientInfo r;
    r.id = o.value(QStringLiteral("id")).toInt();
    r.code = o.value(QStringLiteral("code")).toString();
    r.name = o.value(QStringLiteral("name")).toString();
    r.robotId = o.value(QStringLiteral("robot_id")).toInt();
    r.robotCode = o.value(QStringLiteral("robot_code")).toString();
    r.gender = o.value(QStringLiteral("gender")).toString();
    r.birthDate = o.value(QStringLiteral("birth_date")).toString();
    r.homeAddress = o.value(QStringLiteral("home_address")).toString();
    r.diseases = o.value(QStringLiteral("diseases")).toString();
    r.note = o.value(QStringLiteral("note")).toString();
    r.operatorId = o.value(QStringLiteral("operator_id")).toInt();
    r.operatorName = o.value(QStringLiteral("operator_name")).toString();
    r.level = o.value(QStringLiteral("level")).toString(QStringLiteral("normal"));
    r.statusSummary = o.value(QStringLiteral("status_summary")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("meds_today")).toArray()) {
        const QJsonObject m = v.toObject();
        MedicationTodayInfo info;
        info.slotNo = m.value(QStringLiteral("slot_no")).toInt();
        info.state = m.value(QStringLiteral("state")).toString();
        r.medsToday.append(info);
    }
    return r;
}

ApiClient::RecipientDetailInfo ApiClient::parseRecipientDetail(const QJsonObject& o) {
    RecipientDetailInfo d;
    d.recipient = parseRecipient(o);
    for (const QJsonValue& v : o.value(QStringLiteral("guardians")).toArray()) {
        const QJsonObject g = v.toObject();
        GuardianInfo info;
        info.id = g.value(QStringLiteral("id")).toInt();
        info.name = g.value(QStringLiteral("name")).toString();
        info.phone = g.value(QStringLiteral("phone")).toString();
        info.relation = g.value(QStringLiteral("relation")).toString();
        info.priority = g.value(QStringLiteral("priority")).toInt();
        info.notifyEnabled = g.value(QStringLiteral("notify_enabled")).toBool(true);
        d.guardians.append(info);
    }
    for (const QJsonValue& v : o.value(QStringLiteral("schedules")).toArray()) {
        const QJsonObject s = v.toObject();
        MedicationScheduleInfo info;
        info.id = s.value(QStringLiteral("id")).toInt();
        info.slotNo = s.value(QStringLiteral("slot_no")).toInt();
        info.drugName = s.value(QStringLiteral("drug_name")).toString();
        // 서버는 "HH:MM:SS"를 내려준다 — patrol-schedules와 동일하게 초 단위를 자른다.
        info.doseTime = s.value(QStringLiteral("dose_time")).toString().left(5);
        info.daysOfWeek = s.value(QStringLiteral("days_of_week")).toInt(127);
        d.schedules.append(info);
    }
    for (const QJsonValue& v : o.value(QStringLiteral("history")).toArray()) {
        const QJsonObject h = v.toObject();
        MedicationHistoryInfo info;
        info.date = h.value(QStringLiteral("date")).toString();
        info.adherence = h.value(QStringLiteral("adherence")).toDouble(-1.0);
        d.history.append(info);
    }
    return d;
}

void ApiClient::fetchRecipients(ListCallback<RecipientInfo> callback) {
    getJsonArray(QStringLiteral("/recipients"), [callback](bool ok, const QByteArray& body) {
        QVector<RecipientInfo> recipients;
        if (!ok) {
            callback(false, recipients);
            return;
        }
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            recipients.append(parseRecipient(v.toObject()));
        }
        callback(true, recipients);
    });
}

void ApiClient::fetchRecipientDetail(int recipientId, ItemCallback<RecipientDetailInfo> callback) {
    getJsonArray(QStringLiteral("/recipients/%1").arg(recipientId), [callback](bool ok, const QByteArray& body) {
        if (!ok) {
            callback(false, RecipientDetailInfo{}, QString());
            return;
        }
        callback(true, parseRecipientDetail(QJsonDocument::fromJson(body).object()), QString());
    });
}

namespace {
// server/app/schemas.py::RecipientCreate 대응 JSON — POST(등록)/PUT(수정) 둘 다 모양이
// 같아서 본문 구성만 공유한다.
QJsonObject recipientBodyFromParams(const ApiClient::RecipientCreateParams& params) {
    QJsonArray guardians;
    for (const auto& g : params.guardians) {
        guardians.append(QJsonObject{{"name", g.name},
                                      {"phone", g.phone},
                                      {"relation", g.relation},
                                      {"priority", g.priority},
                                      {"notify_enabled", g.notifyEnabled}});
    }
    QJsonArray schedules;
    for (const auto& s : params.schedules) {
        schedules.append(QJsonObject{{"slot_no", s.slotNo},
                                      {"drug_name", s.drugName},
                                      {"dose_time", s.doseTime},
                                      {"days_of_week", s.daysOfWeek}});
    }

    QJsonObject body{
        {"name", params.name},
        {"home_address", params.homeAddress},
        {"diseases", params.diseases},
        {"note", params.note},
        {"guardians", guardians},
        {"schedules", schedules},
    };
    if (!params.gender.isEmpty()) body.insert(QStringLiteral("gender"), params.gender);
    if (!params.birthDate.isEmpty()) body.insert(QStringLiteral("birth_date"), params.birthDate);
    if (!params.robotCode.isEmpty()) body.insert(QStringLiteral("robot_code"), params.robotCode);
    if (params.operatorId > 0) body.insert(QStringLiteral("operator_id"), params.operatorId);
    return body;
}
}  // namespace

void ApiClient::createRecipient(const RecipientCreateParams& params, ItemCallback<RecipientDetailInfo> callback) {
    postJson(QStringLiteral("/recipients"), recipientBodyFromParams(params),
             [callback](bool ok, const QByteArray& respBody, const QString& err) {
                 if (!ok) {
                     callback(false, RecipientDetailInfo{}, err);
                     return;
                 }
                 callback(true, parseRecipientDetail(QJsonDocument::fromJson(respBody).object()), err);
             });
}

void ApiClient::updateRecipient(int recipientId, const RecipientCreateParams& params,
                                 ItemCallback<RecipientDetailInfo> callback) {
    putJson(QStringLiteral("/recipients/%1").arg(recipientId), recipientBodyFromParams(params),
            [callback](bool ok, const QByteArray& respBody, const QString& err) {
                if (!ok) {
                    callback(false, RecipientDetailInfo{}, err);
                    return;
                }
                callback(true, parseRecipientDetail(QJsonDocument::fromJson(respBody).object()), err);
            });
}

void ApiClient::deactivateRecipient(int recipientId, SimpleCallback callback) {
    postJson(QStringLiteral("/recipients/%1/deactivate").arg(recipientId), QJsonObject{},
             [callback](bool ok, const QByteArray&, const QString& err) { callback(ok, err); });
}

void ApiClient::fetchOperators(ListCallback<OperatorInfo> callback) {
    getJsonArray(QStringLiteral("/accounts"), [callback](bool ok, const QByteArray& body) {
        QVector<OperatorInfo> operators;
        if (!ok) {
            callback(false, operators);
            return;
        }
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("role_code")).toString() != QStringLiteral("operator")) continue;
            if (!o.value(QStringLiteral("is_active")).toBool()) continue;
            OperatorInfo info;
            info.id = o.value(QStringLiteral("id")).toInt();
            info.name = o.value(QStringLiteral("display_name")).toString();
            operators.append(info);
        }
        callback(true, operators);
    });
}

void ApiClient::driveToPoint(const QString& robotCode, int mapId, const QString& code, double xMeters,
                              double yMeters, double yawRad, SimpleCallback callback) {
    QJsonObject body{{"robot_code", robotCode}, {"map_id", mapId},   {"code", code},
                      {"x", xMeters},           {"y", yMeters},     {"yaw", yawRad}};
    postJson(QStringLiteral("/patrol-runs/drive-to-point"), body,
             [callback](bool ok, const QByteArray&, const QString& err) { callback(ok, err); });
}

void ApiClient::prepareNavigation(const QString& robotCode, int mapId) {
    QJsonObject body{{"robot_code", robotCode}, {"map_id", mapId}};
    postJson(QStringLiteral("/patrol-runs/prepare-navigation"), body,
             [](bool, const QByteArray&, const QString&) {});
}

void ApiClient::setInitialPose(const QString& robotCode, int mapId, double xMeters, double yMeters, double yawRad,
                                SimpleCallback callback) {
    QJsonObject body{{"robot_code", robotCode}, {"map_id", mapId}, {"x", xMeters}, {"y", yMeters}, {"yaw", yawRad}};
    postJson(QStringLiteral("/patrol-runs/set-initial-pose"), body,
             [callback](bool ok, const QByteArray&, const QString& err) { callback(ok, err); });
}

void ApiClient::shutdownRobot(const QString& robotCode, SimpleCallback callback) {
    QJsonObject body{{"robot_code", robotCode}};
    postJson(QStringLiteral("/patrol-runs/shutdown-robot"), body,
             [callback](bool ok, const QByteArray&, const QString& err) { callback(ok, err); });
}

void ApiClient::sendTeleop(const QString& robotCode, double linearMps, double angularRadps) {
    // 방향키를 누르고 있는 동안 계속 호출되는 hot path라 응답을 기다리지 않는다 —
    // 실패해도(로봇 미연결 등) 조용히 무시한다. 위험 상황은 SlamMappingCard의
    // 비상정지가 별도로 처리한다.
    QJsonObject body{{"linear", linearMps}, {"angular", angularRadps}};
    postJson(QStringLiteral("/teleop/%1").arg(robotCode), body, [](bool, const QByteArray&, const QString&) {});
}
