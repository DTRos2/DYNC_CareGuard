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
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPair>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include "dialogs/p3_recipient_create_dialog.h"
#include "dialogs/p3_recipient_detail_dialog.h"
#include "net/api_client.h"
#include "net/ws_client.h"
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

// server/app/schemas.py::RecipientItem.level 대응. "offline"은 여기 없다 — 서버는 로봇
// 연결 상태를 모르고(WS robot_status로만 판정), care_event 기반 위험도만 내려준다.
Tokens::Level riskLevelFromCode(const QString& code) {
    if (code == QStringLiteral("emergency")) return Tokens::Level::Emergency;
    if (code == QStringLiteral("attention")) return Tokens::Level::Attention;
    return Tokens::Level::Normal;
}

// ApiClient::RecipientInfo::medsToday(오늘 요일에 스케줄된 슬롯만 담긴 목록)를 카드의
// 고정 3칸 배열로 펼친다. 스케줄이 없는 슬롯은 meds[]의 기본값(Pending)을 그대로 둔다 —
// "예정"과 "스케줄 없음"을 시각적으로 구분하는 상태가 없어서(§P3RecipientPage 논의) 빈
// 슬롯은 지금은 Pending과 같은 모양으로 보인다.
void applyMedsToday(const QVector<ApiClient::MedicationTodayInfo>& medsToday, MedSlotState (&meds)[3]) {
    meds[0] = meds[1] = meds[2] = MedSlotState::Pending;
    for (const auto& m : medsToday) {
        if (m.slotNo < 1 || m.slotNo > 3) continue;
        MedSlotState state = MedSlotState::Pending;
        if (m.state == QStringLiteral("done")) state = MedSlotState::Done;
        else if (m.state == QStringLiteral("missed")) state = MedSlotState::Missed;
        meds[m.slotNo - 1] = state;
    }
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

// §7-2 상태별 퀵액션 ("지금 할 일이 첫 버튼"). NORMAL은 카드 클릭 자체가 상세 진입이라
// 중복되는 "상세 보기" 버튼을 두지 않는다(호출부 rebuildGrid()에서 이 함수 자체를 건너뜀).
// OFFLINE도 버튼이 없다 — "재연결 시도"는 로봇이 서버로 다이얼하는 구조라 대시보드가
// 재연결을 강제로 트리거할 방법이 없고, 대신 서버가 push하는 robot_status를 구독해
// 로봇이 실제로 다시 붙으면 자동으로 NORMAL로 바뀐다(handleWsMessage() 참고).
// EMERGENCY의 "이벤트 확인"(P2로 이동 + 필터)과 ATTENTION의 "복약 재안내"(POST /missions로
// 실제 mission 생성 + 로봇 다운링크 전송)만 실동작이고, "보호자 연락"은 실제 연락 수단이
// 없어 제거했다.
QWidget* makeCardActions(const RecipientCard& rc, QWidget* parent,
                          const std::function<void(const QString&)>& onEventCheck,
                          const std::function<void(const QString&, const QString&, int)>& onMedReannounce) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    switch (rc.level) {
        case Tokens::Level::Emergency: {
            auto* btn = makeActionButton(QStringLiteral("이벤트 확인"), "danger", row);
            QObject::connect(btn, &QPushButton::clicked, row,
                              [onEventCheck, name = rc.name] { onEventCheck(name); });
            layout->addWidget(btn);
            break;
        }
        case Tokens::Level::Attention: {
            // meds[0..2]가 slot_no 1~2~3 순서(applyMedsToday 참고) — 그중 첫 번째
            // Missed를 "지금 재안내할 슬롯"으로 삼는다. 하나만 미수령이면 그게 곧
            // 정답이고, 여러 개면 가장 이른 슬롯(대개 아침)부터 처리한다는 뜻이다.
            int missedSlotNo = 0;
            for (int i = 0; i < 3; ++i) {
                if (rc.meds[i] == MedSlotState::Missed) {
                    missedSlotNo = i + 1;
                    break;
                }
            }
            auto* btn = makeActionButton(QStringLiteral("복약 재안내"), "primary", row);
            QObject::connect(
                btn, &QPushButton::clicked, row,
                [onMedReannounce, robotCode = rc.robotCode, name = rc.name, missedSlotNo] {
                    onMedReannounce(robotCode, name, missedSlotNo);
                });
            layout->addWidget(btn);
            break;
        }
        case Tokens::Level::Offline:
        case Tokens::Level::Normal:
            break;
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

    static const GuardianContact kEmergencyRow{0, QStringLiteral("119 · 112"), QString(), QStringLiteral("—"),
                                                true, true};

    if (card.code == QStringLiteral("C-001")) {
        d.gender = QStringLiteral("여");
        d.birthYear = 1945;
        d.diseases = QStringLiteral("고혈압 · 관절염");
        d.note = QStringLiteral("거동이 느려 순찰 응답 대기 시간을 여유 있게 설정함");
        d.guardians = {
            {1, QStringLiteral("김민재"), QStringLiteral("아들"), QStringLiteral("010-2222-3333"),
             true, false},
            {2, QStringLiteral("이복지"), QStringLiteral("복지사"), QStringLiteral("010-4444-5555"),
             true, false},
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
             true, false},
            {2, QStringLiteral("김복지"), QStringLiteral("복지사"), QStringLiteral("010-9876-5432"),
             true, false},
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
             true, false},
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
             true, false},
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

QString normalizeDoseTime(const QString& raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) return QString();

    if (trimmed.contains(QChar(':'))) {
        const QStringList parts = trimmed.split(QChar(':'));
        if (parts.size() != 2) return QString();
        bool hourOk = false, minOk = false;
        const int hour = parts[0].trimmed().toInt(&hourOk);
        const int minute = parts[1].trimmed().toInt(&minOk);
        if (!hourOk || !minOk) return QString();
        const QTime t(hour, minute);
        return t.isValid() ? t.toString(QStringLiteral("HH:mm")) : QString();
    }

    // 콜론 없이 숫자만 입력한 경우 — 자릿수로 시:분을 추정한다.
    // "9"/"09" → 09:00, "930" → 09:30, "0930" → 09:30.
    for (const QChar& c : trimmed) {
        if (!c.isDigit()) return QString();
    }
    int hour = 0, minute = 0;
    switch (trimmed.size()) {
        case 1:
        case 2:
            hour = trimmed.toInt();
            break;
        case 3:
            hour = trimmed.left(1).toInt();
            minute = trimmed.right(2).toInt();
            break;
        case 4:
            hour = trimmed.left(2).toInt();
            minute = trimmed.right(2).toInt();
            break;
        default:
            return QString();
    }
    const QTime t(hour, minute);
    return t.isValid() ? t.toString(QStringLiteral("HH:mm")) : QString();
}

