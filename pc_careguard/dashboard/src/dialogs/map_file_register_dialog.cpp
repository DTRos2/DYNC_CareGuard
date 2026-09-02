#include "map_file_register_dialog.h"

#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/pgm_image_loader.h"
#include "widgets/simple_icons.h"

MapFileRegisterDialog::MapFileRegisterDialog(QWidget* parent)
    : QDialog(parent), pgmLoaded_(false), yamlLoaded_(false) {
    setWindowTitle(QStringLiteral("파일로 맵 등록"));
    setModal(true);
    setFixedWidth(Tokens::Layout::MODAL_W_S + 80);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());

    auto* body = new QVBoxLayout();
    body->setContentsMargins(16, 14, 16, 14);
    body->setSpacing(6);

    auto* fileLabel = new QLabel(QStringLiteral("맵 파일 — .pgm + .yaml 한 쌍 필수"), this);
    fileLabel->setProperty("role", "label");
    body->addWidget(fileLabel);

    auto* filesRow = new QHBoxLayout();
    filesRow->setSpacing(8);

    pgmDropzone_ = new QFrame(this);
    pgmDropzone_->setObjectName("dropzoneCard");
    pgmDropzone_->setCursor(Qt::PointingHandCursor);
    auto* pgmLayout = new QHBoxLayout(pgmDropzone_);
    pgmLayout->setContentsMargins(10, 8, 10, 8);
    pgmLayout->setSpacing(8);
    pgmIcon_ = new QLabel(pgmDropzone_);
    pgmIcon_->setPixmap(SimpleIcons::fileUpload(Tokens::TEXT_MUTED, 16).pixmap(16, 16));
    pgmLayout->addWidget(pgmIcon_);
    auto* pgmTextCol = new QVBoxLayout();
    pgmTextCol->setSpacing(1);
    pgmLabel_ = new QLabel(QStringLiteral(".pgm 파일"), pgmDropzone_);
    pgmLabel_->setProperty("role", "secondary");
    pgmTextCol->addWidget(pgmLabel_);
    pgmSubLabel_ = new QLabel(QStringLiteral("클릭 또는 드래그로 업로드"), pgmDropzone_);
    pgmSubLabel_->setProperty("role", "muted");
    pgmSubLabel_->setStyleSheet(QStringLiteral("font-size:11px;"));
    pgmTextCol->addWidget(pgmSubLabel_);
    pgmLayout->addLayout(pgmTextCol, 1);
    filesRow->addWidget(pgmDropzone_, 1);

    yamlDropzone_ = new QFrame(this);
    yamlDropzone_->setObjectName("dropzoneCard");
    yamlDropzone_->setCursor(Qt::PointingHandCursor);
    auto* yamlLayout = new QHBoxLayout(yamlDropzone_);
    yamlLayout->setContentsMargins(10, 8, 10, 8);
    yamlLayout->setSpacing(8);
    yamlIcon_ = new QLabel(yamlDropzone_);
    yamlIcon_->setPixmap(SimpleIcons::fileUpload(Tokens::TEXT_MUTED, 16).pixmap(16, 16));
    yamlLayout->addWidget(yamlIcon_);
    auto* yamlTextCol = new QVBoxLayout();
    yamlTextCol->setSpacing(1);
    yamlLabel_ = new QLabel(QStringLiteral(".yaml 파일"), yamlDropzone_);
    yamlLabel_->setProperty("role", "secondary");
    yamlTextCol->addWidget(yamlLabel_);
    yamlSubLabel_ = new QLabel(QStringLiteral("클릭 또는 드래그로 업로드"), yamlDropzone_);
    yamlSubLabel_->setProperty("role", "muted");
    yamlSubLabel_->setStyleSheet(QStringLiteral("font-size:11px;"));
    yamlTextCol->addWidget(yamlSubLabel_);
    yamlLayout->addLayout(yamlTextCol, 1);
    filesRow->addWidget(yamlDropzone_, 1);
    body->addLayout(filesRow);

    pgmDropzone_->installEventFilter(this);
    yamlDropzone_->installEventFilter(this);

    warningBanner_ = new QFrame(this);
    warningBanner_->setObjectName("banner");
    warningBanner_->setProperty("level", "attention");
    auto* warnLayout = new QHBoxLayout(warningBanner_);
    warnLayout->setContentsMargins(8, 6, 8, 6);
    warnLayout->setSpacing(6);
    auto* warnIcon = new QLabel(warningBanner_);
    warnIcon->setPixmap(SimpleIcons::alertTriangle(Tokens::ATTENTION.main, 12).pixmap(12, 12));
    warnLayout->addWidget(warnIcon);
    auto* warnText = new QLabel(QStringLiteral(".yaml 파일이 없으면 해상도·원점 정보를 읽을 수 없어 등록이 불가합니다"),
                                 warningBanner_);
    warnText->setWordWrap(true);
    warnLayout->addWidget(warnText, 1);
    body->addWidget(warningBanner_);

    auto* previewFormRow = new QHBoxLayout();
    previewFormRow->setSpacing(12);

    auto* previewCol = new QVBoxLayout();
    auto* previewLabel = new QLabel(QStringLiteral("미리보기"), this);
    previewLabel->setProperty("role", "label");
    previewCol->addWidget(previewLabel);
    auto* previewBox = new QFrame(this);
    previewBox->setObjectName("previewBox");
    previewBox->setFixedHeight(120);
    auto* previewBoxLayout = new QVBoxLayout(previewBox);
    previewBoxLayout->setContentsMargins(6, 6, 6, 6);
    previewHint_ = new QLabel(QStringLiteral("맵 파일(.pgm)을 업로드하면 미리보기가 표시됩니다"), previewBox);
    previewHint_->setProperty("role", "muted");
    previewHint_->setAlignment(Qt::AlignCenter);
    previewHint_->setWordWrap(true);
    previewBoxLayout->addWidget(previewHint_);
    previewImage_ = new QLabel(previewBox);
    previewImage_->setAlignment(Qt::AlignCenter);
    previewImage_->hide();
    previewBoxLayout->addWidget(previewImage_);
    previewCol->addWidget(previewBox);
    previewFormRow->addLayout(previewCol, 1);

    auto* formCol = new QVBoxLayout();
    formCol->setSpacing(3);
    auto* nameLabel = new QLabel(QStringLiteral("맵 이름"), this);
    nameLabel->setProperty("role", "secondary");
    formCol->addWidget(nameLabel);
    nameEdit_ = new QLineEdit(QStringLiteral("동산주택 2층"), this);
    formCol->addWidget(nameEdit_);
    formCol->addSpacing(5);

    auto* recipientLabel = new QLabel(QStringLiteral("대상자 연결"), this);
    recipientLabel->setProperty("role", "secondary");
    formCol->addWidget(recipientLabel);
    recipientCombo_ = new QComboBox(this);
    recipientCombo_->addItems({QStringLiteral("최금례 (C-004)"), QStringLiteral("김순자 (C-001)"),
                                QStringLiteral("박영호 (C-002)"), QStringLiteral("이말순 (C-003)")});
    formCol->addWidget(recipientCombo_);
    formCol->addSpacing(5);

    auto* robotLabel = new QLabel(QStringLiteral("로봇 매핑 — 1맵 : 1로봇"), this);
    robotLabel->setProperty("role", "secondary");
    formCol->addWidget(robotLabel);
    auto* robotRow = new QFrame(this);
    robotRow->setStyleSheet(
        QStringLiteral("border:1px solid #CDD5DD; border-radius:6px; background:#FFFFFF;"));
    auto* robotRowLayout = new QHBoxLayout(robotRow);
    robotRowLayout->setContentsMargins(8, 4, 8, 4);
    robotRowLayout->setSpacing(5);
    auto* robotIcon = new QLabel(robotRow);
    robotIcon->setPixmap(SimpleIcons::robot(Tokens::TEXT_SECONDARY, 13).pixmap(13, 13));
    robotRowLayout->addWidget(robotIcon);
    auto* robotIdLabel = new QLabel(QStringLiteral("R-04"), robotRow);
    robotIdLabel->setProperty("role", "mono");
    robotRowLayout->addWidget(robotIdLabel);
    auto* robotOnlineLabel = new QLabel(QStringLiteral("● 온라인"), robotRow);
    robotOnlineLabel->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(Tokens::OK.main.name()));
    robotRowLayout->addWidget(robotOnlineLabel);
    robotRowLayout->addStretch();
    formCol->addWidget(robotRow);
    previewFormRow->addLayout(formCol, 1);

    body->addLayout(previewFormRow);

    auto* infoBanner = new QFrame(this);
    infoBanner->setObjectName("banner");
    infoBanner->setProperty("level", "info");
    auto* infoLayout = new QHBoxLayout(infoBanner);
    infoLayout->setContentsMargins(9, 7, 9, 7);
    infoLayout->setSpacing(6);
    auto* infoIcon = new QLabel(infoBanner);
    infoIcon->setPixmap(SimpleIcons::infoCircle(QColor("#0B7A6E"), 12).pixmap(12, 12));
    infoLayout->addWidget(infoIcon);
    auto* infoText = new QLabel(
        QStringLiteral("등록 후 순찰·경로 설정에서 웨이포인트를 지정하세요. 충전 스테이션 웨이포인트가 초기 위치로 사용됩니다."),
        infoBanner);
    infoText->setWordWrap(true);
    infoLayout->addWidget(infoText, 1);
    body->addWidget(infoBanner);

    root->addLayout(body);
    root->addWidget(buildFooter());

    updateRegisterEnabled();
}

