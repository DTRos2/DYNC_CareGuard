#include "p3_recipient_detail_dialog.h"

#include <QApplication>
#include <QDate>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSize>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {

// p3_recipient_page.cpp의 makeStatusBadge()와 같은 발상 — 두 파일 다 자기 안에서만
// 쓰는 로컬 헬퍼라 공용 유틸로 뽑지 않았다(이 코드베이스의 페이지별 로컬 중복 관례).
QLabel* makeStatusBadge(Tokens::Level level, QWidget* parent) {
    auto* badge = new QLabel(parent);
    badge->setObjectName("badge");
    switch (level) {
        case Tokens::Level::Emergency:
            badge->setText(QStringLiteral("긴급"));
            badge->setProperty("level", "emergency");
            break;
        case Tokens::Level::Attention:
            badge->setText(QStringLiteral("주의"));
            badge->setProperty("level", "attention");
            break;
        case Tokens::Level::Offline:
            badge->setText(QStringLiteral("오프라인"));
            badge->setProperty("level", "offline");
            break;
        case Tokens::Level::Normal:
            badge->setText(QStringLiteral("정상"));
            badge->setProperty("level", "offline");  // §11-3: 정상에 초록 배경 금지 — OFFLINE 톤 재사용
            break;
    }
    return badge;
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", "label");
    return label;
}

// QLabel은 QFrame의 서브클래스라서, 여기 담길 QFrame에 타입 선택자(QFrame{...})나 선택자 없는
// "맨 선언"으로 스타일시트를 걸면 그 안의 모든 QLabel 자식에도 테두리가 그대로 새어 들어가
// 라벨 하나하나가 둥근 테두리 박스처럼 보이는 버그가 난다(보호자·복약 표, 오늘의 약통 카드에서
// 실제로 재현됨 — QLabel is-a QFrame이라는 사실을 놓쳐서 생긴 문제). 그래서 반드시 objectName +
// ID 선택자(QFrame#name{...})로 이 프레임 자신에게만 스코프를 좁혀야 한다.
QFrame* makeBoxFrame(QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setObjectName("p3Box");
    frame->setStyleSheet(QStringLiteral("QFrame#p3Box{border:0.5px solid %1; border-radius:8px;}")
                              .arg(Tokens::BORDER.name()));
    return frame;
}

QLineEdit* makeReadOnlyField(const QString& text, QWidget* parent) {
    auto* edit = new QLineEdit(text, parent);
    edit->setReadOnly(true);
    edit->setStyleSheet(QStringLiteral("background:%1; color:%2;")
                             .arg(Tokens::BG_SUNKEN.name(), Tokens::TEXT_MUTED.name()));
    return edit;
}

QWidget* makeFieldBlock(const QString& label, QWidget* editWidget, QWidget* parent) {
    auto* wrap = new QWidget(parent);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    layout->addWidget(makeSectionLabel(label, wrap));
    layout->addWidget(editWidget);
    return wrap;
}

QLabel* makePriorityPill(const GuardianContact& g, QWidget* parent) {
    auto* pill = new QLabel(parent);
    pill->setAlignment(Qt::AlignCenter);
    if (g.isEmergencyFixed) {
        pill->setText(QStringLiteral("긴급"));
        pill->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:4px; "
                                            "font-weight:500; padding:1px 6px;")
                                 .arg(Tokens::EMERGENCY.soft.name(), Tokens::EMERGENCY.main.name()));
    } else if (g.priority == 1) {
        pill->setText(QStringLiteral("1순위"));
        pill->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:4px; "
                                            "font-weight:500; padding:1px 6px;")
                                 .arg(Tokens::ACCENT_SOFT.name(), Tokens::ACCENT_HOVER.name()));
    } else {
        pill->setText(QStringLiteral("%1순위").arg(g.priority));
        pill->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:4px; padding:1px 6px;")
                                 .arg(Tokens::BG_SUNKEN.name(), Tokens::TEXT_SECONDARY.name()));
    }
    return pill;
}

// 오늘의 약통 3칸(§5-3과 같은 상태값, 크기만 카드보다 크게) 배경/테두리/문구 색.
struct PillboxStyle { QColor bg; QString border; QString text; QColor textColor; };
PillboxStyle pillboxStyle(MedSlotState state) {
    switch (state) {
        case MedSlotState::Missed:
            return {Tokens::EMERGENCY.soft, QStringLiteral("0.5px solid %1").arg(Tokens::EMERGENCY.border.name()),
                     QStringLiteral("✕ 미수령"), Tokens::EMERGENCY.main};
        case MedSlotState::InProgress:
            return {Tokens::BG_SURFACE, QStringLiteral("1px solid %1").arg(Tokens::INFO.main.name()),
                     // ⏳(U+23F3)는 컬러 이모지라 QSS color를 무시한다 — p3_recipient_page.cpp의
                     // medSlot과 같은 이유로 일반 도형 문자(◐)를 쓴다.
                     QStringLiteral("◐ 개방 중"), Tokens::INFO.main};
        case MedSlotState::Done:
            return {Tokens::BG_SURFACE, QStringLiteral("0.5px solid %1").arg(Tokens::OK.border.name()),
                     QStringLiteral("✓ 수령 완료"), Tokens::OK.main};
        case MedSlotState::Pending:
        default:
            return {Tokens::BG_SURFACE, QStringLiteral("0.5px solid %1").arg(Tokens::BORDER_STRONG.name()),
                     QStringLiteral("예정"), Tokens::TEXT_MUTED};
    }
}

}  // namespace