QString formatGuardianPhone(const QString& raw) {
    QString digits;
    for (const QChar& c : raw) {
        if (c.isDigit()) digits += c;
    }
    if (digits.isEmpty()) return QString();

    // 서울(02)은 지역번호 2자리, 그 외(휴대폰 010~019·그 외 지역번호)는 3자리로 본다.
    const bool isSeoul = digits.startsWith(QStringLiteral("02"));
    const int prefixLen = isSeoul ? 2 : 3;
    if (digits.size() <= prefixLen) return digits;

    const QString prefix = digits.left(prefixLen);
    const QString rest = digits.mid(prefixLen);
    if (rest.size() <= 4) return prefix + QChar('-') + rest;

    // 중간 그룹을 4자리까지 먼저 채우고 그 이후는 끝 그룹에 이어 붙인다 — 타이핑 중 이미
    // 놓인 숫자가 그룹 사이를 오가며 자리가 안 바뀌게 하기 위한 선택(입력 중에도 실시간으로
    // 하이픈을 넣어달라는 요청 — 가장 흔한 휴대폰 11자리(010-XXXX-XXXX)와 정확히 맞아떨어짐).
    return prefix + QChar('-') + rest.left(4) + QChar('-') + rest.mid(4);
}

void installLivePhoneFormatting(QLineEdit* phoneEdit) {
    // phoneEdit을 그 자신의 커넥션 컨텍스트로 써서(캡처하는 것도 phoneEdit 자신뿐) phoneEdit이
    // 파괴되면 커넥션도 같이 자동 해제된다 — 호출부(다이얼로그)를 캡처할 필요가 없다.
    QObject::connect(phoneEdit, &QLineEdit::textEdited, phoneEdit, [phoneEdit] {
        const QString before = phoneEdit->text();
        const int posBefore = phoneEdit->cursorPosition();
        // 재포맷 후에도 "숫자 기준으로 몇 번째 자리에 커서가 있었는지"를 그대로 유지한다 —
        // 하이픈이 새로 끼어들어도 커서가 뒤로 밀리거나 앞으로 튀지 않게 하기 위함.
        int digitsBeforeCursor = 0;
        for (int i = 0; i < posBefore && i < before.size(); ++i) {
            if (before[i].isDigit()) ++digitsBeforeCursor;
        }
        const QString formatted = formatGuardianPhone(before);
        if (formatted == before) return;
        phoneEdit->setText(formatted);
        int newPos = 0, seenDigits = 0;
        while (newPos < formatted.size() && seenDigits < digitsBeforeCursor) {
            if (formatted[newPos].isDigit()) ++seenDigits;
            ++newPos;
        }
        phoneEdit->setCursorPosition(newPos);
    });
}

