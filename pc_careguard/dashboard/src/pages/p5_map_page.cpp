#include "p5_map_page.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <QJsonArray>
#include <QJsonObject>

#include "dialogs/p5_map_file_register_dialog.h"
#include "dialogs/p5_map_preview_dialog.h"
#include "net/api_client.h"
#include "net/ws_client.h"
#include "theme/tokens.h"
#include "widgets/careguard_combo_box.h"
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

    connectionBanner_ = buildConnectionBanner();
    layout->addWidget(connectionBanner_);

    robotOfflineBanner_ = buildRobotOfflineBanner();
    layout->addWidget(robotOfflineBanner_);

    statusOverlay_ = new MappingStatusOverlay(this);
    connect(statusOverlay_, &MappingStatusOverlay::mapPreviewRequested, this,
            &P5MapPage::openMapPreviewDialog);
    layout->addWidget(statusOverlay_);

    auto* content = new QHBoxLayout();
    content->setSpacing(Tokens::Layout::CARD_GAP);

    slamCard_ = new SlamMappingCard(this);
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
    loadRobotsFromServer();
}

void P5MapPage::setWsClient(WsClient* client) {
    wsClient_ = client;
    connect(wsClient_, &WsClient::messageReceived, this, &P5MapPage::handleWsMessage);
    connect(wsClient_, &WsClient::connectionStateChanged, this,
            [this](bool connected) { connectionBanner_->setVisible(!connected); });
}

QFrame* P5MapPage::buildRobotOfflineBanner() {
    // connectionBanner_(PC<->서버)와 별개 — 이건 로봇(downlink_node)<->서버 연결
    // 상태다. 카메라(uplink)는 계속 잘 보이는데 명령만 조용히 안 먹히는 상황이 실제로
    // 있었어서 level="emergency"(빨강)로 더 눈에 띄게 한다. 서버에서 아직 아무
    // robot_status도 못 받은 시작 시점엔 "안전하게" 오프라인으로 간주해 처음부터
    // 보이는 상태로 둔다(hide() 안 함) — handleWsMessage가 online:true를 받으면 숨긴다.
    auto* banner = new QFrame(this);
    banner->setObjectName("banner");
    banner->setProperty("level", "emergency");
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(10, 7, 10, 7);
    bannerLayout->setSpacing(6);
    auto* icon = new QLabel(banner);
    icon->setPixmap(SimpleIcons::alertCircle(Tokens::EMERGENCY.main, 12).pixmap(12, 12));
    bannerLayout->addWidget(icon);
    auto* text = new QLabel(
        QStringLiteral("로봇 오프라인 · 명령(텔레옵·저장 등)이 전달되지 않습니다"), banner);
    text->setStyleSheet(QStringLiteral("font-weight:500;"));
    bannerLayout->addWidget(text);
    bannerLayout->addStretch();
    return banner;
}

