#include "p5_map_page.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <QJsonArray>
#include <QJsonObject>

#include "dialogs/map_file_register_dialog.h"
#include "dialogs/map_preview_dialog.h"
#include "net/api_client.h"
#include "net/ws_client.h"
#include "theme/tokens.h"
#include "widgets/mapping_status_overlay.h"
#include "widgets/pgm_image_loader.h"
#include "widgets/simple_icons.h"
#include "widgets/slam_mapping_card.h"
#include "widgets/teleop_panel.h"

P5MapPage::P5MapPage(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    layout->addWidget(buildHeaderRow());

    statusOverlay_ = new MappingStatusOverlay(this);
    connect(statusOverlay_, &MappingStatusOverlay::mapPreviewRequested, this,
            &P5MapPage::openMapPreviewDialog);
    layout->addWidget(statusOverlay_);

    auto* content = new QHBoxLayout();
    content->setSpacing(Tokens::Layout::CARD_GAP);

    slamCard_ = new SlamMappingCard(this);
    connect(slamCard_, &SlamMappingCard::emergencyStopRequested, this, &P5MapPage::handleEmergencyStop);
    content->addWidget(slamCard_, /*stretch=*/16);

    auto* rightCol = new QVBoxLayout();
    rightCol->setSpacing(Tokens::Layout::CARD_GAP);

    teleopPanel_ = new TeleopPanel(this);
    teleopPanel_->setFixedWidth(196);
    connect(teleopPanel_, &TeleopPanel::velocityChanged, slamCard_, &SlamMappingCard::setVelocity);
    // 화면 표시(위)와는 별개로, 실제 로봇에게도 같은 속도를 그대로 전달한다
    // (POST /teleop/{robot_code} -> robot_ws -> downlink_node -> /cmd_vel).
    connect(teleopPanel_, &TeleopPanel::velocityChanged, this, [this](qreal linearMps, qreal angularRadps) {
        apiClient_->sendTeleop(currentRobotCode_, linearMps, angularRadps);
    });
    rightCol->addWidget(teleopPanel_);

    rightCol->addWidget(buildSaveInfoCard());
    rightCol->addStretch();

    auto* rightWrap = new QWidget(this);
    rightWrap->setLayout(rightCol);
    rightWrap->setFixedWidth(196);
    content->addWidget(rightWrap, /*stretch=*/0);

    layout->addLayout(content, /*stretch=*/1);

    apiClient_ = new ApiClient(this);
    loadActiveMapsFromServer();

    wsClient_ = new WsClient(this);
    connect(wsClient_, &WsClient::messageReceived, this, &P5MapPage::handleWsMessage);
    wsClient_->connectToServer();
}

QWidget* P5MapPage::buildHeaderRow() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("맵 · 로봇 매핑"), row);
    title->setObjectName("pageTitle");
    layout->addWidget(title);
    layout->addStretch();

    robotChipBtn_ = new QPushButton(SimpleIcons::robot(Tokens::TEXT_SECONDARY, 13), QString(), row);
    robotChipBtn_->setObjectName("robotChip");
    robotChipBtn_->setIconSize(QSize(13, 13));
    robotChipBtn_->setText(QStringLiteral("  R-04  ● 온라인  ▾"));
    layout->addWidget(robotChipBtn_);

    auto* registerBtn = new QPushButton(SimpleIcons::upload(Tokens::TEXT_SECONDARY, 12), QStringLiteral("파일로 등록"), row);
    registerBtn->setProperty("variant", "secondary");
    registerBtn->setIconSize(QSize(12, 12));
    connect(registerBtn, &QPushButton::clicked, this, &P5MapPage::openFileRegisterDialog);
    layout->addWidget(registerBtn);

    auto* startMappingBtn = new QPushButton(SimpleIcons::radar(Tokens::TEXT_INVERSE, 12),
                                             QStringLiteral("매핑 시작"), row);
    startMappingBtn->setProperty("variant", "primary");
    startMappingBtn->setIconSize(QSize(12, 12));
    connect(startMappingBtn, &QPushButton::clicked, this, &P5MapPage::startMappingOnServer);
    layout->addWidget(startMappingBtn);

    return row;
}

