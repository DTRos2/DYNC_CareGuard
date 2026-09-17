#include "p5_map_file_register_dialog.h"

#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStyle>
#include <QTextStream>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/careguard_combo_box.h"
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
    // P5MapPage 헤더의 로봇 선택 콤보와 같은 CareGuardComboBox + objectName("robotChip")
    // — 모양과 항목 텍스트 형식("코드 (이름) · 온라인/오프라인")을 통일한다(디자인 통일
    // 요청, 2026-09-16). setRobots()가 exec() 전에 실제 GET /robots 결과로 채운다 —
    // 그 전까지는 비어있다.
    robotCombo_ = new CareGuardComboBox(this);
    robotCombo_->setObjectName("robotChip");
    connect(robotCombo_, &QComboBox::currentIndexChanged, this, &MapFileRegisterDialog::updateRobotSelectionUi);
    formCol->addWidget(robotCombo_);
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

    // setRobots()가 실제 로봇 목록으로 채우기 전까지의 자리표시자.
    footerStatus_ = new QLabel(QStringLiteral("로봇 목록을 불러오는 중…"), footer);
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
    // 비활성 상태 문구("... 업로드 후 활성화")가 길어 기본 13px/좌우 14px 패딩으로는
    // 버튼 폭(다이얼로그가 560px 고정이라 상태 문구·취소 버튼과 자리를 나눠 써야 한다)에
    // 안 들어가고 잘린다 — size="s"로 좌우 패딩도 10px로 줄이고, 글자도 더 줄인다.
    registerBtn_->setProperty("size", "s");
    registerBtn_->setStyleSheet(QStringLiteral("font-size:10px;"));
    connect(registerBtn_, &QPushButton::clicked, this, &MapFileRegisterDialog::handleRegisterClicked);
    layout->addWidget(registerBtn_);

    return footer;
}

QString MapFileRegisterDialog::defaultMapsDir() {
    return QProcessEnvironment::systemEnvironment().value(QStringLiteral("CAREGUARD_MAPS_DIR"),
                                                            QStringLiteral(CAREGUARD_MAPS_DIR));
}

void MapFileRegisterDialog::pickPgmFile() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral(".pgm 파일 선택"), defaultMapsDir(),
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
    pgmFilePath_ = path;

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
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral(".yaml 파일 선택"), defaultMapsDir(),
                                                        QStringLiteral("YAML (*.yaml *.yml)"));
    if (path.isEmpty()) return;

    // 예전엔 파일명만 확인하고 내용(resolution/origin)은 안 읽었다 — 그래서 이 값들
    // 없이는 서버에 실제로 저장할 수 없다는 걸 여기서 미리 걸러야 한다.
    if (!parseMapYaml(path)) {
        yamlLoaded_ = false;
        yamlSubLabel_->setText(QStringLiteral("읽기 실패 — resolution/origin이 있는 .yaml인지 확인하세요"));
        yamlSubLabel_->setStyleSheet(
            QStringLiteral("color:%1; font-size:11px;").arg(Tokens::EMERGENCY.main.name()));
        updateRegisterEnabled();
        return;
    }

    yamlLoaded_ = true;
    const QFileInfo info(path);
    yamlLabel_->setText(info.fileName());
    yamlSubLabel_->setText(QStringLiteral("업로드 완료"));
    yamlSubLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px;").arg(Tokens::OK.main.name()));
    yamlIcon_->setPixmap(SimpleIcons::fileCheck(Tokens::OK.main, 16).pixmap(16, 16));
    yamlDropzone_->setProperty("filled", true);
    yamlDropzone_->style()->unpolish(yamlDropzone_);
    yamlDropzone_->style()->polish(yamlDropzone_);
    warningBanner_->hide();
    updatePreview();
    updateRegisterEnabled();
}