P3RecipientDetailDialog::P3RecipientDetailDialog(const RecipientDetail& detail, QWidget* parent)
    : QDialog(parent), detail_(detail) {
    setModal(true);
    // OS 창관리자 타이틀바를 없애 커스텀 헤더(제목·X)만 남긴다 — 안 그러면 X가 두 개로 보임
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setFixedWidth(Tokens::Layout::MODAL_W_M);
    setWindowTitle(QStringLiteral("대상자 상세 — %1").arg(detail_.card.name));

    // 한글 IME 조합 문자가 다른 칸으로 새는 문제의 방어 코드 — 자세한 원인과 시행착오는
    // eventFilter() 주석 참고.
    qApp->installEventFilter(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildBasicInfoTab(), QStringLiteral("기본 정보"));
    tabs->addTab(buildMedicationTab(), QStringLiteral("복약"));
    root->addWidget(tabs, /*stretch=*/1);

    root->addWidget(buildFooter());
}

bool P3RecipientDetailDialog::eventFilter(QObject* watched, QEvent* event) {
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

QWidget* P3RecipientDetailDialog::buildHeader() {
    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    auto* name = new QLabel(detail_.card.name, header);
    QFont nameFont = name->font();
    nameFont.setPixelSize(Tokens::Font::BODY);
    nameFont.setWeight(QFont::Medium);
    name->setFont(nameFont);
    layout->addWidget(name);

    auto* idInfo = new QLabel(
        QStringLiteral("%1 · %2 · %3").arg(detail_.card.code, detail_.card.robotCode, detail_.card.address),
        header);
    idInfo->setProperty("role", "mono");
    layout->addWidget(idInfo);

    layout->addWidget(makeStatusBadge(detail_.card.level, header));
    layout->addStretch();

    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_SECONDARY, 16), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(closeBtn);

    return header;
}

QWidget* P3RecipientDetailDialog::buildBasicInfoTab() {
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);

    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(14);
    topRow->addWidget(buildPhotoBlock());

    auto* formCol = new QVBoxLayout();
    formCol->setSpacing(10);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(10);
    nameEdit_ = new QLineEdit(detail_.card.name, tab);
    row1->addWidget(makeFieldBlock(QStringLiteral("이름"), nameEdit_, tab), 1);
    auto* idField = makeReadOnlyField(QStringLiteral("%1 자동").arg(detail_.card.code), tab);
    row1->addWidget(makeFieldBlock(QStringLiteral("대상자 ID"), idField, tab), 1);
    formCol->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(10);
    birthYearEdit_ = new QLineEdit(
        detail_.birthYear > 0 ? QString::number(detail_.birthYear) : QString(), tab);
    birthYearEdit_->setFont(QFont(Tokens::Font::MONO));
    row2->addWidget(makeFieldBlock(QStringLiteral("생년"), birthYearEdit_, tab), 1);
    genderEdit_ = new QLineEdit(detail_.gender, tab);
    row2->addWidget(makeFieldBlock(QStringLiteral("성별"), genderEdit_, tab), 1);
    formCol->addLayout(row2);

    addressEdit_ = new QLineEdit(detail_.card.address, tab);
    formCol->addWidget(makeFieldBlock(QStringLiteral("주거 구역 (주소)"), addressEdit_, tab));

    topRow->addLayout(formCol, 1);
    layout->addLayout(topRow);

    auto* row3 = new QHBoxLayout();
    row3->setSpacing(10);
    diseasesEdit_ = new QLineEdit(detail_.diseases, tab);
    row3->addWidget(makeFieldBlock(QStringLiteral("지병"), diseasesEdit_, tab), 1);
    noteEdit_ = new QLineEdit(detail_.note, tab);
    row3->addWidget(makeFieldBlock(QStringLiteral("담당자 메모"), noteEdit_, tab), 1);
    layout->addLayout(row3);

    // 담당 로봇 · 맵 배정 — 맵·로봇 매핑 페이지에서 등록된 항목이라 여기서는 표시만 한다.
    // p5_map_page.cpp/p6_patrol_page.cpp의 robotChip 버튼과 같은 위젯을 그대로 재사용한다
    // (QSS #robotChip: 1px 테두리 · 28px 높이 · 좌측 아이콘 + 한 줄 텍스트 — 라벨을 여러 개
    // 겹쳐 만들다 생긴 "테두리가 글자를 덮는" 문제를 피하려면 이미 검증된 이 위젯을 쓰는 게 맞다).
    auto* robotMapLabel = new QLabel(
        QStringLiteral("담당 로봇 · 맵 배정 <span style='font-weight:400; color:%1;'>— 맵·로봇 매핑 페이지에서 등록된 항목</span>")
            .arg(Tokens::TEXT_MUTED.name()),
        tab);
    robotMapLabel->setTextFormat(Qt::RichText);
    robotMapLabel->setProperty("role", "label");
    layout->addWidget(robotMapLabel);

    auto* robotMapRow = new QHBoxLayout();
    robotMapRow->setSpacing(10);

    auto* robotChip = new QPushButton(SimpleIcons::robot(Tokens::TEXT_SECONDARY, 13), QString(), tab);
    robotChip->setObjectName("robotChip");
    robotChip->setIconSize(QSize(13, 13));
    robotChip->setText(QStringLiteral("  %1  LIMO · 정상  ▾").arg(detail_.card.robotCode));
    connect(robotChip, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("담당 로봇 변경 — 맵·로봇 매핑 페이지에서 편집")); });
    robotMapRow->addWidget(robotChip, 1);

    auto* mapChip = new QPushButton(SimpleIcons::mapPin(Tokens::TEXT_SECONDARY, 12), QString(), tab);
    mapChip->setObjectName("robotChip");
    mapChip->setIconSize(QSize(12, 12));
    mapChip->setText(QStringLiteral("  %1 맵  WP %2개  ▾")
                          .arg(detail_.card.address)
                          .arg(detail_.mapWaypointCount));
    connect(mapChip, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("맵 배정 변경 — 맵·로봇 매핑 페이지에서 편집")); });
    robotMapRow->addWidget(mapChip, 1);

    layout->addLayout(robotMapRow);

    auto* guardianHeader = new QHBoxLayout();
    auto* guardianLabel = new QLabel(
        QStringLiteral("보호자 · 긴급 연락처 <span style='font-weight:400; color:%1;'>— 알림 발송 대상</span>")
            .arg(Tokens::TEXT_MUTED.name()),
        tab);
    guardianLabel->setTextFormat(Qt::RichText);
    guardianLabel->setProperty("role", "label");
    guardianHeader->addWidget(guardianLabel);
    guardianHeader->addStretch();
    auto* addContactBtn = new QPushButton(SimpleIcons::plus(Tokens::TEXT_PRIMARY, 10),
                                           QStringLiteral("연락처 추가"), tab);
    addContactBtn->setProperty("variant", "secondary");
    addContactBtn->setProperty("size", "s");
    connect(addContactBtn, &QPushButton::clicked, this, &P3RecipientDetailDialog::addGuardian);
    guardianHeader->addWidget(addContactBtn);
    layout->addLayout(guardianHeader);

    guardianTableBox_ = makeBoxFrame(this);
    populateGuardianTable(guardianTableBox_);
    layout->addWidget(guardianTableBox_);
    layout->addStretch();

    return tab;
}

