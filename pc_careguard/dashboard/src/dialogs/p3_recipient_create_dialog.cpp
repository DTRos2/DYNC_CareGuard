#include "p3_recipient_create_dialog.h"

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QIntValidator>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/careguard_combo_box.h"
#include "widgets/simple_icons.h"

namespace {

// p3_recipient_detail_dialog.cpp의 동명 헬퍼와 같은 모양이다 — 이 코드베이스는 페이지/모달별
// 로컬 헬퍼 중복을 관례로 둔다(공용 유틸로 뽑지 않음).
QFrame* makeBoxFrame(QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setObjectName("p3CreateBox");
    frame->setStyleSheet(QStringLiteral("QFrame#p3CreateBox{border:0.5px solid %1; border-radius:8px;}")
                              .arg(Tokens::BORDER.name()));
    return frame;
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", "label");
    return label;
}

QWidget* makeFieldBlock(const QString& label, bool required, QWidget* editWidget, QWidget* parent) {
    auto* wrap = new QWidget(parent);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    const QString text = required
                              ? QStringLiteral("%1 <span style='color:%2'>*</span>").arg(label, Tokens::EMERGENCY.main.name())
                              : label;
    auto* labelWidget = new QLabel(text, wrap);
    labelWidget->setTextFormat(Qt::RichText);
    labelWidget->setProperty("role", "label");
    layout->addWidget(labelWidget);
    layout->addWidget(editWidget);
    return wrap;
}

QLineEdit* makeReadOnlyField(const QString& text, QWidget* parent) {
    auto* edit = new QLineEdit(text, parent);
    edit->setReadOnly(true);
    edit->setStyleSheet(QStringLiteral("background:%1; color:%2;")
                             .arg(Tokens::BG_SUNKEN.name(), Tokens::TEXT_MUTED.name()));
    return edit;
}

QLabel* makePriorityPill(int priority, bool emergencyFixed, QWidget* parent) {
    auto* pill = new QLabel(parent);
    pill->setAlignment(Qt::AlignCenter);
    if (emergencyFixed) {
        pill->setText(QStringLiteral("긴급"));
        pill->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:4px; "
                                            "font-weight:500; padding:1px 6px;")
                                 .arg(Tokens::EMERGENCY.soft.name(), Tokens::EMERGENCY.main.name()));
    } else if (priority == 1) {
        pill->setText(QStringLiteral("1순위"));
        pill->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:4px; "
                                            "font-weight:500; padding:1px 6px;")
                                 .arg(Tokens::ACCENT_SOFT.name(), Tokens::ACCENT_HOVER.name()));
    } else {
        pill->setText(QStringLiteral("%1순위").arg(priority));
        pill->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:4px; padding:1px 6px;")
                                 .arg(Tokens::BG_SUNKEN.name(), Tokens::TEXT_SECONDARY.name()));
    }
    return pill;
}

// 로봇 하나당 맵 하나가 고정으로 딸린 이 앱의 기존 mock 관례(R-01~04 ↔ 4개 주소) — 별도
// 로봇/맵 카탈로그가 없어 p6_patrol_page.cpp/mapping_status_overlay.cpp와 같은 값을 쓴다.
const QStringList kRobotCodes = {QStringLiteral("R-01"), QStringLiteral("R-02"), QStringLiteral("R-03"),
                                  QStringLiteral("R-04")};
const QStringList kRobotMaps = {QStringLiteral("행복빌라 302"), QStringLiteral("은빛아파트 101"),
                                 QStringLiteral("솔마을 7호"), QStringLiteral("동산주택 2층")};

}  // namespace

