#include "p3_recipient_page.h"

#include <algorithm>
#include <functional>

#include <QButtonGroup>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include "dialogs/p3_recipient_create_dialog.h"
#include "dialogs/p3_recipient_detail_dialog.h"
#include "theme/tokens.h"
#include "widgets/live_search_line_edit.h"
#include "widgets/simple_icons.h"

namespace {

// CSS grid의 auto-fill과 같은 공식 (P4 영상 카드 그리드와 동일 패턴).
constexpr int kMinColumns = 1;
constexpr int kCardMinWidth = 320;  // §5-2 대상자 카드 최소 폭

void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

// 카드 자신을 클릭하면 상세 모달을 연다 (버튼 등 자식 위젯 위 클릭은 그쪽이 먼저 소비하므로
// 여기까지 안 온다). P4 video_page.cpp의 ClickableCard와 동일한 발상, 별도 파일로 안 뺀 이유도
// 같다(§관례 — 페이지마다 필요한 만큼만 로컬로 둔다).
class ClickableCard : public QFrame {
    Q_OBJECT
public:
    using QFrame::QFrame;

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) emit clicked();
        QFrame::mousePressEvent(event);
    }
};

// §7-1 상태 판정 우선순위: EMERGENCY > ATTENTION > OFFLINE > NORMAL.
int riskRank(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return 0;
        case Tokens::Level::Attention: return 1;
        case Tokens::Level::Offline:   return 2;
        case Tokens::Level::Normal:    return 3;
    }
    return 3;
}

const char* statusProp(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return Tokens::Prop::EMERGENCY;
        case Tokens::Level::Attention: return Tokens::Prop::ATTENTION;
        case Tokens::Level::Offline:   return Tokens::Prop::OFFLINE;
        case Tokens::Level::Normal:    return Tokens::Prop::NORMAL;
    }
    return Tokens::Prop::NORMAL;
}

// 상태 요약 한 줄(§5-2, 심박 블록을 대체)의 텍스트 색. OFFLINE은 recipientCard QSS가
// 하위 QLabel을 전부 회색으로 덮어써서 role 지정과 무관하게 흐려진다.
const char* summaryRole(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return "danger";
        case Tokens::Level::Attention: return "warning";
        default: return "secondary";
    }
}

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
            // "정상"은 §7-1 상태 판정과는 별개인 중립 표기라 badge 표준 레벨엔 없다(§11-3: 정상에
            // 초록 배경 금지). OFFLINE 토큰과 배경/텍스트 색이 같아 재사용한다
            // (p4_video_page.cpp가 "순찰" 태그에 쓰는 것과 동일한 방식).
            badge->setText(QStringLiteral("정상"));
            badge->setProperty("level", "offline");
            break;
    }
    return badge;
}

QLabel* makeMedSlot(MedSlotState state, QWidget* parent) {
    auto* slot = new QLabel(parent);
    slot->setObjectName("medSlot");
    slot->setFixedSize(22, 22);
    slot->setAlignment(Qt::AlignCenter);
    switch (state) {
        case MedSlotState::Done:
            slot->setText(QStringLiteral("✓"));  // ✓
            slot->setProperty("state", "done");
            break;
        case MedSlotState::InProgress:
            // ⏳(U+23F3)는 컬러 이모지 글리프라 QSS color를 무시하고 자체 색(황토색)으로
            // 렌더링된다 — INFO 파란 테두리와 안 어울려 일반 도형 문자로 대체.
            slot->setText(QStringLiteral("◐"));
            slot->setProperty("state", "inProgress");
            break;
        case MedSlotState::Missed:
            slot->setText(QStringLiteral("✕"));  // ✕
            slot->setProperty("state", "missed");
            break;
        case MedSlotState::Pending:
            slot->setProperty("state", "pending");
            break;
    }
    return slot;
}

QWidget* makeMedRow(const RecipientCard& rc, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* label = new QLabel(QStringLiteral("복약"), row);
    label->setProperty("role", "secondary");
    layout->addWidget(label);

    for (int i = 0; i < 3; ++i) {
        layout->addWidget(makeMedSlot(rc.meds[i], row));
    }

    auto* timesLabel = new QLabel(QStringLiteral("아침·점심·저녁"), row);
    timesLabel->setProperty("role", "muted");
    layout->addWidget(timesLabel);
    layout->addStretch();

    return row;
}