QWidget* P3RecipientDetailDialog::buildPhotoBlock() {
    auto* wrap = new QWidget(this);
    wrap->setFixedWidth(110);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignHCenter);

    // §5-6: 미등록 상태 — 회색 인물 실루엣 플레이스홀더. 사진 업로드 API가 없어 항상 이 상태만
    // 보여준다(등록됨 상태·변경/삭제 버튼 쌍은 실제 업로드가 붙기 전까지 만들지 않는다).
    auto* photoBox = new QFrame(wrap);
    photoBox->setObjectName("p3PhotoBox");
    photoBox->setFixedSize(96, 96);
    photoBox->setStyleSheet(
        QStringLiteral("QFrame#p3PhotoBox{background:%1; border:0.5px solid %2; border-radius:12px;}")
            .arg(Tokens::BG_SUNKEN.name(), Tokens::BORDER_STRONG.name()));
    auto* photoLayout = new QVBoxLayout(photoBox);
    photoLayout->setContentsMargins(0, 0, 0, 0);
    auto* silhouette = new QLabel(photoBox);
    silhouette->setPixmap(SimpleIcons::user(Tokens::TEXT_MUTED, 40).pixmap(40, 40));
    silhouette->setAlignment(Qt::AlignCenter);
    photoLayout->addWidget(silhouette);
    layout->addWidget(photoBox, 0, Qt::AlignHCenter);

    auto* registerBtn = new QPushButton(QStringLiteral("사진 등록"), wrap);
    registerBtn->setProperty("variant", "secondary");
    registerBtn->setProperty("size", "s");
    connect(registerBtn, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("사진 등록")); });
    layout->addWidget(registerBtn, 0, Qt::AlignHCenter);

    auto* caption = new QLabel(QStringLiteral("JPG·PNG\n최대 5MB"), wrap);
    caption->setProperty("role", "caption");
    caption->setAlignment(Qt::AlignCenter);
    layout->addWidget(caption);

    return wrap;
}