P3RecipientCreateDialog::P3RecipientCreateDialog(const QString& nextCode, const QVector<OperatorOption>& operators,
                                                   QWidget* parent)
    : QDialog(parent), nextCode_(nextCode) {
    setModal(true);
    // OS 창관리자 타이틀바를 없애 커스텀 헤더(제목·X)만 남긴다 — 안 그러면 X가 두 개로 보임
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setFixedWidth(620);
    setWindowTitle(QStringLiteral("대상자 등록"));

    // 한글 IME 조합 문자가 다른 칸으로 새는 문제의 방어 코드 — 자세한 원인과 시행착오는
    // eventFilter() 주석 참고.
    qApp->installEventFilter(this);

    // 항상 있는 긴급 고정 행 — p3_recipient_page.cpp mockRecipientDetail()의 kEmergencyRow와
    // 같은 값(별도 export 없이 그대로 복제 — 위 헬퍼들과 같은 이유).
    detail_.guardians = {
        GuardianContact{0, QStringLiteral("119 · 112"), QString(), QStringLiteral("—"), true, true}};

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 헤더 ----
    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 0, 16, 0);
    headerLayout->setSpacing(8);
    headerLayout->setAlignment(Qt::AlignVCenter);
    header->setFixedHeight(46);

    auto* titleLabel = new QLabel(QStringLiteral("대상자 등록"), header);
    QFont titleFont = titleLabel->font();
    titleFont.setPixelSize(14);
    titleFont.setWeight(QFont::Medium);
    titleLabel->setFont(titleFont);
    headerLayout->addWidget(titleLabel);

    auto* subtitleLabel = new QLabel(QStringLiteral("신규 등록 · 저장 시 대상자 ID 자동 부여"), header);
    subtitleLabel->setProperty("role", "caption");
    headerLayout->addWidget(subtitleLabel);
    headerLayout->addStretch();

    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_SECONDARY, 16), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    headerLayout->addWidget(closeBtn);

    root->addWidget(header);

    // ---- 본문 ----
    auto* body = new QWidget(this);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);

    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(14);
    topRow->addWidget(buildPhotoBlock());

    auto* formCol = new QVBoxLayout();
    formCol->setSpacing(10);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(10);
    nameEdit_ = new QLineEdit(body);
    nameEdit_->setPlaceholderText(QStringLiteral("홍길동"));
    connect(nameEdit_, &QLineEdit::textChanged, this, &P3RecipientCreateDialog::refreshValidation);
    row1->addWidget(makeFieldBlock(QStringLiteral("이름"), true, nameEdit_, body), 1);
    auto* idField = makeReadOnlyField(QStringLiteral("%1 자동 부여").arg(nextCode_), body);
    row1->addWidget(makeFieldBlock(QStringLiteral("대상자 ID"), false, idField, body), 1);

    // 관리 담당자 — 계정관리(P7)에 등록된 관제담당자 계정 중에서 고른다. 항목이 없으면
    // "담당자 없음" 하나만 남아 미배정 상태로 등록된다.
    operatorCombo_ = new CareGuardComboBox(body);
    operatorCombo_->addItem(QStringLiteral("담당자 없음"), 0);
    for (const auto& op : operators) operatorCombo_->addItem(op.name, op.id);
    row1->addWidget(makeFieldBlock(QStringLiteral("관리 담당자"), false, operatorCombo_, body), 1);
    formCol->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(10);
    birthYearEdit_ = new QLineEdit(body);
    birthYearEdit_->setPlaceholderText(QStringLiteral("1945"));
    birthYearEdit_->setValidator(new QIntValidator(1900, QDate::currentDate().year(), birthYearEdit_));
    birthYearEdit_->setFont(QFont(Tokens::Font::MONO));
    connect(birthYearEdit_, &QLineEdit::textChanged, this, &P3RecipientCreateDialog::refreshValidation);
    row2->addWidget(makeFieldBlock(QStringLiteral("생년"), true, birthYearEdit_, body), 1);

    genderCombo_ = new CareGuardComboBox(body);
    genderCombo_->addItems({QStringLiteral("선택"), QStringLiteral("남"), QStringLiteral("여")});
    connect(genderCombo_, &QComboBox::currentIndexChanged, this, &P3RecipientCreateDialog::refreshValidation);
    row2->addWidget(makeFieldBlock(QStringLiteral("성별"), true, genderCombo_, body), 1);
    formCol->addLayout(row2);

    addressEdit_ = new QLineEdit(body);
    addressEdit_->setPlaceholderText(QStringLiteral("행복빌라 302"));
    connect(addressEdit_, &QLineEdit::textChanged, this, &P3RecipientCreateDialog::refreshValidation);
    formCol->addWidget(makeFieldBlock(QStringLiteral("주거 구역 (주소)"), true, addressEdit_, body));

    topRow->addLayout(formCol, 1);
    layout->addLayout(topRow);

    auto* row3 = new QHBoxLayout();
    row3->setSpacing(10);
    diseasesEdit_ = new QLineEdit(body);
    diseasesEdit_->setPlaceholderText(QStringLiteral("고혈압 · 관절염"));
    row3->addWidget(makeFieldBlock(QStringLiteral("지병"), false, diseasesEdit_, body), 1);
    noteEdit_ = new QLineEdit(body);
    noteEdit_->setPlaceholderText(QStringLiteral("순찰 시 참고할 특이사항"));
    row3->addWidget(makeFieldBlock(QStringLiteral("담당자 메모"), false, noteEdit_, body), 1);
    layout->addLayout(row3);

    auto* robotMapLabel = new QLabel(
        QStringLiteral("담당 로봇 · 맵 배정 "
                        "<span style='font-weight:400; color:%1;'>— 선택 · 맵·로봇 매핑 페이지에서 등록된 항목만 선택 가능</span>")
            .arg(Tokens::TEXT_MUTED.name()),
        body);
    robotMapLabel->setTextFormat(Qt::RichText);
    robotMapLabel->setProperty("role", "label");
    layout->addWidget(robotMapLabel);

    auto* robotMapRow = new QHBoxLayout();
    robotMapRow->setSpacing(10);
    robotCombo_ = new CareGuardComboBox(body);
    robotCombo_->addItem(QStringLiteral("로봇 선택"));
    robotCombo_->addItems(kRobotCodes);
    robotMapRow->addWidget(robotCombo_, 1);

    mapCombo_ = new CareGuardComboBox(body);
    mapCombo_->addItem(QStringLiteral("맵 선택"));
    robotMapRow->addWidget(mapCombo_, 1);
    layout->addLayout(robotMapRow);

    connect(robotCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        mapCombo_->clear();
        mapCombo_->addItem(QStringLiteral("맵 선택"));
        if (index >= 1 && index <= kRobotMaps.size()) {
            // 로봇을 고르면 그 로봇에 매핑된 맵 하나만 후보로 뜬다(§ 와이어프레임 안내 문구).
            mapCombo_->addItem(kRobotMaps[index - 1]);
            mapCombo_->setCurrentIndex(1);
        }
        refreshValidation();
    });
    connect(mapCombo_, &QComboBox::currentIndexChanged, this, &P3RecipientCreateDialog::refreshValidation);

    auto* hintLabel = new QLabel(body);
    hintLabel->setProperty("role", "warning");
    hintLabel->setText(QStringLiteral("ⓘ 로봇을 먼저 고르면 해당 로봇에 매핑된 맵만 표시됩니다"));
    layout->addWidget(hintLabel);

    layout->addWidget(buildGuardianSection());
    layout->addWidget(buildScheduleSection());
    layout->addStretch();

    auto* scrollArea = body;  // 와이어프레임엔 스크롤 명시가 없어 고정 높이로 두되, 내용이 길면
                              // QDialog 자체가 세로로 늘어난다(모든 섹션이 항상 보이는 편이 낫다).
    root->addWidget(scrollArea, /*stretch=*/1);

    // ---- 푸터 ----
    auto* footer = new QFrame(this);
    footer->setObjectName("modalFooter");
    footer->setStyleSheet(QStringLiteral("QFrame#modalFooter{background:%1; border-top:0.5px solid %2;}")
                               .arg(Tokens::BG_SUNKEN.name(), Tokens::BORDER.name()));
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 0, 16, 0);
    footerLayout->setSpacing(8);
    footer->setFixedHeight(52);

    requiredCountLabel_ = new QLabel(footer);
    requiredCountLabel_->setProperty("role", "warning");
    footerLayout->addWidget(requiredCountLabel_);
    footerLayout->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("취소"), footer);
    cancelBtn->setProperty("variant", "secondary");
    cancelBtn->setProperty("size", "s");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    footerLayout->addWidget(cancelBtn);

    registerBtn_ = new QPushButton(QStringLiteral("등록"), footer);
    registerBtn_->setProperty("variant", "primary");
    connect(registerBtn_, &QPushButton::clicked, this, &P3RecipientCreateDialog::handleRegisterClicked);
    footerLayout->addWidget(registerBtn_);

    root->addWidget(footer);

    refreshValidation();
}