QPushButton* makeActionButton(const QString& text, const QString& variant, QWidget* parent) {
    auto* btn = new QPushButton(text, parent);
    btn->setProperty("variant", variant);
    btn->setProperty("size", "s");
    return btn;
}

// §7-2 상태별 퀵액션 ("지금 할 일이 첫 버튼"). 조치 기능은 아직 없어 전부 안내 메시지로 대체한다.
QWidget* makeCardActions(const RecipientCard& rc, QWidget* parent,
                          const std::function<void(const QString&)>& comingSoon) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    QPushButton* primary = nullptr;
    switch (rc.level) {
        case Tokens::Level::Emergency:
            primary = makeActionButton(QStringLiteral("이벤트 확인"), "danger", row);
            break;
        case Tokens::Level::Attention:
            primary = makeActionButton(QStringLiteral("복약 재안내"), "primary", row);
            break;
        case Tokens::Level::Offline:
            primary = makeActionButton(QStringLiteral("재연결 시도"), "primary", row);
            break;
        case Tokens::Level::Normal:
            primary = makeActionButton(QStringLiteral("상세 보기"), "secondary", row);
            break;
    }
    QObject::connect(primary, &QPushButton::clicked, row,
                      [comingSoon, text = primary->text()] { comingSoon(text); });
    layout->addWidget(primary);

    if (rc.level != Tokens::Level::Normal) {
        auto* contactBtn = makeActionButton(QStringLiteral("보호자 연락"), "secondary", row);
        QObject::connect(contactBtn, &QPushButton::clicked, row,
                          [comingSoon] { comingSoon(QStringLiteral("보호자 연락")); });
        layout->addWidget(contactBtn);
    }

    layout->addStretch();
    return row;
}

}  // namespace

QVector<RecipientCard> mockRecipients() {
    QVector<RecipientCard> rows;
    rows.append({QStringLiteral("C-001"), QStringLiteral("김순자"), QStringLiteral("R-01"),
                 QStringLiteral("행복빌라 302"), Tokens::Level::Emergency,
                 QStringLiteral("안부 질문 2회 무응답 → 로봇 출동 · 음성 확인 대기"),
                 {MedSlotState::Done, MedSlotState::InProgress, MedSlotState::Pending}});
    rows.append({QStringLiteral("C-002"), QStringLiteral("박영호"), QStringLiteral("R-02"),
                 QStringLiteral("은빛아파트 101"), Tokens::Level::Attention,
                 QStringLiteral("아침 약 미수령 · 보호자 문자 발송됨 12:41"),
                 {MedSlotState::Missed, MedSlotState::Pending, MedSlotState::Pending}});
    rows.append({QStringLiteral("C-003"), QStringLiteral("이말순"), QStringLiteral("R-03"),
                 QStringLiteral("솔마을 7호"), Tokens::Level::Normal,
                 QStringLiteral("순찰 정상 · 마지막 확인 14:20"),
                 {MedSlotState::Done, MedSlotState::Done, MedSlotState::Pending}});
    rows.append({QStringLiteral("C-004"), QStringLiteral("최금례"), QStringLiteral("R-04"),
                 QStringLiteral("동산주택 2층"), Tokens::Level::Offline,
                 QStringLiteral("로봇 통신 두절 · 마지막 확인 13:58 · 30초 주기 재연결 시도 중"),
                 {MedSlotState::Pending, MedSlotState::Pending, MedSlotState::Pending}});
    return rows;
}

namespace {

// 최근 28일(월요일 시작) 수령률 mock. seed로 대상자별 패턴을 살짝 다르게 준다 —
// 실 데이터가 아니므로 결정론적으로 생성해 스크린샷/테스트가 매번 같은 그림이 되게 한다.
QVector<MedicationDayRecord> mockHistory(int seed) {
    QVector<MedicationDayRecord> days;
    const QDate today = QDate::currentDate();
    // 4행×7열(월~일) 그리드가 항상 온전한 주 단위가 되도록, 이번 주 월요일에서 3주를 더
    // 뺀 날짜부터 시작한다. 이번 주의 아직 안 지난 요일은 아래서 -1(빈 칸)로 채워진다.
    const QDate currentWeekMonday = today.addDays(-(today.dayOfWeek() - 1));
    const QDate start = currentWeekMonday.addDays(-21);

    for (int i = 0; i < 28; ++i) {
        const QDate date = start.addDays(i);
        MedicationDayRecord rec;
        rec.date = date;
        if (date > today) {
            rec.adherence = -1.0;  // 미래 — 빈 칸
        } else {
            const int bucket = (date.dayOfYear() + seed) % 11;
            if (bucket == 0) rec.adherence = 0.0;        // 완전 미수령
            else if (bucket == 1) rec.adherence = 0.5;   // 일부만 수령
            else rec.adherence = 1.0;                    // 정상 수령
        }
        days.append(rec);
    }
    return days;
}

}  // namespace