void P3RecipientDetailDialog::populateGuardianTable(QWidget* box) {
    // QGridLayout으로 칸마다 배경/패딩을 따로 주면 horizontalSpacing만큼 흰 틈이 격자로 보여
    // "칸별 알약" 같은 모양이 된다(리뷰 지적). event_detail_modal.cpp의 음성 확인 로그처럼
    // 행 전체를 하나의 QWidget으로 두고 1px 구분선으로만 나누는 방식으로 통일한다.
    // kPhoneW는 연락처 칸의 QLabel(보기)·QLineEdit(수정) 공통 폭이다. QLineEdit 하나만 더
    // 넓게 쓰면(테두리·padding 탓에 QLabel보다 실사용 폭이 좁아서) 그 뒤 채널·아이콘 칸이
    // 수정 모드에서만 오른쪽으로 밀려 헤더와 안 맞아 보인다(gui/issue/target1.png 지적) —
    // 그래서 라벨·인풋 모두 이 상수 하나를 공유한다.
    static const int kPillW = 52, kNameW = 160, kPhoneW = 134, kChannelW = 92;
    static const int kIconW = 24, kIconGap = 6;
    // 보기 모드는 편집+삭제 두 아이콘, 수정 모드는 저장+취소 두 아이콘이 나란히 붙는다 —
    // 헤더의 마지막 칸도 이 두 아이콘 폭에 맞춰야 우측 끝 정렬이 흐트러지지 않는다.
    static const int kActionsW = kIconW * 2 + kIconGap;
    // 칸 사이 간격을 균일한 spacing 하나로 주면 이름(가변폭) 뒤 여백이 다른 칸보다 훨씬
    // 넓어 보이는 문제가 생긴다(리뷰 지적: 순위-이름 너무 가깝고, 이름-연락처 너무 멀고,
    // 연락처-알림채널 너무 가까움). 이름 칸을 고정폭으로 바꾸고, 칸마다 간격을 따로 준다.
    static const int kGapPillName = 16, kGapNamePhone = 10, kGapPhoneChannel = 40, kGapChannelEdit = 16;

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
    // 순위 헤더는 칩(makePriorityPill)이 칸 안에서 가운데 정렬이라 헤더 글자도 가운데
    // 맞춰야 칩과 위아래로 시각적으로 맞아 보인다(나머지 칸은 본문도 좌측 정렬이라 그대로 둠).
    addHeaderCell(QStringLiteral("순위"), kPillW, Qt::AlignCenter);
    headerLayout->addSpacing(kGapPillName);
    addHeaderCell(QStringLiteral("이름 (관계)"), kNameW);
    headerLayout->addSpacing(kGapNamePhone);
    addHeaderCell(QStringLiteral("연락처"), kPhoneW);
    headerLayout->addSpacing(kGapPhoneChannel);
    addHeaderCell(QStringLiteral("알림 채널"), kChannelW);
    headerLayout->addSpacing(kGapChannelEdit);
    // 편집 아이콘이 테이블 우측 끝에 최대한 붙도록 남는 폭을 여기서 흡수한다.
    headerLayout->addStretch();
    addHeaderCell(QString(), kActionsW);
    boxLayout->addWidget(header);

    // "이름 (관계)" 한 필드로 보여주지만 실제 데이터는 name/relation이 분리돼 있어(guardian
    // 테이블), 저장 시 "이름 (관계)" 형식이면 되돌려 나누고 아니면 전부 이름으로 취급한다.
    static const QRegularExpression kNameRelationRe(QStringLiteral("^(.*[^\\s])\\s*\\((.*)\\)\\s*$"));

    for (int r = 0; r < detail_.guardians.size(); ++r) {
        const bool emergencyFixed = detail_.guardians[r].isEmergencyFixed;

        auto* sep = new QFrame(box);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BORDER.name()));
        boxLayout->addWidget(sep);

        auto* stack = new QStackedWidget(box);

        // ---- 보기 모드 ----
        auto* viewPage = new QWidget(stack);
        auto* viewLayout = new QHBoxLayout(viewPage);
        viewLayout->setContentsMargins(10, 7, 10, 7);
        viewLayout->setSpacing(0);

        auto* pill = makePriorityPill(detail_.guardians[r], viewPage);
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
        // 편집 아이콘(또는 긴급 고정 행의 spacer)이 테이블 우측 끝에 최대한 붙도록 남는
        // 폭을 여기서 흡수한다 — 순위 칸은 왼쪽 끝에 그대로 붙어 있다.
        viewLayout->addStretch();

        auto refreshViewLabels = [this, r, nameLabel, phoneLabel, channelLabel] {
            const GuardianContact& gg = detail_.guardians[r];
            const QString nameText =
                gg.relation.isEmpty() ? gg.name : QStringLiteral("%1 (%2)").arg(gg.name, gg.relation);
            nameLabel->setText(QFontMetrics(nameLabel->font()).elidedText(nameText, Qt::ElideRight, kNameW));
            nameLabel->setToolTip(nameText);
            phoneLabel->setText(gg.phone);
            channelLabel->setText(gg.notifyChannel);
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
                const QString question =
                    QStringLiteral("%1 연락처를 삭제할까요?").arg(name.isEmpty() ? QStringLiteral("이") : name);
                if (QMessageBox::question(this, QStringLiteral("보호자 삭제"), question,
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::No) == QMessageBox::Yes) {
                    removeGuardian(r);
                }
            });
            stack->addWidget(viewPage);  // index 0

            // ---- 수정 모드 — 클릭했을 때만 QLineEdit으로 바뀐다 ----
            auto* editPage = new QWidget(stack);
            auto* editLayout = new QHBoxLayout(editPage);
            editLayout->setContentsMargins(10, 7, 10, 7);
            editLayout->setSpacing(0);

            auto* pillEdit = makePriorityPill(detail_.guardians[r], editPage);  // 순위는 여기서 편집하지 않음
            pillEdit->setFixedWidth(kPillW);
            editLayout->addWidget(pillEdit);
            editLayout->addSpacing(kGapPillName);

            auto* nameEdit = new QLineEdit(editPage);
            nameEdit->setFixedWidth(kNameW);
            editLayout->addWidget(nameEdit);
            editLayout->addSpacing(kGapNamePhone);
            auto* phoneEdit = new QLineEdit(editPage);
            phoneEdit->setFixedWidth(kPhoneW);
            editLayout->addWidget(phoneEdit);
            editLayout->addSpacing(kGapPhoneChannel);
            auto* channelEdit = new QLineEdit(editPage);
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

            stack->addWidget(editPage);  // index 1

            connect(editBtn, &QPushButton::clicked, this,
                    [this, r, stack, nameEdit, phoneEdit, channelEdit] {
                        const GuardianContact& gg = detail_.guardians[r];
                        const QString nameText = gg.relation.isEmpty()
                                                      ? gg.name
                                                      : QStringLiteral("%1 (%2)").arg(gg.name, gg.relation);
                        nameEdit->setText(nameText);
                        phoneEdit->setText(gg.phone);
                        channelEdit->setText(gg.notifyChannel);
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
                        detail_.guardians[r].phone = phoneEdit->text().trimmed();
                        detail_.guardians[r].notifyChannel = channelEdit->text().trimmed();
                        refreshViewLabels();
                        stack->setCurrentIndex(0);
                    });

            // 방금 "연락처 추가"로 만든 빈 행에서 취소를 누르면 빈 행을 남기지 않고 통째로
            // 지운다 — 그 외(기존 값이 있던 행 편집 중 취소)에는 보기 모드로만 돌아간다.
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
                channelEdit->setText(QString());
                stack->setCurrentIndex(1);
                nameEdit->setFocus();
            }
        } else {
            auto* spacer = new QWidget(viewPage);
            spacer->setFixedWidth(kActionsW);
            viewLayout->addWidget(spacer);
            stack->addWidget(viewPage);  // 긴급 고정 행은 수정 모드 자체가 없다
        }

        boxLayout->addWidget(stack);
    }
}