bool P3RecipientCreateDialog::eventFilter(QObject* watched, QEvent* event) {
    // 한글 입력 중(조합 중인 자모가 남아있는 상태) 다른 QLineEdit을 클릭하면, 원래 칸엔
    // 정상적으로 커밋되면서 "방금 포커스를 받은" 새 칸에도 같은 자모가 또 나타난다
    // (gui/issue/target5.png). commit()/reset()을 FocusOut·FocusIn 여러 시점에서 시도해봤지만
    // 다 실패했다 — 실제로 ibus-hangul 엔진을 붙여 재현해보니, 이 leak은 로컬 위젯 포커스
    // 이벤트보다 "늦게" 도착하는 비동기 IBus 커밋 콜백이 그 시점에 포커스를 쥔 위젯(=새로
    // 옮겨간 칸)으로 잘못 전달되는 것으로 보인다 — 포커스가 바뀌기 전에 아무리 일찍
    // commit()을 호출해도 그 "응답"이 도착하는 시점 자체가 이미 늦어서 못 막는다.
    //
    // 처음엔 반영된 뒤(80ms 후) 스냅샷으로 되돌리는 방식이었는데, 그러면 화면에 잘못된
    // 글자가 잠깐 나타났다 지워지는 게 눈에 보였다. 그래서 아예 반영 자체를 막는다: 새 칸이
    // 포커스를 받은 시점부터 80ms 동안, 그 칸에 도착하는 QInputMethodEvent 중 "조합 중인
    // 글자 없이 커밋 문자열만 있는" 이벤트(=새로 조합을 시작하는 게 아니라 이전 칸에서 새어
    // 들어온 완성글자)는 위젯에 전달하지 않고 그대로 삼킨다. 사용자가 새 칸에서 바로 한글
    // 조합을 시작하는 정상 입력은 preeditString이 채워진 이벤트라 이 필터에 걸리지 않는다.
    if (event->type() == QEvent::FocusIn) {
        if (qobject_cast<QLineEdit*>(watched)) {
            imeGuardDeadline_[watched] = QDateTime::currentMSecsSinceEpoch() + 80;
        }
    } else if (event->type() == QEvent::InputMethod) {
        auto it = imeGuardDeadline_.find(watched);
        if (it != imeGuardDeadline_.end()) {
            if (QDateTime::currentMSecsSinceEpoch() < it.value()) {
                auto* ime = static_cast<QInputMethodEvent*>(event);
                if (!ime->commitString().isEmpty() && ime->preeditString().isEmpty()) {
                    imeGuardDeadline_.erase(it);
                    return true;
                }
            } else {
                imeGuardDeadline_.erase(it);
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

QWidget* P3RecipientCreateDialog::buildPhotoBlock() {
    auto* wrap = new QWidget(this);
    wrap->setFixedWidth(110);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto* photoBox = new QFrame(wrap);
    photoBox->setObjectName("p3CreatePhotoBox");
    photoBox->setFixedSize(96, 96);
    photoBox->setStyleSheet(
        QStringLiteral("QFrame#p3CreatePhotoBox{background:%1; border:0.5px dashed %2; border-radius:12px;}")
            .arg(Tokens::BG_SUNKEN.name(), Tokens::BORDER_STRONG.name()));
    auto* photoLayout = new QVBoxLayout(photoBox);
    photoLayout->setContentsMargins(0, 0, 0, 0);
    photoLayout->setSpacing(4);
    photoLayout->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel(photoBox);
    icon->setPixmap(SimpleIcons::upload(Tokens::TEXT_MUTED, 22).pixmap(22, 22));
    icon->setAlignment(Qt::AlignCenter);
    photoLayout->addWidget(icon);

    auto* label = new QLabel(QStringLiteral("사진 등록"), photoBox);
    label->setProperty("role", "caption");
    label->setAlignment(Qt::AlignCenter);
    photoLayout->addWidget(label);

    layout->addWidget(photoBox, 0, Qt::AlignHCenter);

    auto* caption = new QLabel(QStringLiteral("JPG·PNG\n최대 5MB · 선택"), wrap);
    caption->setProperty("role", "caption");
    caption->setAlignment(Qt::AlignCenter);
    layout->addWidget(caption);

    return wrap;
}

QWidget* P3RecipientCreateDialog::buildGuardianSection() {
    auto* section = new QWidget(this);
    auto* sectionLayout = new QVBoxLayout(section);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(5);

    auto* headerRow = new QHBoxLayout();
    auto* label = new QLabel(
        QStringLiteral("보호자 · 긴급 연락처 <span style='font-weight:400; color:%1;'>— 1순위 1건 필수</span>")
            .arg(Tokens::TEXT_MUTED.name()),
        section);
    label->setTextFormat(Qt::RichText);
    label->setProperty("role", "label");
    headerRow->addWidget(label);
    headerRow->addStretch();

    auto* addBtn = new QPushButton(SimpleIcons::plus(Tokens::TEXT_PRIMARY, 10),
                                    QStringLiteral("연락처 추가"), section);
    addBtn->setProperty("variant", "secondary");
    addBtn->setProperty("size", "s");
    connect(addBtn, &QPushButton::clicked, this, &P3RecipientCreateDialog::addGuardian);
    headerRow->addWidget(addBtn);
    sectionLayout->addLayout(headerRow);

    guardianTableBox_ = makeBoxFrame(section);
    refreshGuardianTable();
    sectionLayout->addWidget(guardianTableBox_);

    return section;
}

void P3RecipientCreateDialog::refreshGuardianTable() {
    if (QLayout* old = guardianTableBox_->layout()) {
        QLayoutItem* item;
        while ((item = old->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        delete old;
    }

    static const int kPillW = 52, kNameW = 160, kPhoneW = 134, kChannelW = 92;
    static const int kIconW = 24, kIconGap = 6;
    static const int kActionsW = kIconW * 2 + kIconGap;
    static const int kGapPillName = 16, kGapNamePhone = 10, kGapPhoneChannel = 40, kGapChannelEdit = 16;
    static const QRegularExpression kNameRelationRe(QStringLiteral("^(.*[^\\s])\\s*\\((.*)\\)\\s*$"));

    auto* box = guardianTableBox_;
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setSpacing(0);

    auto* header = new QWidget(box);
    header->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BG_SUNKEN.name()));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 6, 10, 6);
    headerLayout->setSpacing(0);
    auto addHeaderCell = [&](const QString& text, int fixedWidth, Qt::Alignment align = Qt::AlignLeft) {
        auto* h = new QLabel(text, header);
        h->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:500;")
                              .arg(Tokens::TEXT_SECONDARY.name()));
        h->setFixedWidth(fixedWidth);
        h->setAlignment(align | Qt::AlignVCenter);
        headerLayout->addWidget(h);
    };
    addHeaderCell(QStringLiteral("순위"), kPillW, Qt::AlignCenter);
    headerLayout->addSpacing(kGapPillName);
    addHeaderCell(QStringLiteral("이름 (관계)"), kNameW);
    headerLayout->addSpacing(kGapNamePhone);
    addHeaderCell(QStringLiteral("연락처"), kPhoneW);
    headerLayout->addSpacing(kGapPhoneChannel);
    addHeaderCell(QStringLiteral("알림 채널"), kChannelW);
    headerLayout->addSpacing(kGapChannelEdit);
    headerLayout->addStretch();
    addHeaderCell(QString(), kActionsW);
    boxLayout->addWidget(header);

    const bool onlyEmergency =
        detail_.guardians.size() == 1 && detail_.guardians.first().isEmergencyFixed;
    if (onlyEmergency) {
        auto* sep = new QFrame(box);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BORDER.name()));
        boxLayout->addWidget(sep);

        auto* empty = new QWidget(box);
        auto* emptyLayout = new QVBoxLayout(empty);
        emptyLayout->setContentsMargins(10, 14, 10, 14);
        emptyLayout->setSpacing(4);
        emptyLayout->setAlignment(Qt::AlignCenter);
        auto* icon = new QLabel(empty);
        icon->setPixmap(SimpleIcons::users(Tokens::TEXT_MUTED, 18).pixmap(18, 18));
        icon->setAlignment(Qt::AlignCenter);
        emptyLayout->addWidget(icon);
        auto* text = new QLabel(QStringLiteral("등록된 연락처가 없습니다 · 알림을 받을 보호자를 1명 이상 추가하세요"), empty);
        text->setProperty("role", "caption");
        text->setAlignment(Qt::AlignCenter);
        emptyLayout->addWidget(text);
        boxLayout->addWidget(empty);
    }

    for (int r = 0; r < detail_.guardians.size(); ++r) {
        const bool emergencyFixed = detail_.guardians[r].isEmergencyFixed;

        auto* sep = new QFrame(box);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BORDER.name()));
        boxLayout->addWidget(sep);

        auto* stack = new QStackedWidget(box);

        auto* viewPage = new QWidget(stack);
        auto* viewLayout = new QHBoxLayout(viewPage);
        viewLayout->setContentsMargins(10, 7, 10, 7);
        viewLayout->setSpacing(0);

        auto* pill = makePriorityPill(detail_.guardians[r].priority, emergencyFixed, viewPage);
        pill->setFixedWidth(kPillW);
        viewLayout->addWidget(pill);
        viewLayout->addSpacing(kGapPillName);

        auto* nameLabel = new QLabel(viewPage);
        nameLabel->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
        nameLabel->setFixedWidth(kNameW);
        viewLayout->addWidget(nameLabel);
        viewLayout->addSpacing(kGapNamePhone);

        auto* phoneLabel = new QLabel(viewPage);
        phoneLabel->setProperty("role", "mono");
        phoneLabel->setFixedWidth(kPhoneW);
        viewLayout->addWidget(phoneLabel);
        viewLayout->addSpacing(kGapPhoneChannel);

        auto* channelLabel = new QLabel(viewPage);
        channelLabel->setProperty("role", "secondary");
        channelLabel->setFixedWidth(kChannelW);
        viewLayout->addWidget(channelLabel);
        viewLayout->addSpacing(kGapChannelEdit);
        viewLayout->addStretch();

        auto refreshViewLabels = [this, r, nameLabel, phoneLabel, channelLabel] {
            const GuardianContact& gg = detail_.guardians[r];
            const QString nameText =
                gg.relation.isEmpty() ? gg.name : QStringLiteral("%1 (%2)").arg(gg.name, gg.relation);
            nameLabel->setText(QFontMetrics(nameLabel->font()).elidedText(nameText, Qt::ElideRight, kNameW));
            nameLabel->setToolTip(nameText);
            phoneLabel->setText(gg.phone);
            channelLabel->setText(gg.isEmergencyFixed
                                       ? QStringLiteral("모의 신고 전용")
                                       : (gg.notifyEnabled ? QStringLiteral("수신") : QStringLiteral("미수신")));
        };
        refreshViewLabels();

        if (!emergencyFixed) {
            auto* editBtn = new QPushButton(viewPage);
            editBtn->setIcon(SimpleIcons::edit(Tokens::TEXT_MUTED, 12));
            editBtn->setProperty("variant", "ghost");
            editBtn->setProperty("size", "s");
            editBtn->setFixedSize(kIconW, kIconW);
            viewLayout->addWidget(editBtn);
            viewLayout->addSpacing(kIconGap);

            auto* deleteBtn = new QPushButton(viewPage);
            deleteBtn->setIcon(SimpleIcons::trash(Tokens::TEXT_MUTED, 12));
            deleteBtn->setProperty("variant", "ghost");
            deleteBtn->setProperty("size", "s");
            deleteBtn->setFixedSize(kIconW, kIconW);
            viewLayout->addWidget(deleteBtn);
            connect(deleteBtn, &QPushButton::clicked, this, [this, r] {
                const QString name = detail_.guardians[r].name;
                const QString question = QStringLiteral("%1 연락처를 삭제할까요?")
                                              .arg(name.isEmpty() ? QStringLiteral("이") : name);
                if (QMessageBox::question(this, QStringLiteral("보호자 삭제"), question,
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::No) == QMessageBox::Yes) {
                    removeGuardian(r);
                }
            });
            stack->addWidget(viewPage);

            auto* editPage = new QWidget(stack);
            auto* editLayout = new QHBoxLayout(editPage);
            editLayout->setContentsMargins(10, 7, 10, 7);
            editLayout->setSpacing(0);

            auto* pillEdit = makePriorityPill(detail_.guardians[r].priority, false, editPage);
            pillEdit->setFixedWidth(kPillW);
            editLayout->addWidget(pillEdit);
            editLayout->addSpacing(kGapPillName);

            auto* nameEdit = new QLineEdit(editPage);
            nameEdit->setFixedWidth(kNameW);
            editLayout->addWidget(nameEdit);
            editLayout->addSpacing(kGapNamePhone);
            auto* phoneEdit = new QLineEdit(editPage);
            phoneEdit->setFixedWidth(kPhoneW);
            installLivePhoneFormatting(phoneEdit);  // 입력하는 즉시 하이픈이 보이도록
            editLayout->addWidget(phoneEdit);
            editLayout->addSpacing(kGapPhoneChannel);
            // DB의 guardian.notify_enabled가 boolean이라 입력도 체크박스로 맞춘다(예전엔
            // "문자 + 전화" 같은 자유 텍스트였는데 저장할 칸이 없어 결국 boolean으로 뭉개졌다).
            auto* channelEdit = new QCheckBox(QStringLiteral("수신"), editPage);
            channelEdit->setFixedWidth(kChannelW);
            editLayout->addWidget(channelEdit);
            editLayout->addSpacing(kGapChannelEdit);
            editLayout->addStretch();

            auto* saveBtn = new QPushButton(editPage);
            saveBtn->setIcon(SimpleIcons::deviceFloppy(Tokens::TEXT_SECONDARY, 12));
            saveBtn->setProperty("variant", "ghost");
            saveBtn->setProperty("size", "s");
            saveBtn->setFixedSize(kIconW, kIconW);
            editLayout->addWidget(saveBtn);
            editLayout->addSpacing(kIconGap);

            auto* cancelBtn = new QPushButton(editPage);
            cancelBtn->setIcon(SimpleIcons::x(Tokens::TEXT_MUTED, 12));
            cancelBtn->setProperty("variant", "ghost");
            cancelBtn->setProperty("size", "s");
            cancelBtn->setFixedSize(kIconW, kIconW);
            editLayout->addWidget(cancelBtn);

            stack->addWidget(editPage);

            connect(editBtn, &QPushButton::clicked, this,
                    [this, r, stack, nameEdit, phoneEdit, channelEdit] {
                        const GuardianContact& gg = detail_.guardians[r];
                        const QString nameText = gg.relation.isEmpty()
                                                      ? gg.name
                                                      : QStringLiteral("%1 (%2)").arg(gg.name, gg.relation);
                        nameEdit->setText(nameText);
                        phoneEdit->setText(gg.phone);
                        channelEdit->setChecked(gg.notifyEnabled);
                        stack->setCurrentIndex(1);
                    });

            connect(saveBtn, &QPushButton::clicked, this,
                    [this, r, stack, nameEdit, phoneEdit, channelEdit, refreshViewLabels] {
                        const QString raw = nameEdit->text().trimmed();
                        if (raw.isEmpty()) return;

                        QString name = raw;
                        QString relation = detail_.guardians[r].relation;
                        const QRegularExpressionMatch m = kNameRelationRe.match(raw);
                        if (m.hasMatch()) {
                            name = m.captured(1).trimmed();
                            relation = m.captured(2).trimmed();
                        }
                        detail_.guardians[r].name = name;
                        detail_.guardians[r].relation = relation;
                        // "-" 없이 숫자만 입력해도 저장 시점에 하이픈을 넣어준다(요청: 노출
                        // 형식 가독성).
                        detail_.guardians[r].phone = formatGuardianPhone(phoneEdit->text());
                        detail_.guardians[r].notifyEnabled = channelEdit->isChecked();
                        refreshViewLabels();
                        stack->setCurrentIndex(0);
                        refreshValidation();
                    });

            connect(cancelBtn, &QPushButton::clicked, this, [this, r, stack] {
                if (detail_.guardians[r].name.isEmpty()) {
                    removeGuardian(r);
                } else {
                    stack->setCurrentIndex(0);
                }
            });

            if (r == autoEditGuardianIndex_) {
                autoEditGuardianIndex_ = -1;
                nameEdit->setText(QString());
                phoneEdit->setText(QString());
                channelEdit->setChecked(true);
                stack->setCurrentIndex(1);
                nameEdit->setFocus();
            }
        } else {
            auto* fixedLabel = new QLabel(QStringLiteral("고정"), viewPage);
            fixedLabel->setProperty("role", "caption");
            fixedLabel->setFixedWidth(kActionsW);
            fixedLabel->setAlignment(Qt::AlignCenter);
            viewLayout->addWidget(fixedLabel);
            stack->addWidget(viewPage);
        }

        boxLayout->addWidget(stack);
    }

    refreshValidation();
}