RecipientDetail mockRecipientDetail(const RecipientCard& card) {
    RecipientDetail d;
    d.card = card;

    static const GuardianContact kEmergencyRow{
        0, QStringLiteral("119 · 112"), QString(), QStringLiteral("—"),
        QStringLiteral("모의 신고 전용"), true};

    if (card.code == QStringLiteral("C-001")) {
        d.gender = QStringLiteral("여");
        d.birthYear = 1945;
        d.diseases = QStringLiteral("고혈압 · 관절염");
        d.note = QStringLiteral("거동이 느려 순찰 응답 대기 시간을 여유 있게 설정함");
        d.guardians = {
            {1, QStringLiteral("김민재"), QStringLiteral("아들"), QStringLiteral("010-2222-3333"),
             QStringLiteral("문자 + 전화"), false},
            {2, QStringLiteral("이복지"), QStringLiteral("복지사"), QStringLiteral("010-4444-5555"),
             QStringLiteral("문자"), false},
            kEmergencyRow,
        };
        d.schedules = {
            {1, QStringLiteral("혈압약"), QStringLiteral("08:00"), QStringLiteral("매일")},
            {2, QStringLiteral("관절약"), QStringLiteral("13:00"), QStringLiteral("매일")},
            {3, QStringLiteral("혈압약"), QStringLiteral("19:00"), QStringLiteral("매일")},
        };
        d.mapWaypointCount = 4;
    } else if (card.code == QStringLiteral("C-002")) {
        d.gender = QStringLiteral("남");
        d.birthYear = 1948;
        d.diseases = QStringLiteral("고혈압");
        d.note = QStringLiteral("청력 저하로 음성 안내 음량 크게 설정 필요");
        d.guardians = {
            {1, QStringLiteral("박민수"), QStringLiteral("아들"), QStringLiteral("010-1234-5678"),
             QStringLiteral("문자 + 전화"), false},
            {2, QStringLiteral("김복지"), QStringLiteral("복지사"), QStringLiteral("010-9876-5432"),
             QStringLiteral("문자"), false},
            kEmergencyRow,
        };
        d.schedules = {
            {1, QStringLiteral("혈압약 (암로디핀)"), QStringLiteral("08:00"), QStringLiteral("매일")},
            {2, QStringLiteral("소화제"), QStringLiteral("12:30"), QStringLiteral("월·수·금")},
            {3, QStringLiteral("혈압약 (암로디핀)"), QStringLiteral("18:30"), QStringLiteral("매일")},
        };
        d.mapWaypointCount = 5;
    } else if (card.code == QStringLiteral("C-003")) {
        d.gender = QStringLiteral("여");
        d.birthYear = 1950;
        d.diseases = QStringLiteral("퇴행성 관절염");
        d.note = QStringLiteral("특이사항 없음 · 순찰 협조적");
        d.guardians = {
            {1, QStringLiteral("이수진"), QStringLiteral("딸"), QStringLiteral("010-3333-7777"),
             QStringLiteral("문자 + 전화"), false},
            kEmergencyRow,
        };
        d.schedules = {
            {1, QStringLiteral("알러지약"), QStringLiteral("08:00"), QStringLiteral("매일")},
            {2, QStringLiteral("소화제"), QStringLiteral("12:30"), QStringLiteral("월·수·금")},
            {3, QStringLiteral("관절약"), QStringLiteral("18:30"), QStringLiteral("매일")},
        };
        d.mapWaypointCount = 6;
    } else {  // C-004
        d.gender = QStringLiteral("여");
        d.birthYear = 1952;
        d.diseases = QStringLiteral("고혈압 · 당뇨");
        d.note = QStringLiteral("통신 불안정 지역 — 재연결 지연 시 유선 연락 우선");
        d.guardians = {
            {1, QStringLiteral("최영수"), QStringLiteral("아들"), QStringLiteral("010-5555-1111"),
             QStringLiteral("문자 + 전화"), false},
            kEmergencyRow,
        };
        d.schedules = {
            {1, QStringLiteral("혈압약"), QStringLiteral("09:00"), QStringLiteral("매일")},
            {2, QStringLiteral("당뇨약"), QStringLiteral("13:00"), QStringLiteral("매일")},
            {3, QStringLiteral("혈압약"), QStringLiteral("19:00"), QStringLiteral("매일")},
        };
        d.mapWaypointCount = 3;
    }

    d.history = mockHistory(card.code.right(1).toInt());

    // 오늘 아침 칸이 미수령이면 안내 배너를 채운다 — 카드 요약 텍스트를 그대로 재사용해
    // 배너와 카드가 서로 다른 말을 하지 않게 한다.
    if (card.meds[0] == MedSlotState::Missed) {
        d.medAlertText = card.statusSummary;
    }

    return d;
}