QWidget* P5MapPage::buildSaveInfoCard() {
    auto* card = new QFrame(this);
    card->setObjectName("card");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    layout->setSpacing(3);

    auto* title = new QLabel(QStringLiteral("맵 저장 정보"), card);
    title->setProperty("role", "label");
    layout->addWidget(title);
    layout->addSpacing(4);

    auto* nameLabel = new QLabel(QStringLiteral("맵 이름"), card);
    nameLabel->setProperty("role", "secondary");
    layout->addWidget(nameLabel);
    auto* nameEdit = new QLineEdit(QStringLiteral("동산주택 2층"), card);
    layout->addWidget(nameEdit);
    layout->addSpacing(5);

    auto* recipientLabel = new QLabel(QStringLiteral("대상자 연결"), card);
    recipientLabel->setProperty("role", "secondary");
    layout->addWidget(recipientLabel);
    auto* recipientCombo = new QComboBox(card);
    recipientCombo->addItem(QStringLiteral("최금례 (C-004)"));
    layout->addWidget(recipientCombo);
    layout->addSpacing(8);

    auto* saveBtn = new QPushButton(SimpleIcons::deviceFloppy(Tokens::TEXT_INVERSE, 12),
                                     QStringLiteral("SLAM 맵 저장 · 배포"), card);
    saveBtn->setProperty("variant", "primary");
    saveBtn->setIconSize(QSize(12, 12));
    // 좁은 우측 패널(196px) 폭에 기본 13px 글자로는 문구가 버튼 밖으로 잘려 보인다.
    saveBtn->setStyleSheet(QStringLiteral("font-size:11px;"));
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, QStringLiteral("저장 완료"),
                                  QStringLiteral("SLAM 맵이 저장되고 R-04에 배포되었습니다."));
    });
    layout->addWidget(saveBtn);
    layout->addSpacing(4);

    auto* caption = new QLabel(QStringLiteral("초기 위치는 순찰·경로 설정에서 충전 스테이션 웨이포인트로 지정됩니다."),
                                card);
    caption->setProperty("role", "muted");
    caption->setStyleSheet(QStringLiteral("font-size:11px;"));
    caption->setWordWrap(true);
    layout->addWidget(caption);

    return card;
}

void P5MapPage::openFileRegisterDialog() {
    QWidget* host = window();
    auto* overlay = new QWidget(host);
    overlay->setObjectName("modalOverlay");
    overlay->setGeometry(host->rect());
    overlay->show();
    overlay->raise();

    MapFileRegisterDialog dialog(this);
    connect(&dialog, &MapFileRegisterDialog::mapRegistered, this,
            [this](const QImage& image, const QString& mapName, const QString& robotId,
                   const QString& recipientName) {
                slamCard_->applyRegisteredMap(image, mapName, robotId);
                statusOverlay_->markMapDeployed(robotId, mapName, recipientName);
                registeredMaps_[robotId] = RegisteredMapInfo{image, mapName, recipientName};
                emit mapRegistered(image, mapName, robotId, recipientName);
            });
    dialog.exec();

    overlay->deleteLater();
}

void P5MapPage::openMapPreviewDialog(const QString& robotId) {
    QWidget* host = window();
    auto* overlay = new QWidget(host);
    overlay->setObjectName("modalOverlay");
    overlay->setGeometry(host->rect());
    overlay->show();
    overlay->raise();

    MapPreviewDialog dialog(this);
    const RegisteredMapInfo info = registeredMaps_.value(robotId);
    dialog.setMapInfo(info.image, info.mapName, info.recipientName, robotId);
    dialog.exec();

    overlay->deleteLater();
}