P3RecipientPage::P3RecipientPage(QWidget* parent) : QWidget(parent) {
    apiClient_ = new ApiClient(this);
    // GET /recipients가 비동기로 도착하기 전까지는 화면이 비어 보이므로, 응답 오기 전
    // 첫 rebuildGrid()는 mock으로 그려두고 loadRecipientsFromServer()가 도착하는 대로 덮어쓴다.
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
    loadRecipientsFromServer();
}

void P3RecipientPage::setWsClient(WsClient* client) {
    // robot_status(로봇 연결/해제) push를 구독해 OFFLINE 카드를 자동으로 갱신한다.
    wsClient_ = client;
    connect(wsClient_, &WsClient::messageReceived, this, &P3RecipientPage::handleWsMessage);
}

void P3RecipientPage::setGuardianUserId(int userId) {
    if (guardianUserId_ == userId) return;
    guardianUserId_ = userId;
    loadRecipientsFromServer();
}

void P3RecipientPage::loadRecipientsFromServer() {
    apiClient_->fetchRecipients([this](bool ok, const QVector<ApiClient::RecipientInfo>& items) {
        if (!ok) {
            // 서버 미기동 등으로 실패해도 화면은 mock으로 계속 동작해야 한다 — 이미
            // 생성자에서 mockRecipients()를 채워뒀으니 여기선 아무것도 안 해도 된다.
            return;
        }
        QVector<RecipientCard> cards;
        QStringList names;
        for (const auto& info : items) {
            RecipientCard card;
            card.id = info.id;
            card.code = info.code;
            card.name = info.name;
            card.robotCode = info.robotCode.isEmpty() ? QStringLiteral("미배정") : info.robotCode;
            card.address = info.homeAddress;
            card.operatorId = info.operatorId;
            card.operatorName = info.operatorName;
            // 로봇 연결 상태(Offline)는 여기 없다 — 페이지 진입 직후엔 항상 온라인으로 가정하고
            // riskLevel을 그대로 표시 레벨로 쓴다. 이후 WS robot_status가 오면 handleWsMessage()가
            // riskLevel을 보존한 채 Offline과 조합한다.
            card.riskLevel = riskLevelFromCode(info.level);
            card.level = card.riskLevel;
            card.statusSummary = info.statusSummary;
            applyMedsToday(info.medsToday, card.meds);
            cards.append(card);
            names.append(card.name);
        }
        recipients_ = cards;
        rebuildGrid();
        // guardianUserId_==0(admin/operator)이면 빈 목록 = "필터 없음"으로 보낸다. names는
        // 이미 fetchRecipients가 guardian_user_id로 걸러온 결과라 그대로 넘겨도 맞지만,
        // P2EventPage 쪽 필터 의미(빈 목록=전체 허용)를 명확히 하기 위해 구분해서 보낸다.
        emit guardianRecipientNamesChanged(guardianUserId_ > 0 ? names : QStringList());
    }, guardianUserId_);
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
    // 카드 버튼들은 클릭 콜스택 안에서 rebuildGrid()를 재호출하지 않으므로(신호 emit이나
    // 비동기 API 콜백뿐) 즉시 delete로 유령 위젯이 다음 grab()/repaint 전에 남지 않게 한다.
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

        auto* operatorLabel = new QLabel(
            QStringLiteral("관리 담당자: %1")
                .arg(rc.operatorName.isEmpty() ? QStringLiteral("미배정") : rc.operatorName),
            card);
        operatorLabel->setProperty("role", "muted");
        cardLayout->addWidget(operatorLabel);

        auto* summaryLabel = new QLabel(rc.statusSummary, card);
        summaryLabel->setProperty("role", summaryRole(rc.level));
        summaryLabel->setWordWrap(true);
        cardLayout->addWidget(summaryLabel);

        cardLayout->addWidget(makeMedRow(rc, card));
        if (rc.level != Tokens::Level::Normal && rc.level != Tokens::Level::Offline) {
            cardLayout->addWidget(makeCardActions(
                rc, card, [this](const QString& name) { emit eventCheckRequested(name); },
                [this](const QString& robotCode, const QString& name, int slotNo) {
                    requestMedicationReannounce(robotCode, name, slotNo);
                }));
        }

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

void P3RecipientPage::requestMedicationReannounce(const QString& robotCode, const QString& recipientName,
                                                   int slotNo) {
    // medication_scheduler가 없어(스케줄러는 patrol_schedule만 폴링) 스케줄을 거치지 않고
    // 버튼 클릭 즉시 POST /missions로 mission을 만들고 로봇에 바로 다운링크를 보낸다.
    // slotNo는 makeCardActions가 이미 카드에 표시돼 있던 미수령 슬롯에서 찾아온 값 —
    // 0이면(이론상 안 나와야 함, Attention인데 meds에 Missed가 없는 방어적 경우) 서버가
    // 그대로 0을 실어 보내고, 로봇(downlink_node._start_medication)이 "유효한 slot_no
    // 없음"으로 보고 무시한다.
    apiClient_->startMedicationMission(
        robotCode, slotNo,
        [this, recipientName](bool ok, const ApiClient::MissionInfo& mission, const QString& err) {
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("복약 재안내 실패"),
                                      QStringLiteral("서버에 명령을 전달하지 못했습니다: %1").arg(err));
                return;
            }
            if (!mission.robotConnected) {
                QMessageBox::information(
                    this, QStringLiteral("복약 재안내"),
                    QStringLiteral("%1 대상자 미션(#%2)은 기록했지만, 로봇이 아직 연결되어 있지 "
                                    "않습니다.")
                        .arg(recipientName)
                        .arg(mission.id));
                return;
            }
            QMessageBox::information(
                this, QStringLiteral("복약 재안내"),
                QStringLiteral("%1 대상자에게 복약 재안내 미션(#%2)을 로봇에 전송했습니다.")
                    .arg(recipientName)
                    .arg(mission.id));
        });
}