void P3RecipientCreateDialog::addGuardian() {
    int insertAt = detail_.guardians.size();
    for (int i = 0; i < detail_.guardians.size(); ++i) {
        if (detail_.guardians[i].isEmergencyFixed) {
            insertAt = i;
            break;
        }
    }
    GuardianContact fresh;
    fresh.priority = insertAt + 1;
    detail_.guardians.insert(insertAt, fresh);
    autoEditGuardianIndex_ = insertAt;
    refreshGuardianTable();
}

void P3RecipientCreateDialog::removeGuardian(int index) {
    if (index < 0 || index >= detail_.guardians.size()) return;
    detail_.guardians.removeAt(index);
    int nextPriority = 1;
    for (auto& g : detail_.guardians) {
        if (!g.isEmergencyFixed) g.priority = nextPriority++;
    }
    refreshGuardianTable();
}

QWidget* P3RecipientCreateDialog::buildScheduleSection() {
    auto* section = new QWidget(this);
    auto* sectionLayout = new QVBoxLayout(section);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(5);

    auto* headerRow = new QHBoxLayout();
    auto* label = new QLabel(
        QStringLiteral("초기 복약 스케줄 <span style='font-weight:400; color:%1;'>— 선택 · 등록 후 복약 탭에서도 수정 가능</span>")
            .arg(Tokens::TEXT_MUTED.name()),
        section);
    label->setTextFormat(Qt::RichText);
    label->setProperty("role", "label");
    headerRow->addWidget(label);
    headerRow->addStretch();

    auto* addBtn = new QPushButton(SimpleIcons::plus(Tokens::TEXT_PRIMARY, 10),
                                    QStringLiteral("스케줄 추가"), section);
    addBtn->setProperty("variant", "secondary");
    addBtn->setProperty("size", "s");
    connect(addBtn, &QPushButton::clicked, this, &P3RecipientCreateDialog::addSchedule);
    headerRow->addWidget(addBtn);
    sectionLayout->addLayout(headerRow);

    scheduleTableBox_ = makeBoxFrame(section);
    refreshScheduleTable();
    sectionLayout->addWidget(scheduleTableBox_);

    return section;
}