// populateGuardianTable()이 만든 자식(헤더·구분선·행 스택)을 전부 지우고 detail_.guardians로
// 다시 채운다. 행마다 편집기 상태(QStackedWidget)를 개별로 갱신하는 대신 통째로 다시 그리는
// 이유는, 삭제로 인덱스가 밀리면 각 행 위젯에 캡처된 int r이 전부 stale해지기 때문이다.
void P3RecipientDetailDialog::refreshGuardianTable() {
    if (!guardianTableBox_) return;
    if (QLayout* old = guardianTableBox_->layout()) {
        QLayoutItem* item;
        while ((item = old->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        delete old;
    }
    populateGuardianTable(guardianTableBox_);
}

void P3RecipientDetailDialog::addGuardian() {
    // 긴급 고정 행(119·112) 앞에 새 행을 끼워 넣는다 — 그 행은 항상 마지막 순위여야 한다.
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
    autoEditGuardianIndex_ = insertAt;  // 다시 그린 뒤 이 행을 바로 수정 모드로 연다
    refreshGuardianTable();
}

void P3RecipientDetailDialog::removeGuardian(int index) {
    if (index < 0 || index >= detail_.guardians.size()) return;
    detail_.guardians.removeAt(index);
    int nextPriority = 1;
    for (auto& g : detail_.guardians) {
        if (!g.isEmergencyFixed) g.priority = nextPriority++;
    }
    refreshGuardianTable();
}

QWidget* P3RecipientDetailDialog::buildMedicationTab() {
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);

    if (!detail_.medAlertText.isEmpty()) {
        auto* banner = new QFrame(tab);
        banner->setObjectName("banner");
        banner->setProperty("level", Tokens::Prop::ATTENTION);
        auto* bannerLayout = new QHBoxLayout(banner);
        bannerLayout->setContentsMargins(10, 9, 10, 9);
        bannerLayout->setSpacing(8);

        auto* icon = new QLabel(banner);
        icon->setPixmap(SimpleIcons::alertTriangle(Tokens::ATTENTION.main, 13).pixmap(13, 13));
        bannerLayout->addWidget(icon);

        auto* text = new QLabel(detail_.medAlertText, banner);
        text->setWordWrap(true);
        bannerLayout->addWidget(text, 1);

        auto* renotifyBtn = new QPushButton(QStringLiteral("재안내"), banner);
        renotifyBtn->setProperty("variant", "primary");
        renotifyBtn->setProperty("size", "s");
        connect(renotifyBtn, &QPushButton::clicked, this,
                [this] { showComingSoon(QStringLiteral("복약 재안내")); });
        bannerLayout->addWidget(renotifyBtn);

        auto* ackBtn = new QPushButton(QStringLiteral("확인 처리"), banner);
        ackBtn->setProperty("variant", "secondary");
        ackBtn->setProperty("size", "s");
        connect(ackBtn, &QPushButton::clicked, this,
                [this] { showComingSoon(QStringLiteral("미수령 확인 처리")); });
        bannerLayout->addWidget(ackBtn);

        layout->addWidget(banner);
    }

    layout->addWidget(makeSectionLabel(QStringLiteral("오늘의 약통 상태"), tab));
    layout->addWidget(buildTodayPillbox());

    auto* scheduleHeader = new QHBoxLayout();
    scheduleHeader->addWidget(makeSectionLabel(QStringLiteral("복약 스케줄"), tab));
    scheduleHeader->addStretch();
    auto* addScheduleBtn = new QPushButton(SimpleIcons::plus(Tokens::TEXT_PRIMARY, 10),
                                            QStringLiteral("스케줄 추가"), tab);
    addScheduleBtn->setProperty("variant", "secondary");
    addScheduleBtn->setProperty("size", "s");
    connect(addScheduleBtn, &QPushButton::clicked, this, &P3RecipientDetailDialog::addSchedule);
    scheduleHeader->addWidget(addScheduleBtn);
    layout->addLayout(scheduleHeader);

    scheduleTableBox_ = makeBoxFrame(this);
    populateScheduleTable(scheduleTableBox_);
    layout->addWidget(scheduleTableBox_);

    layout->addWidget(makeSectionLabel(QStringLiteral("복약 이력 · 최근 4주"), tab));
    layout->addWidget(buildHistoryHeatmap());

    layout->addStretch();
    return tab;
}

QWidget* P3RecipientDetailDialog::buildTodayPillbox() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    static const QStringList kSlotNames = {QStringLiteral("아침"), QStringLiteral("점심"),
                                            QStringLiteral("저녁")};

    for (int i = 0; i < 3; ++i) {
        const MedicationScheduleItem* sched = nullptr;
        for (const auto& s : detail_.schedules) {
            if (s.slotNo == i + 1) { sched = &s; break; }
        }
        const PillboxStyle style = pillboxStyle(detail_.card.meds[i]);

        auto* box = new QFrame(row);
        box->setObjectName("p3PillboxCard");
        box->setStyleSheet(
            QStringLiteral("QFrame#p3PillboxCard{background:%1; border:%2; border-radius:8px;}")
                .arg(style.bg.name(), style.border));
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(8, 10, 8, 10);
        boxLayout->setSpacing(4);
        boxLayout->setAlignment(Qt::AlignHCenter);

        const QString timeText = sched ? sched->doseTime : Tokens::Format::EMPTY;
        auto* slotLabel = new QLabel(
            QStringLiteral("칸 %1 · %2 %3").arg(i + 1).arg(kSlotNames[i], timeText), box);
        slotLabel->setProperty("role", "secondary");
        slotLabel->setAlignment(Qt::AlignCenter);
        boxLayout->addWidget(slotLabel);

        auto* drugLabel = new QLabel(sched ? sched->drugName : Tokens::Format::EMPTY, box);
        QFont drugFont = drugLabel->font();
        drugFont.setPixelSize(Tokens::Font::BODY_S);
        drugFont.setWeight(QFont::Medium);
        drugLabel->setFont(drugFont);
        drugLabel->setAlignment(Qt::AlignCenter);
        boxLayout->addWidget(drugLabel);

        auto* statusLabel = new QLabel(style.text, box);
        statusLabel->setStyleSheet(
            QStringLiteral("font-size:11px; font-weight:500; color:%1;").arg(style.textColor.name()));
        statusLabel->setAlignment(Qt::AlignCenter);
        boxLayout->addWidget(statusLabel);

        layout->addWidget(box, 1);
    }

    return row;
}