void P3RecipientPage::handleWsMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("recipient_changed") || type == QStringLiteral("robot_recipient_changed")) {
        // 다른 대시보드가 대상자 정보(이름·담당 로봇·보호자 등)를 바꿨거나, 로봇
        // 등록 모달에서 담당 대상자를 재배정했을 수 있다 — 부분 필드만 오므로
        // patrol_schedules_changed와 같은 이유로 GET /recipients를 통째로 다시 받는다.
        loadRecipientsFromServer();
        // recipient_changed에만 recipient_id가 실려 온다(robot_recipient_changed는
        // robot_id만 있음) — 그 대상자의 상세 모달이 지금 열려 있으면 같이 갱신한다.
        // id가 없거나(robot_recipient_changed) 다른 대상자면 refreshOpenDetailDialog()
        // 안의 recipientId<=0/openDetailRecipientId_ 불일치 가드에서 조용히 무시된다.
        refreshOpenDetailDialog(message.value(QStringLiteral("recipient_id")).toInt());
        return;
    }

    if (type != QStringLiteral("robot_status")) return;

    const QString robotCode = message.value(QStringLiteral("robot_code")).toString();
    const bool online = message.value(QStringLiteral("online")).toBool();

    bool changed = false;
    for (RecipientCard& rc : recipients_) {
        if (rc.robotCode != robotCode) continue;
        // 재연결(온라인)은 OFFLINE 카드를 riskLevel(care_event 기반 위험도)로 되돌린다 —
        // 예전엔 무조건 NORMAL로 되돌려서, 실제로는 미수령 등으로 주의·응급 상태였던 대상자도
        // 재연결 순간 카드가 정상으로 잘못 표시됐다. 연결 해제(오프라인)는 로봇 상태를 확인할
        // 수 없다는 뜻이라 현재 레벨과 무관하게 OFFLINE으로 덮어쓴다.
        if (online && rc.level == Tokens::Level::Offline) {
            rc.level = rc.riskLevel;
            changed = true;
        } else if (!online && rc.level != Tokens::Level::Offline) {
            rc.level = Tokens::Level::Offline;
            changed = true;
        }
    }
    if (changed) rebuildGrid();
}