bool MapFileRegisterDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (watched == pgmDropzone_) {
            pickPgmFile();
            return true;
        }
        if (watched == yamlDropzone_) {
            pickYamlFile();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

QFrame* MapFileRegisterDialog::buildHeader() {
    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    auto* icon = new QLabel(header);
    icon->setPixmap(SimpleIcons::upload(Tokens::ACCENT, 15).pixmap(15, 15));
    layout->addWidget(icon);

    auto* title = new QLabel(QStringLiteral("파일로 맵 등록"), header);
    title->setStyleSheet(QStringLiteral("font-size:14px; font-weight:500; color:#16202B;"));
    layout->addWidget(title);

    auto* subtitle = new QLabel(QStringLiteral("— 기존 SLAM 결과물 업로드"), header);
    subtitle->setProperty("role", "muted");
    subtitle->setStyleSheet(QStringLiteral("font-size:11px;"));
    layout->addWidget(subtitle);
    layout->addStretch();

    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_SECONDARY, 16), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(closeBtn);

    return header;
}

QFrame* MapFileRegisterDialog::buildFooter() {
    auto* footer = new QFrame(this);
    footer->setObjectName("modalFooter");
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    footerStatus_ = new QLabel(QStringLiteral("R-04에 이미 할당된 맵 없음 · 매핑 가능"), footer);
    footerStatus_->setProperty("role", "muted");
    footerStatus_->setStyleSheet(QStringLiteral("font-size:11px;"));
    layout->addWidget(footerStatus_);
    layout->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("취소"), footer);
    cancelBtn->setProperty("variant", "secondary");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancelBtn);

    registerBtn_ = new QPushButton(QStringLiteral("등록 · 배포 (.pgm + .yaml 업로드 후 활성화)"), footer);
    registerBtn_->setProperty("variant", "primary");
    connect(registerBtn_, &QPushButton::clicked, this, &MapFileRegisterDialog::handleRegisterClicked);
    layout->addWidget(registerBtn_);

    return footer;
}