P3RecipientPage::P3RecipientPage(QWidget* parent) : QWidget(parent) {
    recipients_ = mockRecipients();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    layout->addWidget(buildHeaderRow());

    gridPage_ = new QWidget(this);
    grid_ = new QGridLayout(gridPage_);
    grid_->setSpacing(Tokens::Layout::CARD_GAP);
    grid_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(gridPage_, /*stretch=*/1);

    pulseTimer_ = new QTimer(this);
    connect(pulseTimer_, &QTimer::timeout, this, [this] {
        pulseOn_ = !pulseOn_;
        for (auto* card : emergencyCards_) {
            card->setProperty("pulse", pulseOn_ ? QVariant() : QStringLiteral("off"));
            repolish(card);
        }
    });
    pulseTimer_->start(Tokens::Motion::PULSE_PERIOD_MS);

    rebuildGrid();
}

QWidget* P3RecipientPage::buildHeaderRow() {
    auto* bar = new QWidget(this);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("대상자 관리"), bar);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    searchEdit_ = new LiveSearchLineEdit(bar);
    searchEdit_->setPlaceholderText(QStringLiteral("이름 · ID 검색"));
    searchEdit_->setFixedWidth(200);
    connect(searchEdit_, &LiveSearchLineEdit::liveTextChanged, this,
            [this](const QString&) { rebuildGrid(); });
    layout->addWidget(searchEdit_);

    layout->addStretch();

    // 정렬 세그먼트. 기본값 "위험 순" (§6 P3 구현 주의점).
    auto* sortSeg = new QWidget(bar);
    auto* sortSegLayout = new QHBoxLayout(sortSeg);
    sortSegLayout->setContentsMargins(0, 0, 0, 0);
    sortSegLayout->setSpacing(0);

    auto* riskBtn = new QPushButton(QStringLiteral("위험 순"), sortSeg);
    auto* nameBtn = new QPushButton(QStringLiteral("이름순"), sortSeg);
    auto* robotBtn = new QPushButton(QStringLiteral("로봇별"), sortSeg);
    riskBtn->setProperty("pos", "first");
    robotBtn->setProperty("pos", "last");

    sortGroup_ = new QButtonGroup(this);
    sortGroup_->setExclusive(true);
    for (auto* btn : {riskBtn, nameBtn, robotBtn}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        sortSegLayout->addWidget(btn);
        sortGroup_->addButton(btn);
    }
    riskBtn->setChecked(true);
    layout->addWidget(sortSeg);

    connect(riskBtn, &QPushButton::clicked, this, [this] { sortMode_ = SortMode::Risk; rebuildGrid(); });
    connect(nameBtn, &QPushButton::clicked, this, [this] { sortMode_ = SortMode::Name; rebuildGrid(); });
    connect(robotBtn, &QPushButton::clicked, this, [this] { sortMode_ = SortMode::Robot; rebuildGrid(); });

    auto* registerBtn = new QPushButton(SimpleIcons::plus(Tokens::TEXT_INVERSE, 12),
                                         QStringLiteral("대상자 등록"), bar);
    registerBtn->setProperty("variant", "primary");
    registerBtn->setProperty("size", "s");
    connect(registerBtn, &QPushButton::clicked, this, &P3RecipientPage::registerRecipient);
    layout->addWidget(registerBtn);

    return bar;
}