void P3RecipientCreateDialog::refreshScheduleTable() {
    if (QLayout* old = scheduleTableBox_->layout()) {
        QLayoutItem* item;
        while ((item = old->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        delete old;
    }

    // p3_recipient_detail_dialog.cpp populateScheduleTable()과 같은 열 폭/아이콘 배치.
    // kTimeW는 콤보박스 드롭다운 화살표까지 들어가야 해서 QLineEdit 시절(76px)보다 넓혔다.
    static const int kSlotW = 36, kTimeW = 96, kDaysW = 80;
    static const int kIconW = 24, kIconGap = 6;
    static const int kActionsW = kIconW * 2 + kIconGap;
    static const QStringList kDefaultTimes = {QStringLiteral("08:00"), QStringLiteral("13:00"),
                                               QStringLiteral("19:00")};
    // 자유 입력(normalizeDoseTime()이 "9"/"930"/"9:30" 등도 받아준다)과 함께, 자주 쓰는
    // 시각을 바로 고를 수 있게 드롭다운도 제공한다(요청: 입력만 되던 걸 선택도 가능하게).
    static const QStringList kTimePresets = {
        QStringLiteral("06:00"), QStringLiteral("07:00"), QStringLiteral("07:30"), QStringLiteral("08:00"),
        QStringLiteral("08:30"), QStringLiteral("09:00"), QStringLiteral("12:00"), QStringLiteral("12:30"),
        QStringLiteral("13:00"), QStringLiteral("18:00"), QStringLiteral("18:30"), QStringLiteral("19:00"),
        QStringLiteral("19:30"), QStringLiteral("20:00"), QStringLiteral("21:00"), QStringLiteral("22:00")};

    auto* box = scheduleTableBox_;
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setSpacing(0);

    auto* header = new QWidget(box);
    header->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BG_SUNKEN.name()));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 6, 10, 6);
    headerLayout->setSpacing(8);
    auto addHeaderCell = [&](const QString& text, int fixedWidth, int stretch) {
        auto* h = new QLabel(text, header);
        h->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:500;")
                              .arg(Tokens::TEXT_SECONDARY.name()));
        if (fixedWidth > 0) h->setFixedWidth(fixedWidth);
        headerLayout->addWidget(h, stretch);
    };
    addHeaderCell(QStringLiteral("칸"), kSlotW, 0);
    addHeaderCell(QStringLiteral("약 이름"), 0, 1);
    addHeaderCell(QStringLiteral("시각"), kTimeW, 0);
    addHeaderCell(QStringLiteral("요일"), kDaysW, 0);
    addHeaderCell(QString(), kActionsW, 0);
    boxLayout->addWidget(header);

    if (detail_.schedules.isEmpty()) {
        auto* sep = new QFrame(box);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BORDER.name()));
        boxLayout->addWidget(sep);

        auto* empty = new QWidget(box);
        auto* emptyLayout = new QVBoxLayout(empty);
        emptyLayout->setContentsMargins(10, 14, 10, 14);
        emptyLayout->setSpacing(4);
        emptyLayout->setAlignment(Qt::AlignCenter);
        auto* text = new QLabel(QStringLiteral("등록된 복약 스케줄이 없습니다 · 필요하면 추가하세요"), empty);
        text->setProperty("role", "caption");
        text->setAlignment(Qt::AlignCenter);
        emptyLayout->addWidget(text);
        boxLayout->addWidget(empty);
    }

    for (int r = 0; r < detail_.schedules.size(); ++r) {
        auto* sep = new QFrame(box);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BORDER.name()));
        boxLayout->addWidget(sep);

        auto* stack = new QStackedWidget(box);

        auto* viewPage = new QWidget(stack);
        auto* viewLayout = new QHBoxLayout(viewPage);
        viewLayout->setContentsMargins(10, 7, 10, 7);
        viewLayout->setSpacing(8);

        auto* slotLabel = new QLabel(QString::number(detail_.schedules[r].slotNo), viewPage);
        slotLabel->setProperty("role", "mono");
        slotLabel->setFixedWidth(kSlotW);
        viewLayout->addWidget(slotLabel);

        auto* drugLabel = new QLabel(viewPage);
        drugLabel->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
        viewLayout->addWidget(drugLabel, 1);

        auto* timeLabel = new QLabel(viewPage);
        timeLabel->setProperty("role", "mono");
        timeLabel->setFixedWidth(kTimeW);
        viewLayout->addWidget(timeLabel);

        auto* daysLabel = new QLabel(viewPage);
        daysLabel->setProperty("role", "secondary");
        daysLabel->setFixedWidth(kDaysW);
        viewLayout->addWidget(daysLabel);

        auto refreshViewLabels = [this, r, slotLabel, drugLabel, timeLabel, daysLabel] {
            const MedicationScheduleItem& ss = detail_.schedules[r];
            slotLabel->setText(QString::number(ss.slotNo));
            drugLabel->setText(ss.drugName);
            timeLabel->setText(ss.doseTime);
            daysLabel->setText(ss.daysLabel);
        };
        refreshViewLabels();

        auto* editBtn = new QPushButton(viewPage);
        editBtn->setIcon(SimpleIcons::edit(Tokens::TEXT_MUTED, 12));
        editBtn->setProperty("variant", "ghost");
        editBtn->setProperty("size", "s");
        editBtn->setFixedSize(kIconW, kIconW);
        viewLayout->addWidget(editBtn);

        auto* deleteBtn = new QPushButton(viewPage);
        deleteBtn->setIcon(SimpleIcons::trash(Tokens::TEXT_MUTED, 12));
        deleteBtn->setProperty("variant", "ghost");
        deleteBtn->setProperty("size", "s");
        deleteBtn->setFixedSize(kIconW, kIconW);
        viewLayout->addWidget(deleteBtn);
        connect(deleteBtn, &QPushButton::clicked, this, [this, r] {
            const QString drug = detail_.schedules[r].drugName;
            const QString question =
                QStringLiteral("%1 스케줄을 삭제할까요?").arg(drug.isEmpty() ? QStringLiteral("이") : drug);
            if (QMessageBox::question(this, QStringLiteral("스케줄 삭제"), question,
                                       QMessageBox::Yes | QMessageBox::No,
                                       QMessageBox::No) == QMessageBox::Yes) {
                removeSchedule(r);
            }
        });
        stack->addWidget(viewPage);

        auto* editPage = new QWidget(stack);
        auto* editLayout = new QHBoxLayout(editPage);
        editLayout->setContentsMargins(10, 7, 10, 7);
        editLayout->setSpacing(8);

        auto* slotEdit = new QLabel(QString::number(detail_.schedules[r].slotNo), editPage);  // 칸 번호는 고정
        slotEdit->setProperty("role", "mono");
        slotEdit->setFixedWidth(kSlotW);
        editLayout->addWidget(slotEdit);

        auto* drugEdit = new QLineEdit(editPage);
        drugEdit->setPlaceholderText(QStringLiteral("미설정"));
        editLayout->addWidget(drugEdit, 1);
        // 편집 가능한 콤보박스 — 목록에서 골라도 되고, normalizeDoseTime()이 받아주는
        // 형식이면 뭐든 직접 타이핑해도 된다(예: "9", "930").
        auto* timeEdit = new CareGuardComboBox(editPage);
        timeEdit->setEditable(true);
        timeEdit->addItems(kTimePresets);
        timeEdit->setCurrentText(QString());
        timeEdit->setFixedWidth(kTimeW);
        editLayout->addWidget(timeEdit);
        auto* daysEdit = new QLineEdit(editPage);
        daysEdit->setFixedWidth(kDaysW);
        editLayout->addWidget(daysEdit);

        auto* saveBtn = new QPushButton(editPage);
        saveBtn->setIcon(SimpleIcons::deviceFloppy(Tokens::TEXT_SECONDARY, 12));
        saveBtn->setProperty("variant", "ghost");
        saveBtn->setProperty("size", "s");
        saveBtn->setFixedSize(kIconW, kIconW);
        editLayout->addWidget(saveBtn);

        auto* cancelBtn = new QPushButton(editPage);
        cancelBtn->setIcon(SimpleIcons::x(Tokens::TEXT_MUTED, 12));
        cancelBtn->setProperty("variant", "ghost");
        cancelBtn->setProperty("size", "s");
        cancelBtn->setFixedSize(kIconW, kIconW);
        editLayout->addWidget(cancelBtn);

        stack->addWidget(editPage);

        connect(editBtn, &QPushButton::clicked, this, [this, r, stack, drugEdit, timeEdit, daysEdit] {
            const MedicationScheduleItem& ss = detail_.schedules[r];
            drugEdit->setText(ss.drugName);
            timeEdit->setCurrentText(ss.doseTime);
            daysEdit->setText(ss.daysLabel);
            stack->setCurrentIndex(1);
        });

        connect(saveBtn, &QPushButton::clicked, this,
                [this, r, stack, drugEdit, timeEdit, daysEdit, refreshViewLabels] {
                    static const QStringList kDefaultTimes = {QStringLiteral("08:00"), QStringLiteral("13:00"),
                                                               QStringLiteral("19:00")};
                    const QString drugName = drugEdit->text().trimmed();
                    if (drugName.isEmpty()) return;

                    // 저장 버튼을 누르는 즉시 검사한다 — 시각 형식이 이상하면 등록까지
                    // 안 가고 이 자리에서 바로 고칠 수 있게(요청: 저장 시점부터 유효성 검사).
                    const QString rawTime = timeEdit->currentText().trimmed();
                    QString normalizedTime;
                    if (!rawTime.isEmpty()) {
                        normalizedTime = normalizeDoseTime(rawTime);
                        if (normalizedTime.isEmpty()) {
                            QMessageBox::warning(
                                this, QStringLiteral("복용 시각 오류"),
                                QStringLiteral("'%1'을(를) 시각으로 알아볼 수 없습니다.\n9, 09:30, 930처럼 "
                                               "입력하거나 목록에서 골라주세요.")
                                    .arg(rawTime));
                            return;  // 수정 모드 유지 — 바로 고칠 수 있게
                        }
                    } else if (detail_.schedules[r].doseTime.isEmpty()) {
                        const int slotNo = detail_.schedules[r].slotNo;
                        normalizedTime = (slotNo >= 1 && slotNo <= 3) ? kDefaultTimes[slotNo - 1] : QString();
                    } else {
                        normalizedTime = detail_.schedules[r].doseTime;  // 안 건드렸으면 기존 값 유지
                    }

                    detail_.schedules[r].drugName = drugName;
                    detail_.schedules[r].doseTime = normalizedTime;
                    const QString days = daysEdit->text().trimmed();
                    if (!days.isEmpty()) {
                        detail_.schedules[r].daysLabel = days;
                    } else if (detail_.schedules[r].daysLabel.isEmpty()) {
                        detail_.schedules[r].daysLabel = QStringLiteral("매일");
                    }
                    refreshViewLabels();
                    stack->setCurrentIndex(0);
                });

        connect(cancelBtn, &QPushButton::clicked, this, [this, r, stack] {
            if (detail_.schedules[r].drugName.isEmpty()) {
                removeSchedule(r);
            } else {
                stack->setCurrentIndex(0);
            }
        });

        if (r == autoEditScheduleIndex_) {
            autoEditScheduleIndex_ = -1;
            const int slotNo = detail_.schedules[r].slotNo;
            drugEdit->setText(QString());
            timeEdit->setCurrentText(QString());
            if (timeEdit->lineEdit()) {
                timeEdit->lineEdit()->setPlaceholderText(
                    slotNo >= 1 && slotNo <= 3 ? kDefaultTimes[slotNo - 1] : QString());
            }
            daysEdit->setText(QString());
            daysEdit->setPlaceholderText(QStringLiteral("매일"));
            stack->setCurrentIndex(1);
            drugEdit->setFocus();
        }

        boxLayout->addWidget(stack);
    }
}

