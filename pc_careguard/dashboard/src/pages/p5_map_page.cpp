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

#include "dialogs/map_file_register_dialog.h"
#include "dialogs/map_preview_dialog.h"
#include "theme/tokens.h"
#include "widgets/mapping_status_overlay.h"
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
    rightCol->addWidget(teleopPanel_);

    rightCol->addWidget(buildSaveInfoCard());
    rightCol->addStretch();

    auto* rightWrap = new QWidget(this);
    rightWrap->setLayout(rightCol);
    rightWrap->setFixedWidth(196);
    content->addWidget(rightWrap, /*stretch=*/0);

    layout->addLayout(content, /*stretch=*/1);
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

void P5MapPage::handleEmergencyStop() {
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