namespace {

QString genderLabelFromCode(const QString& code) {
    if (code == QStringLiteral("male")) return QStringLiteral("남");
    if (code == QStringLiteral("female")) return QStringLiteral("여");
    if (code == QStringLiteral("other")) return QStringLiteral("기타");
    return QString();
}

// P3RecipientCreateDialog::genderCombo_는 "남"/"여"만 필수 항목으로 강제하므로("선택"이면
// 등록 버튼이 비활성) 여기 들어오는 값은 항상 이 둘 중 하나다.
QString genderCodeFromLabel(const QString& label) {
    if (label == QStringLiteral("남")) return QStringLiteral("male");
    if (label == QStringLiteral("여")) return QStringLiteral("female");
    return QString();
}

QString daysLabelFromBitmask(int mask) {
    if (mask >= 127) return QStringLiteral("매일");
    static const QVector<QPair<int, QChar>> kBits = {
        {1, QChar(u'월')}, {2, QChar(u'화')}, {4, QChar(u'수')},  {8, QChar(u'목')},
        {16, QChar(u'금')}, {32, QChar(u'토')}, {64, QChar(u'일')}};
    QString label;
    for (const auto& bit : kBits) {
        if (mask & bit.first) {
            if (!label.isEmpty()) label += QChar(u'·');
            label += bit.second;
        }
    }
    return label.isEmpty() ? QStringLiteral("매일") : label;
}

// 복약 스케줄의 "요일" 칸은 QLineEdit 자유 입력이라 "매일"/"월·수·금" 같은 임의 문자열이
// 들어온다 — days_of_week 비트마스크가 표현 못 하는 패턴(예: "격일")은 인식된 요일만
// 반영하고, 아예 못 알아보면 매일(127)로 보수적으로 채운다(스케줄 자체를 버리는 것보다 낫다).
int bitmaskFromDaysLabel(const QString& label) {
    if (label.trimmed().isEmpty() || label.contains(QStringLiteral("매일"))) return 127;
    static const QVector<QPair<QChar, int>> kBits = {
        {QChar(u'월'), 1}, {QChar(u'화'), 2}, {QChar(u'수'), 4},  {QChar(u'목'), 8},
        {QChar(u'금'), 16}, {QChar(u'토'), 32}, {QChar(u'일'), 64}};
    int mask = 0;
    for (const auto& bit : kBits) {
        if (label.contains(bit.first)) mask |= bit.second;
    }
    return mask == 0 ? 127 : mask;
}

RecipientDetail recipientDetailFromApi(const ApiClient::RecipientDetailInfo& info) {
    RecipientDetail d;
    d.card.id = info.recipient.id;
    d.card.code = info.recipient.code;
    d.card.name = info.recipient.name;
    d.card.robotCode =
        info.recipient.robotCode.isEmpty() ? QStringLiteral("미배정") : info.recipient.robotCode;
    d.card.address = info.recipient.homeAddress;
    d.card.operatorId = info.recipient.operatorId;
    d.card.operatorName = info.recipient.operatorName;
    d.card.riskLevel = riskLevelFromCode(info.recipient.level);
    d.card.level = d.card.riskLevel;  // 로봇 오프라인 여부는 openDetail()이 카드 최신값으로 덮어씀
    d.card.statusSummary = info.recipient.statusSummary;
    applyMedsToday(info.recipient.medsToday, d.card.meds);

    d.gender = genderLabelFromCode(info.recipient.gender);
    d.birthYear = info.recipient.birthDate.left(4).toInt();
    d.phone = info.recipient.phone;
    d.diseases = info.recipient.diseases;
    d.note = info.recipient.note;

    int priority = 1;
    for (const auto& g : info.guardians) {
        GuardianContact gc;
        gc.priority = priority++;
        gc.name = g.name;
        gc.relation = g.relation;
        gc.phone = g.phone;
        gc.notifyEnabled = g.notifyEnabled;
        gc.isEmergencyFixed = false;
        gc.id = g.id;
        gc.email = g.email;
        d.guardians.append(gc);
    }
    // 119·112 고정 행은 DB에 없는 화면 전용 표시 — mockRecipientDetail()/생성 모달과 동일하게 덧붙인다.
    d.guardians.append(
        GuardianContact{0, QStringLiteral("119 · 112"), QString(), QStringLiteral("—"), true, true});

    for (const auto& s : info.schedules) {
        MedicationScheduleItem item;
        item.slotNo = s.slotNo;
        item.drugName = s.drugName;
        item.doseTime = s.doseTime;
        item.daysLabel = daysLabelFromBitmask(s.daysOfWeek);
        d.schedules.append(item);
    }

    for (const auto& h : info.history) {
        MedicationDayRecord rec;
        rec.date = QDate::fromString(h.date, QStringLiteral("yyyy-MM-dd"));
        rec.adherence = h.adherence;
        d.history.append(rec);
    }

    return d;
}

}  // namespace

void P3RecipientPage::refreshOpenDetailDialog(int recipientId) {
    if (!openDetailDialog_ || recipientId <= 0 || openDetailRecipientId_ != recipientId) return;

    apiClient_->fetchRecipientDetail(
        recipientId, [this, recipientId](bool ok, const ApiClient::RecipientDetailInfo& info, const QString&) {
            if (!ok) return;
            // 응답을 기다리는 동안 모달이 닫혔거나(openDetailDialog_ == nullptr) 다른 대상자의
            // 모달로 바뀌었을 수 있어(openDetailRecipientId_ 변경) 다시 확인한다.
            if (openDetailDialog_ && openDetailRecipientId_ == recipientId) {
                RecipientDetail fresh = recipientDetailFromApi(info);
                // level은 서버가 계산한 riskLevel일 뿐 OFFLINE을 모른다(recipientDetailFromApi
                // 주석 참고) — openDetail()과 같은 이유로, 로봇 연결 여부는 이 창이 WS
                // robot_status로 직접 덮어써둔 recipients_ 쪽 값을 그대로 가져와야 한다.
                for (const auto& card : recipients_) {
                    if (card.id == recipientId) {
                        fresh.card.level = card.level;
                        break;
                    }
                }
                openDetailDialog_->applyRemoteUpdate(fresh);
            }
        });
}