void P3RecipientDetailDialog::populateScheduleTable(QWidget* box) {
    // populateGuardianTable()과 같은 이유로 QGridLayout 대신 행-구분선 방식을 쓴다.
    static const int kSlotW = 36, kTimeW = 60, kDaysW = 80;
    static const int kIconW = 24, kIconGap = 6;
    // 보기 모드는 편집+삭제 두 아이콘, 수정 모드는 저장+취소 두 아이콘이 나란히 붙는다 —
    // 헤더의 마지막 칸도 이 두 아이콘 폭에 맞춰야 우측 끝 정렬이 흐트러지지 않는다.
    static const int kActionsW = kIconW * 2 + kIconGap;

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

    for (int r = 0; r < detail_.schedules.size(); ++r) {
        auto* sep = new QFrame(box);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BORDER.name()));
        boxLayout->addWidget(sep);

        auto* stack = new QStackedWidget(box);

        // ---- 보기 모드 ----
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
        viewLayout->addSpacing(kIconGap);

        auto* deleteBtn = new QPushButton(viewPage);
        deleteBtn->setIcon(SimpleIcons::trash(Tokens::TEXT_MUTED, 12));
        deleteBtn->setProperty("variant", "ghost");
        deleteBtn->setProperty("size", "s");
        deleteBtn->setFixedSize(kIconW, kIconW);
        viewLayout->addWidget(deleteBtn);
        connect(deleteBtn, &QPushButton::clicked, this, [this, r] {
            const QString drugName = detail_.schedules[r].drugName;
            const QString question =
                QStringLiteral("%1 스케줄을 삭제할까요?").arg(drugName.isEmpty() ? QStringLiteral("이") : drugName);
            if (QMessageBox::question(this, QStringLiteral("스케줄 삭제"), question,
                                       QMessageBox::Yes | QMessageBox::No,
                                       QMessageBox::No) == QMessageBox::Yes) {
                removeSchedule(r);
            }
        });
        stack->addWidget(viewPage);  // index 0

        // ---- 수정 모드 — 클릭했을 때만 QLineEdit으로 바뀐다 ----
        auto* editPage = new QWidget(stack);
        auto* editLayout = new QHBoxLayout(editPage);
        editLayout->setContentsMargins(10, 7, 10, 7);
        editLayout->setSpacing(8);

        // 칸(약통 칸통) 번호도 수정 가능 — 저장 시 다른 행과 중복되면 경고하고 막는다(아래 saveBtn).
        auto* slotEdit = new QLineEdit(QString::number(detail_.schedules[r].slotNo), editPage);
        slotEdit->setValidator(new QIntValidator(1, 99, slotEdit));
        slotEdit->setFixedWidth(kSlotW);
        editLayout->addWidget(slotEdit);

        auto* drugEdit = new QLineEdit(editPage);
        editLayout->addWidget(drugEdit, 1);
        auto* timeEdit = new QLineEdit(editPage);
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

        stack->addWidget(editPage);  // index 1

        connect(editBtn, &QPushButton::clicked, this,
                [this, r, stack, slotEdit, drugEdit, timeEdit, daysEdit] {
                    const MedicationScheduleItem& ss = detail_.schedules[r];
                    slotEdit->setText(QString::number(ss.slotNo));
                    drugEdit->setText(ss.drugName);
                    timeEdit->setText(ss.doseTime);
                    daysEdit->setText(ss.daysLabel);
                    stack->setCurrentIndex(1);
                });

        connect(saveBtn, &QPushButton::clicked, this,
                [this, r, stack, slotEdit, drugEdit, timeEdit, daysEdit, refreshViewLabels] {
                    const QString drugName = drugEdit->text().trimmed();
                    if (drugName.isEmpty()) return;

                    bool slotOk = false;
                    const int slotNo = slotEdit->text().trimmed().toInt(&slotOk);
                    if (!slotOk || slotNo <= 0) {
                        QMessageBox::warning(this, QStringLiteral("칸 번호 오류"),
                                              QStringLiteral("칸 번호는 1 이상의 숫자로 입력해야 합니다."));
                        return;
                    }
                    for (int i = 0; i < detail_.schedules.size(); ++i) {
                        if (i != r && detail_.schedules[i].slotNo == slotNo) {
                            QMessageBox::warning(
                                this, QStringLiteral("칸 번호 중복"),
                                QStringLiteral("이미 %1번 칸을 사용 중인 스케줄이 있습니다.\n다른 칸 번호를 입력하세요.")
                                    .arg(slotNo));
                            return;
                        }
                    }

                    detail_.schedules[r].slotNo = slotNo;
                    detail_.schedules[r].drugName = drugName;
                    detail_.schedules[r].doseTime = timeEdit->text().trimmed();
                    detail_.schedules[r].daysLabel = daysEdit->text().trimmed();
                    refreshViewLabels();
                    stack->setCurrentIndex(0);
                });

        // 방금 "스케줄 추가"로 만든 빈 행에서 취소를 누르면 빈 행을 남기지 않고 통째로
        // 지운다 — populateGuardianTable()의 cancelBtn과 같은 방침.
        connect(cancelBtn, &QPushButton::clicked, this, [this, r, stack] {
            if (detail_.schedules[r].drugName.isEmpty()) {
                detail_.schedules.removeAt(r);
                refreshScheduleTable();
            } else {
                stack->setCurrentIndex(0);
            }
        });

        if (r == autoEditScheduleIndex_) {
            autoEditScheduleIndex_ = -1;
            drugEdit->setText(QString());
            timeEdit->setText(QString());
            daysEdit->setText(QString());
            stack->setCurrentIndex(1);
            drugEdit->setFocus();
        }

        boxLayout->addWidget(stack);
    }
}