QVector<int> P3RecipientPage::visibleIndexesSorted() const {
    const QString keyword = searchEdit_->liveText().trimmed();

    QVector<int> indexes;
    for (int i = 0; i < recipients_.size(); ++i) {
        const RecipientCard& rc = recipients_[i];
        if (keyword.isEmpty() || rc.name.contains(keyword, Qt::CaseInsensitive) ||
            rc.code.contains(keyword, Qt::CaseInsensitive)) {
            indexes.append(i);
        }
    }

    std::stable_sort(indexes.begin(), indexes.end(), [this](int a, int b) {
        const RecipientCard& ra = recipients_[a];
        const RecipientCard& rb = recipients_[b];
        switch (sortMode_) {
            case SortMode::Risk:
                if (riskRank(ra.level) != riskRank(rb.level)) {
                    return riskRank(ra.level) < riskRank(rb.level);
                }
                return ra.name.localeAwareCompare(rb.name) < 0;
            case SortMode::Name:
                return ra.name.localeAwareCompare(rb.name) < 0;
            case SortMode::Robot:
                if (ra.robotCode != rb.robotCode) {
                    return ra.robotCode.localeAwareCompare(rb.robotCode) < 0;
                }
                return ra.name.localeAwareCompare(rb.name) < 0;
        }
        return false;
    });
    return indexes;
}

void P3RecipientPage::rebuildGrid() {
    // takeAt()으로 먼저 위젯을 떼어 delete해야 한다 — 레이아웃만 delete하면 이전 카드가
    // gridPage_의 자식으로 남아 다음 그리드 위에 겹쳐 보인다. P4 rebuildCardGrid()는 카드 자신의
    // 클릭 콜스택 안에서 재호출될 수 있어 deleteLater()를 쓰지만(§use-after-free 회피), 여기
    // 카드 버튼은 showComingSoon()만 호출해 재진입 위험이 없으므로 즉시 delete로 유령 위젯이
    // 다음 grab()/repaint 전에 남지 않게 한다.
    QLayoutItem* item;
    while ((item = grid_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    delete grid_;
    grid_ = new QGridLayout(gridPage_);
    grid_->setSpacing(Tokens::Layout::CARD_GAP);
    grid_->setContentsMargins(0, 0, 0, 0);
    emergencyCards_.clear();

    const QVector<int> order = visibleIndexesSorted();
    if (order.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("검색 결과가 없습니다"), gridPage_);
        empty->setProperty("role", "muted");
        empty->setAlignment(Qt::AlignCenter);
        grid_->addWidget(empty, 0, 0);
        return;
    }

    const bool grouping = (sortMode_ == SortMode::Robot);
    QString lastRobot;
    int row = 0;
    int col = 0;

    for (int idx : order) {
        const RecipientCard& rc = recipients_[idx];

        if (grouping && rc.robotCode != lastRobot) {
            if (col != 0) {
                row++;
                col = 0;
            }
            auto* header = new QLabel(
                QStringLiteral("%1 · %2").arg(rc.robotCode, rc.address), gridPage_);
            header->setProperty("role", "label");
            grid_->addWidget(header, row, 0, 1, columns_);
            row++;
            lastRobot = rc.robotCode;
        }

        auto* card = new ClickableCard(gridPage_);
        card->setObjectName("recipientCard");
        card->setProperty(Tokens::Prop::STATUS, statusProp(rc.level));
        card->setMinimumWidth(kCardMinWidth);
        card->setCursor(Qt::PointingHandCursor);
        connect(card, &ClickableCard::clicked, this, [this, idx] { openDetail(idx); });

        // QWidget은 QGraphicsEffect를 한 번에 하나만 가질 수 있어 오프라인은 그림자 대신
        // 불투명도 효과만 적용한다 (§5-2는 OFFLINE에 그림자를 요구하지 않는다).
        if (rc.level == Tokens::Level::Offline) {
            auto* opacity = new QGraphicsOpacityEffect(card);
            opacity->setOpacity(0.75);
            card->setGraphicsEffect(opacity);
        } else {
            auto* shadow = new QGraphicsDropShadowEffect(card);
            const Tokens::ShadowSpec& spec =
                rc.level == Tokens::Level::Emergency ? Tokens::SHADOW_MD : Tokens::SHADOW_SM;
            shadow->setBlurRadius(spec.blur);
            shadow->setOffset(0, spec.dy);
            shadow->setColor(spec.color);
            card->setGraphicsEffect(shadow);
        }

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                        Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
        cardLayout->setSpacing(8);

        auto* headRow = new QHBoxLayout();
        headRow->setSpacing(6);
        auto* nameLabel = new QLabel(rc.name, card);
        QFont nameFont = nameLabel->font();
        nameFont.setPixelSize(Tokens::Font::BODY);
        nameFont.setWeight(QFont::Medium);
        nameLabel->setFont(nameFont);
        headRow->addWidget(nameLabel);
        headRow->addWidget(makeStatusBadge(rc.level, card));
        headRow->addStretch();
        cardLayout->addLayout(headRow);

        auto* idLabel = new QLabel(
            QStringLiteral("%1 · %2 · %3").arg(rc.code, rc.robotCode, rc.address), card);
        idLabel->setProperty("role", "mono");
        cardLayout->addWidget(idLabel);

        auto* summaryLabel = new QLabel(rc.statusSummary, card);
        summaryLabel->setProperty("role", summaryRole(rc.level));
        summaryLabel->setWordWrap(true);
        cardLayout->addWidget(summaryLabel);

        cardLayout->addWidget(makeMedRow(rc, card));
        cardLayout->addWidget(
            makeCardActions(rc, card, [this](const QString& f) { showComingSoon(f); }));

        grid_->addWidget(card, row, col);
        if (rc.level == Tokens::Level::Emergency) {
            emergencyCards_.append(card);
        }

        col++;
        if (col >= columns_) {
            col = 0;
            row++;
        }
    }

    for (int c = 0; c < columns_; ++c) {
        grid_->setColumnStretch(c, 1);
    }
    grid_->setRowStretch(col == 0 ? row : row + 1, 1);
}

int P3RecipientPage::computeColumnCount() const {
    const int available = width() - 2 * Tokens::Layout::PAGE_PADDING;
    const int gap = Tokens::Layout::CARD_GAP;
    const int columns = (available + gap) / (kCardMinWidth + gap);
    return qMax(kMinColumns, columns);
}

void P3RecipientPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int newColumns = computeColumnCount();
    if (newColumns != columns_) {
        columns_ = newColumns;
        rebuildGrid();
    }
}