namespace {
// robotCode를 이미 담당 중인 대상자 이름을 recipients_에서 찾는다 — 로봇 콤보에 "배정 중"
// 표시를 채우고, 그 로봇을 고르면 재배정 확인 모달을 띄울지 판단하는 데 쓴다(요청,
// 2026-09-15). selfRobotCode(상세 모달에서 지금 수정 중인 그 대상자 자신의 현재 로봇)와
// 같으면 재배정이 아니므로 빈 문자열을 돌려준다 — 등록 모달은 selfRobotCode를 빈 문자열로
// 넘겨 이 예외가 걸리지 않게 한다.
QString findRobotAssignee(const QVector<RecipientCard>& recipients, const QString& robotCode,
                           const QString& selfRobotCode) {
    if (robotCode.isEmpty() || robotCode == selfRobotCode) return QString();
    for (const auto& rec : recipients) {
        if (rec.robotCode == robotCode) return rec.name;
    }
    return QString();
}
}  // namespace

void P3RecipientPage::openDetail(int recipientIndex) {
    if (recipientIndex < 0 || recipientIndex >= recipients_.size()) return;
    const RecipientCard card = recipients_[recipientIndex];

    // "관리 담당자"·"담당 로봇"·"담당 맵" 콤보 후보는 모달을 열 때마다 새로 받아온다(대상자
    // 목록처럼 페이지 진입 시 한 번만 캐싱하지 않음 — P7 계정관리·맵 배정이 그 사이 바뀔 수
    // 있다). registerRecipient()와 같은 이유·같은 3단 중첩(operators -> robots -> maps).
    apiClient_->fetchOperators([this, recipientIndex, card](bool, const QVector<ApiClient::OperatorInfo>& items) {
        QVector<OperatorOption> operators;
        for (const auto& o : items) operators.append(OperatorOption{o.id, o.name});

        apiClient_->fetchRobots([this, recipientIndex, card, operators](
                                     bool, const QVector<ApiClient::RobotInfo>& robotItems) {
            QVector<RobotOption> robots;
            for (const auto& r : robotItems) {
                robots.append(RobotOption{r.id, r.code, findRobotAssignee(recipients_, r.code, card.robotCode)});
            }

            apiClient_->fetchActiveMaps([this, recipientIndex, card, operators, robots](
                                             bool, const QVector<ApiClient::MapInfo>& mapItems) {
                QVector<MapOption> maps;
                for (const auto& m : mapItems) {
                    // 맵의 담당 대상자는 서버가 이미 recipient_name으로 내려준다 — 로봇과 달리
                    // recipients_를 다시 훑을 필요 없이, 자기 자신 로봇의 맵인지만 걸러내면 된다.
                    const QString assignee = (m.robotCode == card.robotCode) ? QString() : m.recipientName;
                    maps.append(MapOption{m.id, m.robotCode, m.name, assignee});
                }

        auto openWithDetail = [this, recipientIndex, operators, robots, maps](RecipientDetail detail) {
            if (recipientIndex >= recipients_.size()) return;  // 조회 대기 중 목록이 바뀌었으면 방어
            detail.card = recipients_[recipientIndex];  // 이름·주소 등 카드 쪽 최신값으로 덮어쓴다

            auto* dialog = new P3RecipientDetailDialog(detail, operators, robots, maps, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);

            // 열려 있는 동안 recipient_changed가 오면(handleWsMessage) 이 다이얼로그를 다시
            // 찾아 갱신할 수 있도록 기록해둔다. 닫히면(WA_DeleteOnClose) 되돌려서 이미 닫힌
            // 모달을 갱신하려 들지 않게 한다.
            openDetailDialog_ = dialog;
            openDetailRecipientId_ = recipients_[recipientIndex].id;
            connect(dialog, &QObject::destroyed, this, [this] {
                openDetailDialog_ = nullptr;
                openDetailRecipientId_ = -1;
            });

            connect(dialog, &P3RecipientDetailDialog::saveRequested, this,
                    [this, recipientIndex](const RecipientDetail& saved) {
                        if (recipientIndex < recipients_.size()) {
                            // 서버 응답을 기다리는 동안에도 화면에 바로 반영한다 — 정확한 값은
                            // saveRecipientUpdate()의 콜백이 도착하는 대로 한 번 더 덮어쓴다.
                            recipients_[recipientIndex].name = saved.card.name;
                            recipients_[recipientIndex].address = saved.card.address;
                            recipients_[recipientIndex].operatorId = saved.card.operatorId;
                            recipients_[recipientIndex].operatorName = saved.card.operatorName;
                            rebuildGrid();
                        }
                        saveRecipientUpdate(saved);
                    });
            connect(dialog, &P3RecipientDetailDialog::deactivateRequested, this,
                    [this](int recipientId) { deactivateRecipient(recipientId); });
            dialog->exec();
        };

        if (card.id <= 0) {
            // 서버 연결 실패로 남아있는 mock 카드 — DB에 없으니 조회할 수 없다.
            openWithDetail(mockRecipientDetail(card));
            return;
        }

        apiClient_->fetchRecipientDetail(
            card.id,
            [this, openWithDetail, card](bool ok, const ApiClient::RecipientDetailInfo& info, const QString&) {
                if (!ok) {
                    QMessageBox::warning(this, QStringLiteral("조회 실패"),
                                          QStringLiteral("대상자 상세 정보를 불러오지 못했습니다."));
                    openWithDetail(mockRecipientDetail(card));
                    return;
                }
                openWithDetail(recipientDetailFromApi(info));
            });
            });
        });
    });
}