void P3RecipientDetailDialog::refreshScheduleTable() {
    if (!scheduleTableBox_) return;
    if (QLayout* old = scheduleTableBox_->layout()) {
        QLayoutItem* item;
        while ((item = old->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        delete old;
    }
    populateScheduleTable(scheduleTableBox_);
}

void P3RecipientDetailDialog::addSchedule() {
    // 칸 번호를 매번 1로 고정(target3.png)→"가장 적게 쓰인 칸" 선택(target4.png)까지
    // 고쳐봤지만, 1·2·3이 이미 다 한 번씩 쓰인 뒤로는 "적게 쓰인 칸"이 매번 1→2→3→1…로
    // 계속 순환해 결국 "1·2·3만 반복된다"는 지적을 또 받았다. 실제로 원하는 동작은:
    // 아침·점심·저녁(1~3) 중 아직 비어 있는 칸이 있으면 그 칸부터 채우고, 셋 다 이미
    // 쓰였으면(빈 칸이 없으면) 더 이상 재사용하지 말고 계속 다음 번호로 늘어나야 한다.
    bool used[4] = {false, false, false, false};  // 인덱스 1~3만 사용
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
    if (nextSlot == 0) nextSlot = maxSlot + 1;  // 1~3이 이미 다 찼으면 그 다음 번호로 계속 늘어난다

    MedicationScheduleItem fresh;
    fresh.slotNo = nextSlot;
    detail_.schedules.append(fresh);
    autoEditScheduleIndex_ = detail_.schedules.size() - 1;
    refreshScheduleTable();
}

void P3RecipientDetailDialog::removeSchedule(int index) {
    if (index < 0 || index >= detail_.schedules.size()) return;
    detail_.schedules.removeAt(index);
    refreshScheduleTable();
}

QWidget* P3RecipientDetailDialog::buildHistoryHeatmap() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* gridCol = new QWidget(row);
    auto* grid = new QGridLayout(gridCol);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(3);

    static const QStringList kDayNames = {QStringLiteral("월"), QStringLiteral("화"), QStringLiteral("수"),
                                           QStringLiteral("목"), QStringLiteral("금"), QStringLiteral("토"),
                                           QStringLiteral("일")};
    for (int c = 0; c < 7; ++c) {
        auto* label = new QLabel(kDayNames[c], gridCol);
        label->setProperty("role", "caption");
        label->setAlignment(Qt::AlignCenter);
        grid->addWidget(label, 0, c);
    }

    int missedCount = 0;
    int scheduledCount = 0;
    double adherenceSum = 0.0;
    bool todayInProgress = false;
    const QDate today = QDate::currentDate();

    for (int i = 0; i < detail_.history.size(); ++i) {
        const MedicationDayRecord& rec = detail_.history[i];
        const int gridRow = 1 + i / 7;
        const int gridCol2 = i % 7;

        auto* cell = new QFrame(gridCol);
        cell->setFixedHeight(16);
        QString bg;
        QString border = QStringLiteral("none");
        if (rec.adherence < 0.0) {
            bg = Tokens::BG_SUNKEN.name();
        } else if (rec.adherence >= 1.0) {
            bg = Tokens::OK.main.name();
            scheduledCount++;
            adherenceSum += rec.adherence;
        } else if (rec.adherence > 0.0) {
            bg = Tokens::ATTENTION.main.name();
            scheduledCount++;
            adherenceSum += rec.adherence;
        } else {
            bg = Tokens::EMERGENCY.main.name();
            missedCount++;
            scheduledCount++;
        }
        cell->setStyleSheet(QStringLiteral("background:%1; border-radius:3px; border:%2;").arg(bg, border));
        if (rec.date == today && rec.adherence >= 0.0) todayInProgress = true;
        grid->addWidget(cell, gridRow, gridCol2);
    }

    layout->addWidget(gridCol, 1);

    auto* statBox = new QFrame(row);
    statBox->setObjectName("p3StatBox");
    statBox->setFixedWidth(140);
    statBox->setStyleSheet(
        QStringLiteral("QFrame#p3StatBox{background:%1; border-radius:8px;}").arg(Tokens::BG_BASE.name()));
    auto* statLayout = new QVBoxLayout(statBox);
    statLayout->setContentsMargins(12, 10, 12, 10);
    statLayout->setSpacing(2);

    auto* statTitle = new QLabel(QStringLiteral("4주 준수율"), statBox);
    statTitle->setProperty("role", "secondary");
    statLayout->addWidget(statTitle);

    const int pct = scheduledCount > 0 ? qRound(adherenceSum / scheduledCount * 100.0) : 0;
    auto* statValue = new QLabel(QStringLiteral("%1%").arg(pct), statBox);
    QFont valueFont = statValue->font();
    valueFont.setPixelSize(Tokens::Font::H2);
    valueFont.setWeight(QFont::Medium);
    statValue->setFont(valueFont);
    statLayout->addWidget(statValue);

    const QString noteText = todayInProgress
                                  ? QStringLiteral("미수령 %1회 · 오늘 진행 중").arg(missedCount)
                                  : QStringLiteral("미수령 %1회").arg(missedCount);
    auto* statNote = new QLabel(noteText, statBox);
    statNote->setProperty("role", "muted");
    statNote->setWordWrap(true);
    statLayout->addWidget(statNote);

    layout->addWidget(statBox);
    return row;
}

QWidget* P3RecipientDetailDialog::buildFooter() {
    auto* footer = new QFrame(this);
    footer->setObjectName("modalFooter");
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    auto* deactivateBtn = new QPushButton(QStringLiteral("대상자 비활성화"), footer);
    deactivateBtn->setProperty("variant", "secondary");
    deactivateBtn->setProperty("tone", "danger");
    connect(deactivateBtn, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("대상자 비활성화")); });
    layout->addWidget(deactivateBtn);

    layout->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("취소"), footer);
    cancelBtn->setProperty("variant", "secondary");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancelBtn);

    auto* saveBtn = new QPushButton(QStringLiteral("저장"), footer);
    saveBtn->setProperty("variant", "primary");
    connect(saveBtn, &QPushButton::clicked, this, &P3RecipientDetailDialog::handleSaveClicked);
    layout->addWidget(saveBtn);

    return footer;
}

void P3RecipientDetailDialog::handleSaveClicked() {
    const QString name = nameEdit_->text().trimmed();
    const QString address = addressEdit_->text().trimmed();
    if (name.isEmpty() || address.isEmpty()) return;

    // 이름·주소만 상위 카드에 실제로 반영된다. 생년·성별·지병·메모와 보호자·복약 스케줄은
    // 저장 API가 없어 이 다이얼로그 세션 안에서만 유효하다(클래스 주석 참고).
    emit basicInfoSaved(name, address);
    accept();
}

void P3RecipientDetailDialog::showComingSoon(const QString& feature) {
    QMessageBox::information(this, QStringLiteral("CareGuard"),
                              QStringLiteral("추후 구현: %1").arg(feature));
}