bool MapFileRegisterDialog::parseMapYaml(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QJsonObject parsed;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;

        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) continue;
        const QString key = line.left(colon).trimmed();
        QString value = line.mid(colon + 1).trimmed();
        // 인라인 주석(#) 제거 — map_saver_cli 출력엔 없지만 사람이 손으로 고친
        // 파일에는 있을 수 있다.
        const int hashIdx = value.indexOf(QLatin1Char('#'));
        if (hashIdx >= 0) value = value.left(hashIdx).trimmed();
        if (key.isEmpty() || value.isEmpty()) continue;

        if (value.startsWith(QLatin1Char('[')) && value.endsWith(QLatin1Char(']'))) {
            QJsonArray arr;
            const QStringList parts = value.mid(1, value.length() - 2).split(QLatin1Char(','));
            for (const QString& part : parts) {
                bool ok = false;
                const double num = part.trimmed().toDouble(&ok);
                arr.append(ok ? QJsonValue(num) : QJsonValue(part.trimmed()));
            }
            parsed.insert(key, arr);
            continue;
        }
        bool ok = false;
        const double num = value.toDouble(&ok);
        if (ok) {
            parsed.insert(key, num);
        } else {
            if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')) && value.length() >= 2) {
                value = value.mid(1, value.length() - 2);
            }
            parsed.insert(key, value);
        }
    }

    if (!parsed.contains(QStringLiteral("resolution"))) return false;
    resolution_ = parsed.value(QStringLiteral("resolution")).toDouble();

    const QJsonArray origin = parsed.value(QStringLiteral("origin")).toArray();
    if (origin.size() < 2) return false;  // origin은 map_saver_cli 출력에 항상 있다 — 없으면 이상한 파일
    originX_ = origin.at(0).toDouble();
    originY_ = origin.at(1).toDouble();
    originYaw_ = origin.size() > 2 ? origin.at(2).toDouble() : 0.0;

    rawYaml_ = parsed;
    return true;
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

    const int robotIndex = robotCombo_->currentIndex();
    // 로봇 목록이 아직 안 왔거나 비어있으면(서버 미기동 등) 등록을 진행할 수 없다 —
    // updateRegisterEnabled()는 .pgm/.yaml 업로드 여부만 보고 이 경우를 안 걸러낸다.
    if (robotIndex < 0 || robotIndex >= robots_.size()) {
        QMessageBox::warning(this, QStringLiteral("맵 등록 실패"), QStringLiteral("로봇 목록을 불러오지 못했습니다."));
        return;
    }

    emit mapRegistered(pgmImage_, nameEdit_->text(), robots_[robotIndex].code, recipientName);
    accept();
}

QString MapFileRegisterDialog::formatRobotChipText(const ApiClient::RobotInfo& robot) const {
    const QString status = robot.isOnline ? QStringLiteral("● 온라인") : QStringLiteral("○ 오프라인");
    return QStringLiteral("%1 (%2) · %3").arg(robot.code, robot.name, status);
}

void MapFileRegisterDialog::setRobots(const QVector<ApiClient::RobotInfo>& robots, const QString& preselectedCode) {
    robots_ = robots;
    const QSignalBlocker blocker(robotCombo_);
    robotCombo_->clear();
    const QIcon robotIcon = SimpleIcons::robot(Tokens::TEXT_SECONDARY, 13);
    int preselectIndex = 0;
    for (int i = 0; i < robots_.size(); ++i) {
        robotCombo_->addItem(robotIcon, formatRobotChipText(robots_[i]));
        if (robots_[i].code == preselectedCode) preselectIndex = i;
    }
    if (!robots_.isEmpty()) {
        robotCombo_->setCurrentIndex(preselectIndex);
    }
    updateRobotSelectionUi();
}

void MapFileRegisterDialog::updateRobotSelectionUi() {
    const int index = robotCombo_->currentIndex();
    if (index < 0 || index >= robots_.size()) return;
    footerStatus_->setText(QStringLiteral("%1에 이미 할당된 맵 없음 · 매핑 가능").arg(robots_[index].code));
}