void P3RecipientCreateDialog::addSchedule() {
    // populateScheduleTable()의 addSchedule()과 같은 로직(빈 칸부터, 다 찼으면 계속 증가) —
    // [[feedback_test_full_state_space]]에서 정리한 그 알고리즘을 그대로 재사용한다.
    bool used[4] = {false, false, false, false};
    int maxSlot = 0;
    for (const auto& s : detail_.schedules) {
        if (s.slotNo >= 1 && s.slotNo <= 3) used[s.slotNo] = true;
        if (s.slotNo > maxSlot) maxSlot = s.slotNo;
    }
    int nextSlot = 0;
    for (int slot = 1; slot <= 3; ++slot) {
        if (!used[slot]) {
            nextSlot = slot;
            break;
        }
    }
    if (nextSlot == 0) nextSlot = maxSlot + 1;

    MedicationScheduleItem fresh;
    fresh.slotNo = nextSlot;
    detail_.schedules.append(fresh);
    autoEditScheduleIndex_ = detail_.schedules.size() - 1;
    refreshScheduleTable();
}

void P3RecipientCreateDialog::removeSchedule(int index) {
    if (index < 0 || index >= detail_.schedules.size()) return;
    detail_.schedules.removeAt(index);
    refreshScheduleTable();
}

void P3RecipientCreateDialog::refreshValidation() {
    // 담당 로봇·맵 배정은 필수에서 제외한다(리뷰 지적) — 맵·로봇 매핑 페이지에서 나중에
    // 배정할 수도 있으니 등록 시점에 반드시 골라야 할 이유가 없다.
    int filled = 0;
    const int total = 4;
    if (!nameEdit_->text().trimmed().isEmpty()) ++filled;
    if (!birthYearEdit_->text().trimmed().isEmpty()) ++filled;
    if (genderCombo_->currentIndex() > 0) ++filled;
    if (!addressEdit_->text().trimmed().isEmpty()) ++filled;

    if (requiredCountLabel_) {
        requiredCountLabel_->setText(
            QStringLiteral("* 필수 항목 %1개 중 %2개 입력됨").arg(total).arg(filled));
    }
    if (registerBtn_) registerBtn_->setEnabled(filled == total);
}