namespace {
// RecipientDetail(다이얼로그가 들고 있는 화면 모델)을 서버 요청 파라미터로 변환한다.
// 등록(POST)·수정(PUT) 둘 다 이 함수를 쓴다. 복약 스케줄 칸 번호·시각이 유효하지 않으면
// errorOut을 채우고 false를 반환한다 — 호출부가 그 메시지로 QMessageBox를 띄우고 요청을 취소한다.
bool buildRecipientParams(const RecipientDetail& detail, ApiClient::RecipientCreateParams* outParams,
                           QString* errorOut) {
    ApiClient::RecipientCreateParams params;
    params.name = detail.card.name;
    params.gender = genderCodeFromLabel(detail.gender);
    if (detail.birthYear > 0) {
        params.birthDate = QStringLiteral("%1-01-01").arg(detail.birthYear, 4, 10, QChar('0'));
    }
    params.phone = detail.phone;
    params.homeAddress = detail.card.address;
    params.diseases = detail.diseases;
    params.note = detail.note;
    if (detail.card.robotCode != QStringLiteral("미배정")) params.robotCode = detail.card.robotCode;
    params.operatorId = detail.card.operatorId;

    for (const auto& g : detail.guardians) {
        if (g.isEmergencyFixed) continue;  // 119·112는 화면 전용 표시라 서버에 안 보낸다
        ApiClient::GuardianCreateParams gp;
        gp.id = g.id;  // 0이면 서버가 신규로 처리 — GuardianContact::id 주석 참고
        gp.name = g.name;
        gp.phone = g.phone;
        gp.email = g.email;
        gp.relation = g.relation;
        gp.priority = g.priority;
        gp.notifyEnabled = g.notifyEnabled;
        params.guardians.append(gp);
    }

    for (const auto& s : detail.schedules) {
        if (s.slotNo < 1 || s.slotNo > 3) {
            *errorOut = QStringLiteral(
                            "복약 스케줄 칸 번호는 1~3만 가능합니다(약통 3칸 구성). 칸 번호를 고쳐 다시 시도해주세요.");
            return false;
        }
        // 정상 흐름에선 다이얼로그의 스케줄 저장(💾) 버튼이 이미 normalizeDoseTime()으로
        // 정규화해뒀어야 한다 — 여기서 다시 한번 돌리는 건 그 방어선이 뚫렸을 때(예: 값을
        // 안 건드린 기본값)를 위한 이중 확인이다.
        const QString normalizedTime = normalizeDoseTime(s.doseTime);
        if (normalizedTime.isEmpty()) {
            *errorOut = QStringLiteral("'%1' 복용 시각(%2)을 알아볼 수 없습니다. 다시 시도해주세요.")
                            .arg(s.drugName.isEmpty() ? QStringLiteral("복약") : s.drugName, s.doseTime);
            return false;
        }
        ApiClient::MedicationScheduleCreateParams sp;
        sp.slotNo = s.slotNo;
        sp.drugName = s.drugName;
        sp.doseTime = normalizedTime;
        sp.daysOfWeek = bitmaskFromDaysLabel(s.daysLabel);
        params.schedules.append(sp);
    }

    *outParams = params;
    return true;
}
}  // namespace