QFrame* P5MapPage::buildConnectionBanner() {
    // WsClient는 끊기면 내부적으로 3초 간격 재연결을 이미 하고 있었지만(체크리스트 ⑩),
    // 그 상태를 화면에 보여주는 곳이 어디에도 없어서 "화면이 멈췄는데 이유를 모르겠다"는
    // 증상으로만 보였다 — routeEditBanner_(P6)와 같은 QFrame#banner 패턴을 재사용한다.
    auto* banner = new QFrame(this);
    banner->setObjectName("banner");
    banner->setProperty("level", "attention");
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(10, 7, 10, 7);
    bannerLayout->setSpacing(6);
    auto* icon = new QLabel(banner);
    icon->setPixmap(SimpleIcons::plugOff(Tokens::ATTENTION.main, 12).pixmap(12, 12));
    bannerLayout->addWidget(icon);
    auto* text = new QLabel(QStringLiteral("서버 연결 끊김 · 재연결 시도 중…"), banner);
    text->setStyleSheet(QStringLiteral("font-weight:500;"));
    bannerLayout->addWidget(text);
    bannerLayout->addStretch();
    banner->hide();
    return banner;
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

    // 대상자 페이지 모달(P3RecipientCreateDialog)의 로봇/맵 배정 콤보와 같은
    // CareGuardComboBox — 예전엔 QPushButton+QMenu로 흉내 낸 "칩" 버튼이었다(디자인
    // 통일 요청, 2026-09-10). QSS의 QComboBox#robotChip 규칙이 이전 칩 버튼과 같은
    // 박스 크기·정렬을 유지한다.
    robotCombo_ = new CareGuardComboBox(row);
    robotCombo_->setObjectName("robotChip");
    // 기본값(AdjustToContentsOnFirstShow)은 폭을 "처음 표시될 때" 딱 한 번만 항목
    // 길이에 맞춰 계산하고 그 뒤로 안 바꾼다 — updateRobotChipText()가 나중에 현재
    // 항목 텍스트에 "· 온라인/오프라인"을 덧붙이면 이미 고정된(짧은) 폭보다 길어져
    // 말줄임표도 없이 그냥 잘려 보였다("R-04 (4호기) · 오프라인"이 "R-04"로만 보임).
    // 매번 현재 내용 기준으로 폭을 다시 맞추게 한다.
    robotCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(robotCombo_, &QComboBox::currentIndexChanged, this, &P5MapPage::switchToRobot);
    layout->addWidget(robotCombo_);

    auto* registerBtn = new QPushButton(SimpleIcons::upload(Tokens::TEXT_SECONDARY, 12), QStringLiteral("파일로 등록"), row);
    registerBtn->setProperty("variant", "secondary");
    registerBtn->setIconSize(QSize(12, 12));
    connect(registerBtn, &QPushButton::clicked, this, &P5MapPage::openFileRegisterDialog);
    layout->addWidget(registerBtn);

    startMappingBtn_ = new QPushButton(SimpleIcons::radar(Tokens::TEXT_INVERSE, 12),
                                        QStringLiteral("매핑 시작"), row);
    startMappingBtn_->setProperty("variant", "primary");
    startMappingBtn_->setIconSize(QSize(12, 12));
    connect(startMappingBtn_, &QPushButton::clicked, this, &P5MapPage::handleMappingToggleClicked);
    layout->addWidget(startMappingBtn_);

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
    mapNameEdit_ = new QLineEdit(QStringLiteral("동산주택 2층"), card);
    layout->addWidget(mapNameEdit_);
    layout->addSpacing(5);

    auto* recipientLabel = new QLabel(QStringLiteral("대상자 연결"), card);
    recipientLabel->setProperty("role", "secondary");
    layout->addWidget(recipientLabel);
    auto* recipientCombo = new QComboBox(card);
    recipientCombo->addItem(QStringLiteral("최금례 (C-004)"));
    layout->addWidget(recipientCombo);
    layout->addSpacing(8);

    // primary 버튼 배경(청록)과 대비되도록 아이콘을 흰색(TEXT_INVERSE)으로 그렸는데,
    // Normal 모드 픽스맵만 있으면 QStyle이 비활성 상태에서 이 흰 아이콘을 그대로
    // 흐리게(저채도) 렌더링한다 — 비활성 배경(#EDF1F4, 거의 흰색)과 대비가 없어
    // 아이콘이 안 보이는 원인이었다. QPushButton:disabled의 글자색(TEXT_MUTED)과
    // 같은 색으로 Disabled 모드 픽스맵을 따로 채워준다.
    QIcon saveMapIcon = SimpleIcons::deviceFloppy(Tokens::TEXT_INVERSE, 12);
    saveMapIcon.addPixmap(SimpleIcons::deviceFloppy(Tokens::TEXT_MUTED, 12).pixmap(12, 12), QIcon::Disabled);
    saveMapBtn_ = new QPushButton(saveMapIcon, QStringLiteral("SLAM 맵 저장 · 배포"), card);
    saveMapBtn_->setProperty("variant", "primary");
    saveMapBtn_->setIconSize(QSize(12, 12));
    // 좁은 우측 패널(196px) 폭에 기본 13px 글자로는 문구가 버튼 밖으로 잘려 보인다.
    saveMapBtn_->setStyleSheet(QStringLiteral("font-size:11px;"));
    // 매핑 중(currentSlamJobId_ != 0)이 아니면 저장할 대상 자체가 없다 — 1차
    // 프로젝트(MediRun) SlamMappingPage::m_saveButton과 같은 활성화 조건.
    // updateMappingButtonState()가 이후 상태 변화에 맞춰 이 값을 계속 갱신한다.
    saveMapBtn_->setEnabled(false);
    connect(saveMapBtn_, &QPushButton::clicked, this, [this] {
        // 예전엔 이 버튼이 실제로는 아무 것도 안 하고 "저장됐다"는 메시지만 띄웠다 —
        // 이제 진짜로 로봇에 저장 명령을 보낸다. 로봇이 map_saver_cli 실행+업로드를
        // 끝내는 데 시간이 걸리므로, 여기서는 "요청을 보냈다"까지만 알리고 실제 완료
        // 확인(및 매핑 현황 새로고침)은 handleWsMessage의 slam_done에서 처리한다.
        if (currentSlamJobId_ == 0) {
            QMessageBox::warning(this, QStringLiteral("저장 실패"),
                                  QStringLiteral("먼저 \"매핑 시작\"으로 SLAM 작업을 시작해야 합니다."));
            return;
        }
        // 이 입력칸의 이름이 실제 저장되는 맵 이름이 된다 — "매핑 시작"이 만든 임시
        // 이름("새 맵")을 여기서 덮어쓴다.
        apiClient_->saveSlamJob(currentSlamJobId_, mapNameEdit_->text(),
                                 [this](bool ok, const ApiClient::SlamJobInfo&, const QString& err) {
                                     if (!ok) {
                                         QMessageBox::warning(
                                             this, QStringLiteral("저장 요청 실패"),
                                             err.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다.") : err);
                                         return;
                                     }
                                     QMessageBox::information(
                                         this, QStringLiteral("저장 요청 전송됨"),
                                         QStringLiteral("로봇에 저장 명령을 보냈습니다. 완료되면 매핑 현황에 반영됩니다."));
                                 });
    });
    layout->addWidget(saveMapBtn_);
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
            [this, &dialog](const QImage& image, const QString& mapName, const QString& robotId,
                            const QString& recipientName) {
                // 예전엔 여기서 로컬 화면만 갱신하고 서버엔 아무 것도 안 보냈다 — "파일로
                // 등록"이 SLAM 저장·배포와 달리 앱을 재시작하면 사라지는 가짜 등록이었다.
                // 이제 실제로 업로드(POST /media/upload-map) -> 맵 확정(POST /maps)까지
                // 끝내야만 화면(SlamMapView·매핑 현황)을 갱신한다.
                const QString pgmPath = dialog.pgmFilePath();
                const double resolution = dialog.resolution();
                const double originX = dialog.originX();
                const double originY = dialog.originY();
                const double originYaw = dialog.originYaw();
                const QJsonObject rawYaml = dialog.rawYaml();

                apiClient_->uploadMapFile(
                    robotId, pgmPath,
                    [this, image, mapName, robotId, recipientName, resolution, originX, originY, originYaw,
                     rawYaml](bool uploadOk, int fileId, const QString& uploadErr) {
                        if (!uploadOk) {
                            QMessageBox::warning(this, QStringLiteral("맵 등록 실패"),
                                                  uploadErr.isEmpty() ? QStringLiteral("파일 업로드에 실패했습니다.")
                                                                       : uploadErr);
                            return;
                        }
                        ApiClient::MapCreateParams params;
                        params.robotCode = robotId;
                        params.mapName = mapName;
                        params.fileId = fileId;
                        params.resolution = resolution;
                        params.originX = originX;
                        params.originY = originY;
                        params.originYaw = originYaw;
                        params.widthPx = image.width();
                        params.heightPx = image.height();
                        params.rawYaml = rawYaml;
                        apiClient_->createMap(
                            params, [this, image, mapName, robotId, recipientName](
                                        bool createOk, const ApiClient::MapInfo& createdMap, const QString& createErr) {
                                if (!createOk) {
                                    QMessageBox::warning(this, QStringLiteral("맵 등록 실패"),
                                                          createErr.isEmpty()
                                                              ? QStringLiteral("서버에 맵을 저장하지 못했습니다.")
                                                              : createErr);
                                    return;
                                }
                                slamCard_->applyRegisteredMap(image, mapName, robotId);
                                statusOverlay_->markMapDeployed(robotId, mapName, recipientName);
                                registeredMaps_[robotId] =
                                    RegisteredMapInfo{image, mapName, recipientName, createdMap.id};
                                emit mapRegistered(image, mapName, robotId, recipientName);
                            });
                    });
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

void P5MapPage::refreshPatrolState() {
    if (currentRobotIndex_ < 0 || currentRobotIndex_ >= robots_.size()) return;
    const int robotId = robots_[currentRobotIndex_].id;
    const QString robotCode = currentRobotCode_;
    if (robotId <= 0) return;  // 서버 미등록(로컬 데모) 로봇 — 조회 대상 자체가 없다.

    apiClient_->fetchPatrolRuns(
        robotId, QStringLiteral("running"),
        [this, robotCode](bool ok, const QVector<ApiClient::PatrolRunInfo>& runs) {
            if (!ok) return;
            if (currentRobotCode_ != robotCode) return;  // 응답 오는 사이 다른 로봇으로 전환됨.
            robotPatrolling_ = !runs.isEmpty();
            teleopPanel_->setEnabled(!robotPatrolling_);
            teleopPanel_->setToolTip(
                robotPatrolling_
                    ? QStringLiteral("순찰/이동 중에는 원격 조작을 쓸 수 없습니다 — P6에서 먼저 멈춰주세요")
                    : QString());
        });
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

            // "이 로봇엔 등록된 맵이 있다"는 사실과 이름은 이미지 로딩(비동기, 아래)을
            // 기다리지 않고 여기서 바로 캐시에 채운다 — 예전엔 fetchBytes가 성공해야만
            // registeredMaps_에 들어가서, 이미지 로딩이 느리거나(전환 직후) 실패하면
            // (imageUrl이 비어있는 등) 실제로 맵이 있는 로봇도 "맵 없음"으로 계속 보이고
            // 상단 버튼도 "초기화" 대신 "매핑 시작"으로 잘못 나오는 원인이었다. 이미지는
            // 로딩되는 대로 같은 항목을 덮어써 채운다(비어있어도 이름·"초기화" 판정은
            // 이미 맞다).
            if (!registeredMaps_.contains(robotCode)) {
                registeredMaps_[robotCode] = RegisteredMapInfo{QImage(), mapName, recipientName, mapId};
            } else {
                registeredMaps_[robotCode].mapName = mapName;
                registeredMaps_[robotCode].recipientName = recipientName;
                registeredMaps_[robotCode].mapId = mapId;
            }
            if (robotCode == currentRobotCode_) {
                slamCard_->applyRegisteredMap(registeredMaps_[robotCode].image, mapName, robotCode);
                mapNameEdit_->setText(mapName);
                updateMappingButtonState();
            }

            // "미리보기" 모달(openMapPreviewDialog)이 쓰는 registeredMaps_의 실제 이미지를
            // 채운다 — 위에서 이미 이름·존재 여부는 반영했으니, 여기선 이미지만 마저 채운다.
            if (!m.imageUrl.isEmpty()) {
                apiClient_->fetchBytes(
                    m.imageUrl,
                    [this, robotCode, mapName, recipientName, mapId](bool imgOk, const QByteArray& data) {
                        if (!imgOk) return;
                        const QImage image = PgmImageLoader::loadFromData(data);
                        if (image.isNull()) return;
                        registeredMaps_[robotCode] = RegisteredMapInfo{image, mapName, recipientName, mapId};
                        // 지금 화면에 떠 있는 로봇의 맵이면 캔버스도 바로 갱신한다 — 이게
                        // 없으면 이미 배포된 맵이 있는 로봇으로 전환해도(또는 페이지를 막
                        // 열었을 때) 이 조회가 끝나기 전엔 캔버스가 빈 채로 남는다.
                        if (robotCode == currentRobotCode_) {
                            slamCard_->applyRegisteredMap(image, mapName, robotCode);
                            mapNameEdit_->setText(mapName);
                        }
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

void P5MapPage::loadRobotsFromServer() {
    apiClient_->fetchRobots([this](bool ok, const QVector<ApiClient::RobotInfo>& robots) {
        // 서버 미기동/robot 테이블이 비어 있으면 드롭다운은 빈 채로 둔다 — 이전
        // currentRobotCode_ 기본값("R-04")으로 계속 동작한다.
        if (!ok || robots.isEmpty()) return;
        robots_ = robots;
        rebuildRobotMenu();

        int initialIndex = 0;
        for (int i = 0; i < robots_.size(); ++i) {
            if (robots_[i].code == currentRobotCode_) {
                initialIndex = i;
                break;
            }
        }
        currentRobotIndex_ = -1;  // switchToRobot의 "이미 선택된 로봇" 얼리리턴을 피한다.
        switchToRobot(initialIndex);
    });
}

void P5MapPage::rebuildRobotMenu() {
    // addItem 하나하나가 currentIndexChanged를 튀길 수 있어(특히 첫 addItem이
    // index -1 -> 0로 넘어갈 때) 재구성 중엔 신호를 막는다 — switchToRobot()이
    // 엉뚱한 인덱스로 먼저 불리는 걸 막는다. 실제 전환은 이 함수 호출부
    // (loadRobotsFromServer)가 끝난 뒤 명시적으로 switchToRobot()을 부른다.
    const QSignalBlocker blocker(robotCombo_);
    robotCombo_->clear();
    const QIcon robotIcon = SimpleIcons::robot(Tokens::TEXT_SECONDARY, 13);
    for (const ApiClient::RobotInfo& r : robots_) {
        robotCombo_->addItem(robotIcon, QStringLiteral("%1 (%2)").arg(r.code, r.name));
    }
}

void P5MapPage::updateRobotChipText() {
    if (currentRobotIndex_ < 0 || currentRobotIndex_ >= robots_.size()) return;
    const ApiClient::RobotInfo& r = robots_[currentRobotIndex_];
    const QString status = robotOnline_ ? QStringLiteral("● 온라인") : QStringLiteral("○ 오프라인");
    // 텍스트만 바꾼다 — currentIndex는 그대로라 currentIndexChanged는 안 튄다.
    robotCombo_->setItemText(currentRobotIndex_, QStringLiteral("%1 (%2) · %3").arg(r.code, r.name, status));
}

void P5MapPage::switchToRobot(int index) {
    if (index < 0 || index >= robots_.size() || index == currentRobotIndex_) return;
    currentRobotIndex_ = index;
    currentRobotCode_ = robots_[index].code;
    // 사용자가 콤보에서 직접 골랐다면 이미 이 인덱스가 선택돼 있어 no-op이지만,
    // loadRobotsFromServer()의 초기 전환처럼 프로그램이 직접 부른 경우엔 콤보 쪽
    // 표시도 맞춰줘야 한다. setCurrentIndex 자체가 다시 currentIndexChanged를 튀길
    // 수 있어 블로킹한다(위 얼리리턴으로 재귀는 막히지만, 신호가 또 나가는 것 자체는
    // 막는 게 깔끔하다).
    {
        const QSignalBlocker blocker(robotCombo_);
        robotCombo_->setCurrentIndex(index);
    }

    // 이전 로봇의 SLAM 작업/온라인 상태를 새 로봇에 이어서 들고 있으면 안 된다 —
    // P6PatrolPage::loadMapData와 같은 이유(로봇을 바꿨는데 이전 로봇 기준 상태가
    // 남아있으면 안전 문제로 이어질 수 있다).
    currentSlamJobId_ = 0;
    updateMappingButtonState();
    robotOnline_ = false;
    robotOfflineBanner_->setVisible(true);
    updateRobotChipText();
    // 이전 로봇의 순찰 중 상태를 새 로봇에 이어서 들고 있으면 안 된다(위 SLAM/온라인
    // 상태 리셋과 같은 이유) — 응답이 올 때까지는 안전하게 "잠깐 열어둠"(로봇 전환
    // 직후 잘못 잠가서 조작을 막는 것보다 낫다, robotPatrolling_ 선언부 참고).
    robotPatrolling_ = false;
    teleopPanel_->setEnabled(true);
    teleopPanel_->setToolTip(QString());
    refreshPatrolState();
    // 카메라도 마찬가지 — 이전 로봇의 마지막 프레임이 남아있으면 새 로봇 화면인데
    // 정지된 이전 영상 + "연결 끊김" 배지가 뜬다(P6PatrolPage::loadMapData와 같은
    // 이유). 이 로봇의 camera_frame이 새로 도착하기 전까지는 "카메라 연결 중"
    // placeholder로 되돌려 둔다.
    slamCard_->view()->clearCameraFrame();

    if (registeredMaps_.contains(currentRobotCode_)) {
        const RegisteredMapInfo& info = registeredMaps_.value(currentRobotCode_);
        slamCard_->applyRegisteredMap(info.image, info.mapName, currentRobotCode_);
        mapNameEdit_->setText(info.mapName);
    } else {
        slamCard_->resetForRobot(currentRobotCode_);
        mapNameEdit_->setText(QStringLiteral("새 맵"));
    }
}

void P5MapPage::handleMappingToggleClicked() {
    // 1차 프로젝트(MediRun) SlamMappingPage::m_startButton과 같은 토글 —
    // "지울 기존 맵"이 없으면(진행 중인 slam_job도, 이 로봇에 이미 등록된 맵도
    // 없으면) 시작, 있으면 초기화. registeredMaps_에 이 로봇 항목이 있다는 것은
    // switchToRobot이 이미 지도를 그린 상태라는 뜻이라, 매핑을 새로 시작하기 전에
    // "초기화"로 한 번 더 확인시켜야 한다.
    const bool hasExistingMap = currentSlamJobId_ != 0 || registeredMaps_.contains(currentRobotCode_);
    if (!hasExistingMap) {
        // downlink_node._start_slam이 Nav2가 떠 있으면(이 로봇이 순찰 중이었어도)
        // 자동으로 먼저 종료시킨다(SLAM<->Nav2는 동시에 못 돎) — 사용자가 모르고
        // 눌렀다가 "순찰이 갑자기 끊겼다"로 보이지 않도록 먼저 알린다.
        const auto reply = QMessageBox::warning(
            this, QStringLiteral("SLAM 매핑 시작"),
            QStringLiteral("%1에서 진행 중인 주행(Nav2)이 있다면 SLAM 매핑을 시작하는 즉시 "
                            "자동으로 종료됩니다. 계속할까요?")
                .arg(currentRobotCode_),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        startMappingOnServer();
        return;
    }

    // 초기화는 되돌릴 수 없는 동작(화면의 지도·궤적·스캔점이 즉시 지워지고, 진행
    // 중이던 매핑도 서버에 취소 요청이 나간다)이라 반드시 먼저 확인받는다.
    const auto reply = QMessageBox::warning(
        this, QStringLiteral("매핑 초기화"),
        QStringLiteral("%1의 현재 지도가 지워지고 새로 그릴 준비 상태로 돌아갑니다. 수정하실건가요? 되돌릴 수 없습니다.")
            .arg(currentRobotCode_),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    if (currentSlamJobId_ != 0) {
        cancelMappingOnServer();
    }
    // 서버 쪽 map 행도 바로 지운다 — 예전엔 다음 저장(SLAM 맵 저장·배포)이 같은 이름을
    // 덮어쓸 때까지 남아있었다(2026-09-16 요청으로 즉시 삭제). 이 맵을 참조하던
    // waypoint·patrol_route(+schedule)도 서버가 같이 정리한다(map.py::delete_map).
    // mapId가 아직 0(서버 응답을 못 받은 채로 로컬에만 있던 경우)이면 지울 서버
    // 행이 없다는 뜻이라 건너뛴다.
    const int mapIdToDelete = registeredMaps_.value(currentRobotCode_).mapId;
    if (mapIdToDelete != 0) {
        apiClient_->deleteMap(mapIdToDelete, [this](bool ok, const QString& err) {
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("맵 삭제 실패"),
                                      err.isEmpty() ? QStringLiteral("서버에서 맵을 지우지 못했습니다.") : err);
            }
        });
    }
    // 로컬 캐시에서도 지워야 한다 — 안 지우면 registeredMaps_에 이 로봇 항목이 계속
    // 남아있어 hasExistingMap이 다음에도 계속 true로 판정돼(진짜 등록된 맵이 없어도)
    // "매핑 시작"으로 못 돌아간다. 서버 응답을 기다리지 않고 바로 지운다 — 위
    // cancelMappingOnServer와 같은 이유(화면은 즉시 "빈 상태"로 보여야 한다).
    registeredMaps_.remove(currentRobotCode_);
    // 화면에 그려진 지도·궤적·스캔점을 지우고 "이 로봇엔 아직 맵이 없다" 상태로
    // 되돌린다 — 1차 프로젝트의 handleResetMapping()이 stopMappingProcess() 직후
    // 화면을 즉시 비우는 것과 같은 동작. 서버 취소 응답을 기다리지 않고 바로
    // 처리한다(cancelMappingOnServer가 currentSlamJobId_를 이미 동기적으로
    // 0으로 되돌려두므로 updateMappingButtonState()도 바로 반영된다).
    slamCard_->resetForRobot(currentRobotCode_);
    mapNameEdit_->setText(QStringLiteral("새 맵"));
    updateMappingButtonState();
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
            currentSlamJobId_ = job.id;
            updateMappingButtonState();
            slamCard_->markMappingStarted();
            if (!job.robotConnected) {
                QMessageBox::information(
                    this, QStringLiteral("매핑 시작"),
                    QStringLiteral("SLAM 작업은 기록했지만 로봇이 아직 연결되어 있지 않습니다."));
            }
        });
}

void P5MapPage::cancelMappingOnServer() {
    if (currentSlamJobId_ == 0) return;  // 시작한 적 없으면 취소할 것도 없다.
    const int jobId = currentSlamJobId_;
    currentSlamJobId_ = 0;
    apiClient_->cancelSlamJob(jobId, [this](bool ok, const ApiClient::SlamJobInfo&, const QString& err) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("매핑 취소 실패"),
                                  err.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다.") : err);
        }
    });
}

void P5MapPage::updateMappingButtonState() {
    // handleMappingToggleClicked()와 같은 기준 — 진행 중인 slam_job이 있거나, 이
    // 로봇에 이미 등록된 맵이 있으면(registeredMaps_) 버튼은 "초기화"여야 한다.
    // 저장 버튼은 그중에서도 "지금 당장 저장할 진행 중인 job"이 있을 때만 켠다
    // (이미 등록된 맵을 다시 저장할 순 없다 — 새로 매핑해야 저장 가능).
    const bool hasExistingMap = currentSlamJobId_ != 0 || registeredMaps_.contains(currentRobotCode_);
    startMappingBtn_->setText(hasExistingMap ? QStringLiteral("초기화") : QStringLiteral("매핑 시작"));
    // "초기화"는 지금 그려진 지도를 지우는 되돌릴 수 없는 동작이라, "매핑 시작"과
    // 같은 primary(청록) 톤이면 위험성이 눈에 안 띈다 — danger(빨강)로 구분한다.
    startMappingBtn_->setProperty("variant", hasExistingMap ? "danger" : "primary");
    startMappingBtn_->style()->unpolish(startMappingBtn_);
    startMappingBtn_->style()->polish(startMappingBtn_);
    saveMapBtn_->setEnabled(currentSlamJobId_ != 0);
}

void P5MapPage::handleWsMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("map_changed")) {
        // 다른 대시보드가 "파일로 등록"으로 이 로봇의 맵을 새로 올렸을 수 있다 —
        // slam_done과 같은 이유로 GET /maps/active를 다시 받는다.
        loadActiveMapsFromServer();
        return;
    }

    if (type == QStringLiteral("patrol_run_updated") || type == QStringLiteral("patrol_done")) {
        // 이 로봇의 순찰/이동 시작·종료(스케줄 자동 트리거·다른 팀원 PC의 수동 시작·
        // ESP32 SOS 끼어듦/재개·취소 전부 포함) — 텔레옵 잠금을 즉시 다시 맞춘다
        // (P6PatrolPage::handleWsMessage의 같은 처리와 같은 이유, 요청 사양: 순찰 중엔
        // 원격 조작을 막아야 한다).
        if (message.value(QStringLiteral("robot_code")).toString() != currentRobotCode_) return;
        refreshPatrolState();
        return;
    }

    if (type == QStringLiteral("robot_recipient_changed") || type == QStringLiteral("robot_deregistered")) {
        // 담당 대상자 재배정·로봇 등록 해제 — 드롭다운의 "맵 · 대상자" 라벨이
        // 갱신되도록 로봇 목록을 다시 받는다.
        loadRobotsFromServer();
        return;
    }

    if (type == QStringLiteral("robot_status")) {
        // 로봇(downlink_node)이 서버와 실제로 붙어있는지 — connectionBanner_(PC<->서버)와
        // 별개다. 이게 없으면 와이파이가 불안정할 때 카메라(uplink)는 계속 잘 보이는데
        // teleop 명령은 조용히 무시되는 상황(실제로 겪음)에서 사용자가 끊긴 줄 모른 채
        // 화살표를 누르며 "왜 안 움직이지"만 반복하게 된다 — 안전 문제라 반드시 표시한다.
        if (message.value(QStringLiteral("robot_code")).toString() != currentRobotCode_) return;
        robotOnline_ = message.value(QStringLiteral("online")).toBool();
        robotOfflineBanner_->setVisible(!robotOnline_);
        updateRobotChipText();
        return;
    }

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

    if (type == QStringLiteral("camera_frame")) {
        // /camera/color/image_raw/compressed를 uplink_node가 재인코딩 없이 그대로
        // base64로 실어 보낸 것 — JPEG라서 QImage가 바로 디코딩한다.
        if (message.value(QStringLiteral("robot_code")).toString() != currentRobotCode_) return;
        const QByteArray jpegBytes =
            QByteArray::fromBase64(message.value(QStringLiteral("jpeg_base64")).toString().toLatin1());
        QImage frame;
        if (frame.loadFromData(jpegBytes)) {
            slamCard_->view()->setCameraFrame(frame);
        }
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
        // 로봇이 map_saver_cli 실행 + 업로드까지 끝내고 서버가 map 행을 새로 만들었다
        // (POST /slam-jobs/{id}/save로 시작된 저장의 실제 완료 신호). GET /maps/active를
        // 다시 불러 파일 등록(openFileRegisterDialog)과 동일한 경로로 화면에 반영한다.
        // 이미지 자체(pgm)는 여기서 새로 내려받지 않는다 — MappingStatusOverlay는 이름·
        // 대상자·상태만 갱신하고, 실제 이미지는 맵 미리보기를 열 때 필요하면 추가한다.
        const bool hasMapId = !message.value(QStringLiteral("map_id")).isNull();
        if (message.value(QStringLiteral("job_id")).toInt() == currentSlamJobId_) {
            currentSlamJobId_ = 0;
            updateMappingButtonState();
        }
        loadActiveMapsFromServer();
        if (hasMapId) {
            QMessageBox::information(this, QStringLiteral("저장 완료"),
                                      QStringLiteral("SLAM 맵이 저장되고 %1에 배포되었습니다.")
                                          .arg(currentRobotCode_));
        }
    } else {
        // slam_progress: 로봇의 slam_progress_publisher가 1Hz로 보낸 실제 경과시간·
        // occupancy grid 탐색면적. SlamMappingCard의 "경과 MM:SS · 면적 m²" 표시란에
        // 그대로 반영한다.
        slamCard_->applyProgress(message.value(QStringLiteral("elapsed_sec")).toInt(),
                                  message.value(QStringLiteral("area_m2")).toDouble());
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

void P5MapPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // setFocusPolicy(Qt::StrongFocus)만으로는 방향키가 안 온다 — 실제로 setFocus()를
    // 불러줘야 한다. QStackedWidget이 이 페이지로 전환할 때마다 showEvent가 다시 불리니
    // 여기서 매번 포커스를 가져온다("방향키 눌러도 안 움직여" 리포트 — P6PatrolPage도
    // 같은 문제/같은 수정).
    setFocus();
}