void P3RecipientPage::showComingSoon(const QString& feature) {
    QMessageBox::information(this, QStringLiteral("CareGuard"),
                              QStringLiteral("추후 구현: %1").arg(feature));
}

void P3RecipientPage::openDetail(int recipientIndex) {
    if (recipientIndex < 0 || recipientIndex >= recipients_.size()) return;

    const RecipientCard& card = recipients_[recipientIndex];
    // "대상자 등록"으로 방금 만든 대상자는 mockRecipientDetail()이 코드를 못 알아봐 엉뚱한
    // C-004 목데이터로 빠진다(그 함수의 else 분기) — customDetails_에 있으면 그걸 우선 쓰고,
    // 카드 쪽 정보(이름·주소 등 상단 편집으로 바뀔 수 있는 값)만 최신으로 덮어쓴다.
    RecipientDetail detail = customDetails_.contains(card.code) ? customDetails_[card.code]
                                                                 : mockRecipientDetail(card);
    detail.card = card;

    auto* dialog = new P3RecipientDetailDialog(detail, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &P3RecipientDetailDialog::basicInfoSaved, this,
            [this, recipientIndex](const QString& name, const QString& address) {
                if (recipientIndex >= recipients_.size()) return;  // 다이얼로그가 떠 있는 동안 목록이 안 바뀌므로 방어용
                recipients_[recipientIndex].name = name;
                recipients_[recipientIndex].address = address;
                const QString code = recipients_[recipientIndex].code;
                if (customDetails_.contains(code)) {
                    customDetails_[code].card.name = name;
                    customDetails_[code].card.address = address;
                }
                rebuildGrid();
            });
    dialog->exec();
}

void P3RecipientPage::registerRecipient() {
    const QString nextCode = QStringLiteral("C-%1").arg(recipients_.size() + 1, 3, 10, QChar('0'));
    P3RecipientCreateDialog dialog(nextCode, this);
    if (dialog.exec() != QDialog::Accepted) return;

    const RecipientDetail detail = dialog.resultDetail();
    recipients_.append(detail.card);
    customDetails_[detail.card.code] = detail;
    rebuildGrid();
}

#include "p3_recipient_page.moc"