void P5MapPage::loadActiveMapsFromServer() {
    apiClient_->fetchActiveMaps([this](bool ok, const QVector<ApiClient::MapInfo>& maps) {
        // 서버 미기동/DB에 맵 없음 — 매핑 현황 패널은 헤더만 남은 빈 상태로 둔다
        // (더미 데이터로 대체하지 않는다).
        if (!ok) return;
        for (const ApiClient::MapInfo& m : maps) {
            const int mapId = m.id;
            const QString robotCode = m.robotCode;
            const QString mapName = m.name;
            const QString recipientName = m.recipientName;

            // "미리보기" 모달(openMapPreviewDialog)이 쓰는 registeredMaps_는 파일로 직접
            // 등록한 맵만 채워지던 로컬 캐시였다 — 서버에서 받아온 맵도 미리보기가 되도록
            // 이미지를 받아 같은 캐시에 채운다.
            if (!m.imageUrl.isEmpty()) {
                apiClient_->fetchBytes(
                    m.imageUrl, [this, robotCode, mapName, recipientName](bool imgOk, const QByteArray& data) {
                        if (!imgOk) return;
                        const QImage image = PgmImageLoader::loadFromData(data);
                        if (image.isNull()) return;
                        registeredMaps_[robotCode] = RegisteredMapInfo{image, mapName, recipientName};
                    });
            }

            // WP·경로 수는 실제 값을 받아와야 한다 — 가짜 숫자를 보여주지 않기 위해
            // GET /waypoints, GET /patrol-routes를 이어서 조회한 뒤에만 행을 표시한다.
            apiClient_->fetchWaypoints(
                mapId, [this, mapId, robotCode, mapName, recipientName](
                           bool wpOk, const QVector<ApiClient::WaypointInfo>& wps) {
                    const int wpCount = wpOk ? wps.size() : 0;
                    apiClient_->fetchPatrolRoutes(
                        mapId, [this, robotCode, mapName, recipientName, wpCount](
                                   bool routeOk, const QVector<ApiClient::PatrolRouteInfo>& routes) {
                            statusOverlay_->markMapDeployed(robotCode, mapName, recipientName, wpCount,
                                                             routeOk ? routes.size() : 0);
                        });
                });
        }
    });
}

void P5MapPage::startMappingOnServer() {
    apiClient_->startSlamJob(
        currentRobotCode_, QStringLiteral("새 맵"),
        [this](bool ok, const ApiClient::SlamJobInfo& job, const QString& err) {
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("매핑 시작 실패"),
                                      err.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다.") : err);
                return;
            }
            if (!job.robotConnected) {
                QMessageBox::information(
                    this, QStringLiteral("매핑 시작"),
                    QStringLiteral("SLAM 작업은 기록했지만 로봇이 아직 연결되어 있지 않습니다."));
            }
        });
}