void P3RecipientCreateDialog::handleRegisterClicked() {
    const bool hasNonEmergencyGuardian =
        std::any_of(detail_.guardians.cbegin(), detail_.guardians.cend(),
                    [](const GuardianContact& g) { return !g.isEmergencyFixed; });
    if (!hasNonEmergencyGuardian) {
        QMessageBox::warning(this, QStringLiteral("보호자 연락처 필요"),
                              QStringLiteral("보호자 · 긴급 연락처를 1명 이상 추가해야 합니다."));
        return;
    }

    RecipientCard card;
    card.code = nextCode_;
    card.name = nameEdit_->text().trimmed();
    card.robotCode =
        robotCombo_->currentIndex() > 0 ? kRobotCodes[robotCombo_->currentIndex() - 1] : QStringLiteral("미배정");
    card.address = addressEdit_->text().trimmed();
    card.operatorId = operatorCombo_->currentData().toInt();
    card.operatorName = card.operatorId > 0 ? operatorCombo_->currentText() : QString();
    card.level = Tokens::Level::Normal;
    card.statusSummary = QStringLiteral("신규 등록 · 아직 순찰 기록 없음");
    // meds[3]는 기본값(Pending)을 그대로 둔다 — RecipientCard의 멤버 기본 초기화와 동일.

    detail_.card = card;
    detail_.gender = genderCombo_->currentText();
    detail_.birthYear = birthYearEdit_->text().trimmed().toInt();
    detail_.diseases = diseasesEdit_->text().trimmed();
    detail_.note = noteEdit_->text().trimmed();
    detail_.mapWaypointCount = 0;

    // detail_.schedules는 스케줄 표(refreshScheduleTable/addSchedule/저장 버튼)가 실시간으로
    // 관리하므로 여기서 다시 읽어올 필요가 없다 — 보호자 목록과 같은 방식.
    accept();
}