void P3RecipientPage::registerRecipient() {
    // 서버가 실제 채번을 하므로 이건 사용자에게 보여주는 미리보기일 뿐이다 — 등록 완료 후
    // 카드는 서버가 돌려준 진짜 code로 다시 채워진다(아래 createRecipient 콜백).
    const QString nextCodePreview = QStringLiteral("C-%1").arg(recipients_.size() + 1, 3, 10, QChar('0'));

    // "관리 담당자"·"담당 로봇"·"담당 맵" 콤보 후보를 모달을 열기 전에 미리 받아온다 —
    // openDetail()과 같은 이유. 로봇 목록(GET /robots)은 등록 해제 안 된 로봇 전부를,
    // 맵 목록(GET /maps/active)은 로봇에 실제로 배정된 활성 맵만 내려준다(예전엔 이 목록
    // 자체가 없어 R-01~R-04 4대만 하드코딩돼 있었다, 2026-09-15 리포트).
    apiClient_->fetchOperators([this, nextCodePreview](bool, const QVector<ApiClient::OperatorInfo>& items) {
        QVector<OperatorOption> operators;
        for (const auto& o : items) operators.append(OperatorOption{o.id, o.name});

        apiClient_->fetchRobots([this, nextCodePreview, operators](
                                     bool, const QVector<ApiClient::RobotInfo>& robotItems) {
            QVector<RobotOption> robots;
            // 등록 모달은 "지금 수정 중인 자기 자신"이 없으므로 selfRobotCode를 빈 문자열로 넘긴다
            // — recipients_에 이미 그 robotCode를 담당하는 대상자가 있으면 전부 "배정 중"으로 잡힌다.
            for (const auto& r : robotItems) {
                robots.append(RobotOption{r.id, r.code, findRobotAssignee(recipients_, r.code, QString())});
            }

            apiClient_->fetchActiveMaps([this, nextCodePreview, operators, robots](
                                             bool, const QVector<ApiClient::MapInfo>& mapItems) {
                QVector<MapOption> maps;
                for (const auto& m : mapItems) maps.append(MapOption{m.id, m.robotCode, m.name, m.recipientName});

                P3RecipientCreateDialog dialog(nextCodePreview, operators, robots, maps, this);
                if (dialog.exec() != QDialog::Accepted) return;

                const RecipientDetail detail = dialog.resultDetail();

                ApiClient::RecipientCreateParams params;
                QString error;
                if (!buildRecipientParams(detail, &params, &error)) {
                    QMessageBox::warning(this, QStringLiteral("등록 실패"), error);
                    return;
                }

                apiClient_->createRecipient(
                    params, [this](bool ok, const ApiClient::RecipientDetailInfo& info, const QString& err) {
                        if (!ok) {
                            QMessageBox::warning(this, QStringLiteral("등록 실패"),
                                                  QStringLiteral("대상자 등록에 실패했습니다: %1").arg(err));
                            return;
                        }
                        recipients_.append(recipientDetailFromApi(info).card);
                        rebuildGrid();
                    });
            });
        });
    });
}

void P3RecipientPage::saveRecipientUpdate(const RecipientDetail& detail) {
    const int recipientId = detail.card.id;
    if (recipientId <= 0) return;  // 서버 연결 실패로 남은 mock 카드 — 저장할 원본이 없다

    ApiClient::RecipientCreateParams params;
    QString error;
    if (!buildRecipientParams(detail, &params, &error)) {
        QMessageBox::warning(this, QStringLiteral("저장 실패"), error);
        return;
    }

    apiClient_->updateRecipient(
        recipientId, params,
        [this, recipientId](bool ok, const ApiClient::RecipientDetailInfo& info, const QString& err) {
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("저장 실패"),
                                      QStringLiteral("대상자 정보를 저장하지 못했습니다: %1").arg(err));
                return;
            }
            for (auto& card : recipients_) {
                if (card.id != recipientId) continue;
                const RecipientCard updated = recipientDetailFromApi(info).card;
                card.name = updated.name;
                card.address = updated.address;
                card.robotCode = updated.robotCode;
                card.operatorId = updated.operatorId;
                card.operatorName = updated.operatorName;
                card.riskLevel = updated.riskLevel;
                // 로봇이 지금 Offline으로 표시 중이면 그 상태를 지운다 — 저장은 위험도만 새로
                // 반영하고, 연결 여부는 WS robot_status만 바꿀 수 있다(handleWsMessage 참고).
                if (card.level != Tokens::Level::Offline) card.level = updated.riskLevel;
                card.statusSummary = updated.statusSummary;
                for (int i = 0; i < 3; ++i) card.meds[i] = updated.meds[i];
                break;
            }
            rebuildGrid();
            emit recipientsUpdated();
        });
}

void P3RecipientPage::deactivateRecipient(int recipientId) {
    if (recipientId <= 0) return;  // 서버 연결 실패로 남은 mock 카드 — 처리할 원본이 없다

    apiClient_->deactivateRecipient(recipientId, [this, recipientId](bool ok, const QString& err) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("비활성화 실패"),
                                  QStringLiteral("대상자를 비활성화하지 못했습니다: %1").arg(err));
            return;
        }
        for (int i = 0; i < recipients_.size(); ++i) {
            if (recipients_[i].id == recipientId) {
                recipients_.remove(i);
                break;
            }
        }
        rebuildGrid();
    });
}

#include "p3_recipient_page.moc"