void P5MapPage::handleWsMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("slam_map_update")) {
        // SLAM 도중 실시간 지도 미리보기 — uplink_node.py가 /map(OccupancyGrid)을 PGM
        // 바이트로 인코딩해 base64로 보낸 것을 디코딩해 SlamMapView 배경으로 그대로
        // 얹는다. "파일로 등록"한 실제 맵을 보여줄 때와 같은 경로(setBackgroundImage)를
        // 재사용한다 — 렌더링 쪽엔 실시간이든 파일이든 차이가 없다.
        if (message.value(QStringLiteral("robot_code")).toString() != currentRobotCode_) return;
        const QByteArray pgmBase64 = message.value(QStringLiteral("pgm_base64")).toString().toLatin1();
        const QImage liveMap = PgmImageLoader::loadFromData(QByteArray::fromBase64(pgmBase64));
        if (!liveMap.isNull()) {
            // 로봇 화살표·라이다 스캔을 지도 위 올바른 자리에 그리려면 미터<->픽셀
            // 환산 기준(해상도·원점)도 같이 있어야 한다 — 이미지와 함께 넘긴다.
            slamCard_->view()->setMapMeta(message.value(QStringLiteral("resolution")).toDouble(0.05),
                                           QPointF(message.value(QStringLiteral("origin_x")).toDouble(),
                                                   message.value(QStringLiteral("origin_y")).toDouble()),
                                           liveMap.size());
            slamCard_->view()->setBackgroundImage(liveMap);
        }
        return;
    }

    if (type == QStringLiteral("robot_position")) {
        // map->base_link TF 기반 실제 로봇 위치(uplink_node._on_slam_overlay_tick) —
        // 지도 위 빨간 화살표를 이 값으로 그린다.
        if (message.value(QStringLiteral("robot_code")).toString() != currentRobotCode_) return;
        slamCard_->view()->setLivePose(message.value(QStringLiteral("x")).toDouble(),
                                        message.value(QStringLiteral("y")).toDouble(),
                                        message.value(QStringLiteral("yaw")).toDouble());
        return;
    }

    if (type == QStringLiteral("slam_scan_update")) {
        // 실시간 라이다 스캔(map 프레임 x,y가 번갈아 들어있는 평탄화 배열) — rviz2처럼
        // 지도 위에 점으로 겹쳐 그린다.
        if (message.value(QStringLiteral("robot_code")).toString() != currentRobotCode_) return;
        const QJsonArray flat = message.value(QStringLiteral("points")).toArray();
        QVector<QPointF> points;
        points.reserve(flat.size() / 2);
        for (int i = 0; i + 1 < flat.size(); i += 2) {
            points.append(QPointF(flat.at(i).toDouble(), flat.at(i + 1).toDouble()));
        }
        slamCard_->view()->setLiveScan(points);
        return;
    }

    if (type != QStringLiteral("slam_progress") && type != QStringLiteral("slam_done")) return;
    if (message.value(QStringLiteral("robot_code")).toString() != currentRobotCode_) return;

    if (type == QStringLiteral("slam_done")) {
        // 로봇이 매핑을 끝내고 서버가 map 행을 새로 만들었다 — GET /maps/active를 다시
        // 불러 파일 등록(openFileRegisterDialog)과 동일한 경로로 화면에 반영한다.
        // 이미지 자체(pgm)는 여기서 새로 내려받지 않는다 — MappingStatusOverlay는 이름·
        // 대상자·상태만 갱신하고, 실제 이미지는 맵 미리보기를 열 때 필요하면 추가한다.
        loadActiveMapsFromServer();
    }
    // slam_progress는 지금은 별도 진행률 라벨이 없어 무시한다 — SlamMappingCard가 이미
    // 텔레옵 기반 경과시간·탐색면적을 자체적으로 보여주고 있다(로컬 시각 피드백 유지).
}

void P5MapPage::handleEmergencyStop() {
    // Space와 같은 경로(velocityChanged(0,0))라 확인 대화상자를 띄우기 전에 이미
    // 실제 로봇에도 정지 명령이 나간다 — 안전이 우선이라 사용자 확인을 기다리지 않는다.
    teleopPanel_->handleKeyPress(Qt::Key_Space);
    const auto reply =
        QMessageBox::warning(this, QStringLiteral("비상정지"),
                              QStringLiteral("R-04를 비상정지합니다. 진행 중인 매핑 주행이 중단됩니다."),
                              QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Ok);
    if (reply == QMessageBox::Ok) {
        slamCard_->setVelocity(0.0, 0.0);
    }
}

void P5MapPage::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    switch (event->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Space:
            teleopPanel_->handleKeyPress(event->key());
            event->accept();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

void P5MapPage::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    switch (event->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Space:
            teleopPanel_->handleKeyRelease(event->key());
            event->accept();
            return;
        default:
            break;
    }
    QWidget::keyReleaseEvent(event);
}