void MapFileRegisterDialog::pickPgmFile() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral(".pgm 파일 선택"), QString(),
                                                        QStringLiteral("PGM (*.pgm)"));
    if (path.isEmpty()) return;

    const QImage image = PgmImageLoader::load(path);
    if (image.isNull()) {
        pgmSubLabel_->setText(QStringLiteral("읽기 실패 — 올바른 .pgm 파일인지 확인하세요"));
        pgmSubLabel_->setStyleSheet(
            QStringLiteral("color:%1; font-size:11px;").arg(Tokens::EMERGENCY.main.name()));
        return;
    }
    pgmImage_ = image;

    pgmLoaded_ = true;
    const QFileInfo info(path);
    pgmLabel_->setText(info.fileName());
    pgmSubLabel_->setText(QStringLiteral("업로드 완료 · %1×%2 px").arg(image.width()).arg(image.height()));
    pgmSubLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px;").arg(Tokens::OK.main.name()));
    pgmIcon_->setPixmap(SimpleIcons::fileCheck(Tokens::OK.main, 16).pixmap(16, 16));
    pgmDropzone_->setProperty("filled", true);
    pgmDropzone_->style()->unpolish(pgmDropzone_);
    pgmDropzone_->style()->polish(pgmDropzone_);
    updatePreview();
    updateRegisterEnabled();
}

void MapFileRegisterDialog::pickYamlFile() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral(".yaml 파일 선택"), QString(),
                                                        QStringLiteral("YAML (*.yaml *.yml)"));
    if (path.isEmpty()) return;

    yamlLoaded_ = true;
    const QFileInfo info(path);
    yamlLabel_->setText(info.fileName());
    yamlSubLabel_->setText(QStringLiteral("업로드 완료"));
    yamlIcon_->setPixmap(SimpleIcons::fileCheck(Tokens::OK.main, 16).pixmap(16, 16));
    yamlDropzone_->setProperty("filled", true);
    yamlDropzone_->style()->unpolish(yamlDropzone_);
    yamlDropzone_->style()->polish(yamlDropzone_);
    warningBanner_->hide();
    updatePreview();
    updateRegisterEnabled();
}

void MapFileRegisterDialog::updatePreview() {
    if (pgmImage_.isNull()) {
        previewHint_->show();
        previewImage_->hide();
        return;
    }

    previewHint_->hide();
    previewImage_->show();
    const QPixmap scaled =
        QPixmap::fromImage(pgmImage_).scaled(previewImage_->parentWidget()->size() - QSize(12, 12),
                                              Qt::KeepAspectRatio, Qt::SmoothTransformation);
    previewImage_->setPixmap(scaled);
}

void MapFileRegisterDialog::updateRegisterEnabled() {
    const bool ready = pgmLoaded_ && yamlLoaded_;
    registerBtn_->setEnabled(ready);
    registerBtn_->setText(ready ? QStringLiteral("등록 · 배포")
                                 : QStringLiteral("등록 · 배포 (.pgm + .yaml 업로드 후 활성화)"));
}

void MapFileRegisterDialog::handleRegisterClicked() {
    // 콤보 표기 "최금례 (C-004)"에서 이름만 뽑아 전달한다.
    const QString recipientText = recipientCombo_->currentText();
    const int parenIdx = recipientText.indexOf(QLatin1Char('('));
    const QString recipientName =
        (parenIdx > 0 ? recipientText.left(parenIdx).trimmed() : recipientText.trimmed());

    emit mapRegistered(pgmImage_, nameEdit_->text(), QStringLiteral("R-04"), recipientName);
    accept();
}
