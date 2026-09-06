#include "p7_settings_page.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "dialogs/p7_account_dialog.h"
#include "dialogs/p7_robot_dialog.h"
#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {

QLabel* makeBadge(const char* level, const QString& text) {
    auto* badge = new QLabel(text);
    badge->setObjectName("badge");
    badge->setProperty("level", level);
    badge->setAlignment(Qt::AlignCenter);
    return badge;
}

// 노드 상태 점 (§P1 MedStepsWidget과 동일하게 QSS 밖에서 직접 그려야 하는 8px 원).
QLabel* statusDot(const QColor& color) {
    auto* dot = new QLabel();
    dot->setFixedSize(6, 6);
    dot->setStyleSheet(QStringLiteral("background:%1;border-radius:3px;").arg(color.name()));
    return dot;
}

QWidget* wrapCellWidget(QWidget* inner, const QColor& rowBg = Tokens::BG_SURFACE) {
    auto* wrap = new QWidget();
    wrap->setAutoFillBackground(true);
    QPalette pal = wrap->palette();
    pal.setColor(QPalette::Window, rowBg);
    wrap->setPalette(pal);

    auto* layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(inner);
    return wrap;
}

QLabel* linkText(const QString& text, const QColor& color = Tokens::ACCENT_HOVER) {
    auto* label = new QLabel(text);
    label->setStyleSheet(QStringLiteral("color:%1;font-weight:500;").arg(color.name()));
    label->setAlignment(Qt::AlignCenter);
    return label;
}

QFrame* card(QWidget* parent = nullptr) {
    auto* frame = new QFrame(parent);
    frame->setObjectName("card");
    return frame;
}

QLabel* sectionLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setProperty("role", "h3");
    return label;
}

// 로봇 관리 / 계정 관리 / 감사 로그 3탭에서 공통으로 쓰는 목록 헤더행(제목 + 등록/여백 버튼).
QWidget* buildListHeader(const QString& title, const QString& actionText = QString()) {
    auto* row = new QWidget();
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);

    auto* label = new QLabel(title, row);
    label->setProperty("role", "label");
    rowLayout->addWidget(label);
    rowLayout->addStretch();

    if (!actionText.isEmpty()) {
        auto* btn = new QPushButton(actionText, row);
        btn->setProperty("variant", "primary");
        rowLayout->addWidget(btn);
    }
    return row;
}

QLineEdit* thresholdField(const QString& value, int width = 44) {
    auto* edit = new QLineEdit(value);
    edit->setFixedWidth(width);
    edit->setAlignment(Qt::AlignRight);
    return edit;
}

QWidget* thresholdRow(const QString& label, QLineEdit* field, const QString& unit,
                       QLineEdit* field2 = nullptr) {
    auto* row = new QWidget();
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* labelWidget = new QLabel(label, row);
    labelWidget->setProperty("role", "secondary");
    layout->addWidget(labelWidget, /*stretch=*/1);
    layout->addWidget(field);
    if (field2 != nullptr) {
        auto* slash = new QLabel(QStringLiteral("/"), row);
        slash->setProperty("role", "muted");
        layout->addWidget(slash);
        layout->addWidget(field2);
    }
    auto* unitLabel = new QLabel(unit, row);
    unitLabel->setProperty("role", "muted");
    layout->addWidget(unitLabel);
    return row;
}

}  // namespace

void P7SettingsPage::setCurrentUserLoginId(const QString& loginId) {
    currentUserLoginId_ = loginId;
    if (accountTable_ != nullptr) refreshAccountTable();
}

P7SettingsPage::P7SettingsPage(QWidget* parent) : QWidget(parent) {
    robots_ = {
        {QStringLiteral("R-01"), QStringLiteral("192.168.0.11"), QStringLiteral("/limo_01"),
         QStringLiteral("행복빌라 302 · 김순자"), Tokens::Level::Normal, QStringLiteral("● 온라인"),
         QStringLiteral("6/6 정상"), QStringLiteral("1호기"), QStringLiteral("limo"), 41,
         QStringLiteral("김순자 (C-001)"), QStringLiteral("192.168.0.9"),
         QStringLiteral("최근 ping 응답 0.6s 전 · 세션 5d 1h")},
        {QStringLiteral("R-02"), QStringLiteral("192.168.0.12"), QStringLiteral("/limo_02"),
         QStringLiteral("은빛아파트 101 · 박영호"), Tokens::Level::Normal, QStringLiteral("● 온라인"),
         QStringLiteral("6/6 정상"), QStringLiteral("2호기"), QStringLiteral("limo"), 42,
         QStringLiteral("박영호 (C-002)"), QStringLiteral("192.168.0.9"),
         QStringLiteral("최근 ping 응답 0.5s 전 · 세션 4d 3h")},
        {QStringLiteral("R-03"), QStringLiteral("192.168.0.13"), QStringLiteral("/limo_03"),
         QStringLiteral("솔마을 7호 · 이말순"), Tokens::Level::Attention, QStringLiteral("● 온라인"),
         QStringLiteral("5/6 · voice 응답 지연"), QStringLiteral("3호기"), QStringLiteral("limo"), 43,
         QStringLiteral("이말순 (C-003)"), QStringLiteral("192.168.0.9"),
         QStringLiteral("최근 ping 응답 0.4s 전 · 세션 3d 2h")},
        {QStringLiteral("R-04"), QStringLiteral("192.168.0.14"), QStringLiteral("/limo_04"),
         QStringLiteral("동산주택 2층 · 최금례"), Tokens::Level::Offline, QStringLiteral("오프라인"),
         Tokens::Format::EMPTY, QStringLiteral("4호기"), QStringLiteral("limo"), 44,
         QStringLiteral("최금례 (C-004)"), QStringLiteral("192.168.0.9"), QString()},
    };

    r03Nodes_ = {
        {QStringLiteral("patrol_manager"), false, QStringLiteral("up 3d 2h")},
        {QStringLiteral("med_dispenser"), false, QStringLiteral("up 3d 2h")},
        {QStringLiteral("voice_interaction"), true, QStringLiteral("응답 지연 2.1s")},
        {QStringLiteral("wearable_bridge"), false, QStringLiteral("up 1d 6h")},
    };

    // 계정 목록은 하드코딩하지 않고 DB에서 불러온다 (fetchAccounts(), 생성자 마지막에서 호출).

    auditLog_ = {
        {QStringLiteral("08.26 14:30"), QStringLiteral("도희정"), QStringLiteral("영상 열람"),
         QStringLiteral("EV-260826-014 · 사유: 긴급 경보 초기 상황 확인"), true},
        {QStringLiteral("08.26 12:45"), QStringLiteral("강유나"), QStringLiteral("보호자 문자"),
         QStringLiteral("박영호 · 아침 약 미수령 알림 발송"), false},
        {QStringLiteral("08.25 16:20"), QStringLiteral("김예은"), QStringLiteral("이벤트 완료"),
         QStringLiteral("EV-260825-041 · 메모: 보호자 통화로 정상 확인"), true},
        {QStringLiteral("08.24 17:20"), QStringLiteral("admin"), QStringLiteral("맵 배포"),
         QStringLiteral("솔마을 7호 → R-03 · 웨이포인트 4개"), false},
    };

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("시스템 설정"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildRobotTab(), QStringLiteral("로봇 관리"));
    tabs->addTab(buildAccountTab(), QStringLiteral("계정 관리"));
    tabs->addTab(buildThresholdTab(), QStringLiteral("임계값 · 알림"));
    tabs->addTab(buildAuditTab(), QStringLiteral("감사 로그"));
    layout->addWidget(tabs);

    serverUrl_ = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_URL"), QStringLiteral("http://127.0.0.1:8000"));
    netManager_ = new QNetworkAccessManager(this);
    fetchAccounts();
    fetchRecipients();
}

QWidget* P7SettingsPage::buildRobotTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto* headerRow = buildListHeader(QStringLiteral("등록 로봇 %1").arg(robots_.size()),
                                       QStringLiteral("로봇 등록"));
    robotCountLabel_ = headerRow->findChild<QLabel*>();
    auto* headerBtn = headerRow->findChild<QPushButton*>();
    if (headerBtn != nullptr) {
        connect(headerBtn, &QPushButton::clicked, this, [this] { openRobotDialog(-1); });
    }
    layout->addWidget(headerRow);

    robotTable_ = new QTableWidget(tab);
    const QStringList headers = {QStringLiteral("로봇"),  QStringLiteral("IP"), QStringLiteral("네임스페이스"),
                                  QStringLiteral("맵 · 대상자"), QStringLiteral("상태"),
                                  QStringLiteral("노드"), QStringLiteral("액션")};
    robotTable_->setColumnCount(headers.size());
    robotTable_->setHorizontalHeaderLabels(headers);
    robotTable_->verticalHeader()->setVisible(false);
    robotTable_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H_COMPACT);
    robotTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    robotTable_->setSelectionMode(QAbstractItemView::NoSelection);
    robotTable_->setFocusPolicy(Qt::NoFocus);
    robotTable_->setShowGrid(false);
    robotTable_->setWordWrap(false);

    auto* header = robotTable_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Stretch);  // 맵 · 대상자 — 남는 폭 흡수
    robotTable_->setColumnWidth(0, 110);  // 로봇
    robotTable_->setColumnWidth(1, 175);  // IP
    robotTable_->setColumnWidth(2, 160);  // 네임스페이스
    robotTable_->setColumnWidth(4, 130);  // 상태
    robotTable_->setColumnWidth(5, 260);  // 노드
    robotTable_->setColumnWidth(6, 110);  // 액션

    robotTable_->setRowCount(static_cast<int>(robots_.size()));
    for (int r = 0; r < static_cast<int>(robots_.size()); ++r) {
        populateRobotRow(r);
    }
    layout->addWidget(robotTable_, /*stretch=*/1);

    auto* bottomRow = new QWidget(tab);
    auto* bottomLayout = new QHBoxLayout(bottomRow);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(Tokens::Layout::CARD_GAP);

    // 좌측: R-03 노드 상세
    auto* nodeCard = card(bottomRow);
    auto* nodeLayout = new QVBoxLayout(nodeCard);
    nodeLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                    Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    nodeLayout->setSpacing(4);

    auto* nodeTitleRow = new QWidget(nodeCard);
    auto* nodeTitleLayout = new QHBoxLayout(nodeTitleRow);
    nodeTitleLayout->setContentsMargins(0, 0, 0, 4);
    nodeTitleLayout->setSpacing(6);
    auto* nodeTitle = new QLabel(QStringLiteral("R-03 노드 상세"), nodeTitleRow);
    nodeTitle->setProperty("role", "label");
    nodeTitleLayout->addWidget(nodeTitle);
    auto* nodeHint = new QLabel(QStringLiteral("— 행 클릭 시 펼침"), nodeTitleRow);
    nodeHint->setProperty("role", "muted");
    nodeTitleLayout->addWidget(nodeHint);
    nodeTitleLayout->addStretch();
    nodeLayout->addWidget(nodeTitleRow);

    for (const auto& node : r03Nodes_) {
        auto* row = new QWidget(nodeCard);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 4, 0, 4);
        rowLayout->setSpacing(8);

        rowLayout->addWidget(statusDot(node.warn ? Tokens::EMERGENCY.main : Tokens::BORDER_STRONG));

        auto* name = new QLabel(node.name, row);
        name->setFont(QFont(Tokens::Font::MONO));
        rowLayout->addWidget(name);
        rowLayout->addStretch();

        auto* detail = new QLabel(node.detail, row);
        detail->setProperty("role", node.warn ? "warning" : "muted");
        rowLayout->addWidget(detail);

        if (node.warn) {
            // "size" 프로퍼티는 QWidget 내장 size와 이름이 겹쳐 QSS에 반영되지 않는다(§P1 커밋
            // 참고) — 여기서는 기본 크기 그대로 둔다.
            auto* restartBtn = new QPushButton(QStringLiteral("재시작"), row);
            restartBtn->setProperty("variant", "secondary");
            rowLayout->addWidget(restartBtn);
        }
        nodeLayout->addWidget(row);
    }
    nodeLayout->addStretch();
    bottomLayout->addWidget(nodeCard, /*stretch=*/6);

    // 우측: 통신 단절 관리
    auto* discCard = card(bottomRow);
    auto* discLayout = new QVBoxLayout(discCard);
    discLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                    Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    discLayout->setSpacing(8);

    auto* discTitle = new QLabel(QStringLiteral("통신 단절 관리"), discCard);
    discTitle->setProperty("role", "label");
    discLayout->addWidget(discTitle);

    auto* banner = new QFrame(discCard);
    banner->setObjectName("banner");
    banner->setProperty("level", Tokens::Prop::ATTENTION);
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(9, 7, 9, 7);
    bannerLayout->setSpacing(8);

    auto* bannerIcon = new QLabel(banner);
    bannerIcon->setPixmap(SimpleIcons::plugOff(Tokens::ATTENTION.main, 14).pixmap(14, 14));
    bannerLayout->addWidget(bannerIcon);

    auto* bannerTextCol = new QWidget(banner);
    auto* bannerTextLayout = new QVBoxLayout(bannerTextCol);
    bannerTextLayout->setContentsMargins(0, 0, 0, 0);
    bannerTextLayout->setSpacing(2);
    auto* bannerLine1 = new QLabel(QStringLiteral("R-04 · 마지막 동기화 13:58"), bannerTextCol);
    QFont line1Font = bannerLine1->font();
    line1Font.setWeight(QFont::DemiBold);
    bannerLine1->setFont(line1Font);
    bannerTextLayout->addWidget(bannerLine1);
    auto* bannerLine2 = new QLabel(QStringLiteral("로컬 큐 이벤트 7건 대기 · 30초 주기 재시도 중"), bannerTextCol);
    bannerLine2->setProperty("role", "secondary");
    bannerTextLayout->addWidget(bannerLine2);
    bannerLayout->addWidget(bannerTextCol, /*stretch=*/1);

    discLayout->addWidget(banner);

    auto* syncBtn = new QPushButton(SimpleIcons::refresh(Tokens::TEXT_PRIMARY, 12),
                                     QStringLiteral("재연결 후 일괄 동기화"), discCard);
    syncBtn->setProperty("variant", "secondary");
    syncBtn->setIconSize(QSize(12, 12));
    discLayout->addWidget(syncBtn);
    discLayout->addStretch();

    bottomLayout->addWidget(discCard, /*stretch=*/5);

    layout->addWidget(bottomRow, /*stretch=*/1);
    return tab;
}

QWidget* P7SettingsPage::buildAccountTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    const int activeCount = static_cast<int>(
        std::count_if(accounts_.begin(), accounts_.end(), [](const AccountRow& a) { return a.active; }));
    auto* headerRow = buildListHeader(
        QStringLiteral("계정 %1 · 활성 %2").arg(accounts_.size()).arg(activeCount),
        QStringLiteral("계정 등록"));
    accountCountLabel_ = headerRow->findChild<QLabel*>();
    auto* headerBtn = headerRow->findChild<QPushButton*>();
    if (headerBtn != nullptr) {
        connect(headerBtn, &QPushButton::clicked, this, [this] { openAccountDialog(-1); });
    }
    // 삭제는 관리자 권한 계정만 가능 — 실제 표시 여부는 계정 목록을 불러온 뒤
    // refreshAccountTable()에서 isCurrentUserAdmin()으로 판단해 갱신한다.
    deleteAccountBtn_ = new QPushButton(QStringLiteral("삭제"), headerRow);
    deleteAccountBtn_->setProperty("variant", "secondary");
    deleteAccountBtn_->setVisible(false);
    connect(deleteAccountBtn_, &QPushButton::clicked, this, &P7SettingsPage::handleDeleteAccountsClicked);
    headerRow->layout()->addWidget(deleteAccountBtn_);
    layout->addWidget(headerRow);

    accountTable_ = new QTableWidget(tab);
    const QStringList headers = {QStringLiteral("ID"),   QStringLiteral("이름"),
                                  QStringLiteral("역할"), QStringLiteral("담당 (대상자·로봇)"),
                                  QStringLiteral("최종 로그인"), QStringLiteral("상태"),
                                  QStringLiteral("액션"), QStringLiteral("선택")};
    accountTable_->setColumnCount(headers.size());
    accountTable_->setHorizontalHeaderLabels(headers);
    accountTable_->verticalHeader()->setVisible(false);
    accountTable_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    accountTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    accountTable_->setSelectionMode(QAbstractItemView::NoSelection);
    accountTable_->setFocusPolicy(Qt::NoFocus);
    accountTable_->setShowGrid(false);

    auto* header = accountTable_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    accountTable_->setColumnWidth(0, 110);  // ID
    accountTable_->setColumnWidth(1, 100);  // 이름
    accountTable_->setColumnWidth(2, 100);  // 역할
    accountTable_->setColumnWidth(4, 130);  // 최종 로그인
    accountTable_->setColumnWidth(5, 90);   // 상태
    accountTable_->setColumnWidth(6, 150);  // 액션
    accountTable_->setColumnWidth(7, 60);   // 선택(삭제용 체크박스)
    accountTable_->setColumnHidden(7, true);  // 관리자가 아니면 refreshAccountTable()에서도 숨김 유지

    accountTable_->setRowCount(static_cast<int>(accounts_.size()));
    for (int r = 0; r < static_cast<int>(accounts_.size()); ++r) {
        populateAccountRow(r);
    }
    layout->addWidget(accountTable_, /*stretch=*/1);

    return tab;
}

QWidget* P7SettingsPage::buildThresholdTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* cardsRow = new QWidget(tab);
    auto* cardsLayout = new QHBoxLayout(cardsRow);
    cardsLayout->setContentsMargins(0, 0, 0, 0);
    cardsLayout->setSpacing(Tokens::Layout::CARD_GAP);

    // 로봇 공통
    auto* robotCard = card(cardsRow);
    auto* robotLayout = new QVBoxLayout(robotCard);
    robotLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                     Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    robotLayout->setSpacing(8);
    robotLayout->addWidget(sectionLabel(QStringLiteral("로봇 공통")));
    robotLayout->addWidget(thresholdRow(QStringLiteral("배터리 주의 / 복귀"), thresholdField("30"),
                                         QStringLiteral("%"), thresholdField("20")));
    robotLayout->addWidget(
        thresholdRow(QStringLiteral("음성 응답 대기"), thresholdField("8"), QStringLiteral("초")));
    robotLayout->addWidget(
        thresholdRow(QStringLiteral("재연결 재시도 주기"), thresholdField("30"), QStringLiteral("초")));
    cardsLayout->addWidget(robotCard);

    // 복약 공통
    auto* medCard = card(cardsRow);
    auto* medLayout = new QVBoxLayout(medCard);
    medLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                   Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    medLayout->setSpacing(8);
    medLayout->addWidget(sectionLabel(QStringLiteral("복약 공통")));
    medLayout->addWidget(
        thresholdRow(QStringLiteral("미수령 판정 시간"), thresholdField("10"), QStringLiteral("분")));
    medLayout->addWidget(
        thresholdRow(QStringLiteral("재안내 횟수"), thresholdField("2"), QStringLiteral("회")));
    auto* medNote = new QLabel(QStringLiteral("심박 임계값은 대상자별 설정(대상자 관리)이 우선합니다"), medCard);
    medNote->setProperty("role", "muted");
    medNote->setWordWrap(true);
    medLayout->addWidget(medNote);
    cardsLayout->addWidget(medCard);

    layout->addWidget(cardsRow);

    // SMS 발신
    auto* smsCard = card(tab);
    auto* smsLayout = new QHBoxLayout(smsCard);
    smsLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                   Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    smsLayout->setSpacing(10);

    auto* smsLabel = new QLabel(QStringLiteral("SMS 발신"), smsCard);
    smsLabel->setProperty("role", "label");
    smsLayout->addWidget(smsLabel);

    auto* smsNumber = new QLineEdit(QStringLiteral("031-000-0000"), smsCard);
    smsNumber->setFixedWidth(120);
    smsLayout->addWidget(smsNumber);

    auto* testSendBtn = new QPushButton(QStringLiteral("테스트 발송"), smsCard);
    testSendBtn->setProperty("variant", "secondary");
    smsLayout->addWidget(testSendBtn);

    smsLayout->addStretch();

    auto* saveBtn = new QPushButton(QStringLiteral("저장"), smsCard);
    saveBtn->setProperty("variant", "primary");
    smsLayout->addWidget(saveBtn);

    layout->addWidget(smsCard);
    layout->addStretch();

    return tab;
}

QWidget* P7SettingsPage::buildAuditTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto* filterRow = new QWidget(tab);
    auto* filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);
    const QString filterLabels[] = {QStringLiteral("계정 전체"), QStringLiteral("행위 전체"),
                                     QStringLiteral("최근 7일")};
    for (const auto& text : filterLabels) {
        auto* chip = new QPushButton(SimpleIcons::chevronDown(Tokens::TEXT_SECONDARY, 11), text, filterRow);
        chip->setProperty("variant", "secondary");
        chip->setIconSize(QSize(11, 11));
        chip->setLayoutDirection(Qt::RightToLeft);
        filterLayout->addWidget(chip);
    }
    filterLayout->addStretch();
    layout->addWidget(filterRow);

    auto* table = new QTableWidget(tab);
    const QStringList headers = {QStringLiteral("시각"), QStringLiteral("계정"), QStringLiteral("행위"),
                                  QStringLiteral("대상 · 상세")};
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);

    auto* header = table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    table->setColumnWidth(0, 110);
    table->setColumnWidth(1, 80);
    table->setColumnWidth(2, 110);

    table->setRowCount(static_cast<int>(auditLog_.size()));
    for (int r = 0; r < static_cast<int>(auditLog_.size()); ++r) {
        const AuditRow& log = auditLog_[static_cast<size_t>(r)];

        auto* timeItem = new QTableWidgetItem(log.time);
        timeItem->setFont(QFont(Tokens::Font::MONO));
        timeItem->setTextAlignment(Qt::AlignCenter);
        timeItem->setForeground(Tokens::TEXT_SECONDARY);
        table->setItem(r, 0, timeItem);

        auto* accountItem = new QTableWidgetItem(log.account);
        accountItem->setTextAlignment(Qt::AlignCenter);
        table->setItem(r, 1, accountItem);

        auto* actionItem = new QTableWidgetItem(log.action);
        actionItem->setTextAlignment(Qt::AlignCenter);
        if (log.highlight) {
            actionItem->setForeground(Tokens::EMERGENCY.main);
            QFont f = actionItem->font();
            f.setWeight(QFont::DemiBold);
            actionItem->setFont(f);
        }
        table->setItem(r, 2, actionItem);

        auto* detailItem = new QTableWidgetItem(log.detail);
        detailItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        table->setItem(r, 3, detailItem);
    }
    layout->addWidget(table, /*stretch=*/1);

    auto* footnote = new QLabel(
        QStringLiteral("영상 열람 · 이벤트 완료 · E-STOP · 119 모의 신고는 빨간 강조 — 책임 추적 대상 행위"), tab);
    footnote->setProperty("role", "muted");
    layout->addWidget(footnote);

    return tab;
}

void P7SettingsPage::populateRobotRow(int r) {
    const RobotRow& row = robots_[static_cast<size_t>(r)];
    const bool offline = row.level == Tokens::Level::Offline;
    const QColor rowBg = offline ? Tokens::BG_SUNKEN : Tokens::BG_SURFACE;
    const QColor textColor = offline ? Tokens::TEXT_MUTED : Tokens::TEXT_PRIMARY;

    auto* idItem = new QTableWidgetItem(row.id);
    idItem->setFont(QFont(Tokens::Font::MONO));
    idItem->setTextAlignment(Qt::AlignCenter);
    idItem->setBackground(rowBg);
    idItem->setForeground(textColor);
    robotTable_->setItem(r, 0, idItem);

    auto* ipItem = new QTableWidgetItem(row.ip);
    ipItem->setFont(QFont(Tokens::Font::MONO));
    ipItem->setTextAlignment(Qt::AlignCenter);
    ipItem->setBackground(rowBg);
    ipItem->setForeground(offline ? Tokens::TEXT_MUTED : Tokens::TEXT_SECONDARY);
    robotTable_->setItem(r, 1, ipItem);

    auto* nsItem = new QTableWidgetItem(row.ns);
    nsItem->setFont(QFont(Tokens::Font::MONO));
    nsItem->setTextAlignment(Qt::AlignCenter);
    nsItem->setBackground(rowBg);
    nsItem->setForeground(offline ? Tokens::TEXT_MUTED : Tokens::TEXT_SECONDARY);
    robotTable_->setItem(r, 2, nsItem);

    auto* mapItem = new QTableWidgetItem(row.mapRecipient);
    mapItem->setTextAlignment(Qt::AlignCenter);
    mapItem->setBackground(rowBg);
    mapItem->setForeground(textColor);
    robotTable_->setItem(r, 3, mapItem);

    QColor statusColor = Tokens::OK.main;
    if (row.level == Tokens::Level::Offline) statusColor = Tokens::TEXT_MUTED;
    auto* statusItem = new QTableWidgetItem(row.statusDetail);
    statusItem->setTextAlignment(Qt::AlignCenter);
    statusItem->setBackground(rowBg);
    statusItem->setForeground(statusColor);
    robotTable_->setItem(r, 4, statusItem);

    const QColor nodeColor = row.level == Tokens::Level::Attention ? Tokens::ATTENTION.main : textColor;
    auto* nodeItem = new QTableWidgetItem(row.nodeSummary);
    nodeItem->setTextAlignment(Qt::AlignCenter);
    nodeItem->setBackground(rowBg);
    nodeItem->setForeground(nodeColor);
    if (row.level == Tokens::Level::Attention) {
        QFont f = nodeItem->font();
        f.setWeight(QFont::DemiBold);
        nodeItem->setFont(f);
    }
    robotTable_->setItem(r, 5, nodeItem);

    if (offline) {
        robotTable_->setCellWidget(r, 6, wrapCellWidget(linkText(QStringLiteral("재연결")), rowBg));
    } else {
        auto* editBtn = new QPushButton(QStringLiteral("수정 · 재시작"));
        editBtn->setObjectName("linkBtn");
        editBtn->setCursor(Qt::PointingHandCursor);
        connect(editBtn, &QPushButton::clicked, this, [this, r] { openRobotDialog(r); });
        robotTable_->setCellWidget(r, 6, wrapCellWidget(editBtn, rowBg));
    }
}

void P7SettingsPage::refreshRobotTable() {
    if (robotCountLabel_ != nullptr) {
        robotCountLabel_->setText(QStringLiteral("등록 로봇 %1").arg(robots_.size()));
    }
    robotTable_->setRowCount(static_cast<int>(robots_.size()));
    for (int r = 0; r < static_cast<int>(robots_.size()); ++r) {
        populateRobotRow(r);
    }
}

void P7SettingsPage::openRobotDialog(int rowIndex) {
    const bool isNew = (rowIndex < 0);
    RobotDialog::RobotData data;
    if (isNew) {
        int maxNum = 0;
        for (const auto& r : robots_) {
            const int num = r.id.mid(2).toInt();
            maxNum = std::max(maxNum, num);
        }
        data.code = QStringLiteral("R-%1").arg(maxNum + 1, 2, 10, QLatin1Char('0'));
    } else {
        const RobotRow& row = robots_[static_cast<size_t>(rowIndex)];
        data.code = row.id;
        data.name = row.name;
        data.model = row.model;
        data.rosDomainId = row.rosDomainId;
        data.recipientLabel = row.recipientLabel;
        data.connected = (row.level != Tokens::Level::Offline);
        data.wsHost = row.wsHost;
        data.connectionDetail = row.connectionDetail;
        if (row.id == QStringLiteral("R-03")) {
            for (const auto& node : r03Nodes_) {
                if (node.warn) {
                    data.warnNode = node.name;
                    data.warnDetail = node.detail;
                    break;
                }
            }
        }
    }

    const QString previousRecipient = isNew ? QString() : robots_[static_cast<size_t>(rowIndex)].recipientLabel;

    auto* dialog = new RobotDialog(isNew ? RobotDialog::Mode::Register : RobotDialog::Mode::Edit, data, this);
    connect(dialog, &RobotDialog::robotSaved, this,
            [this, rowIndex, isNew, previousRecipient](const RobotDialog::RobotData& saved) {
        RobotRow row;
        if (isNew) {
            row.id = saved.code;
            row.ip = QStringLiteral("—");
            row.ns = QStringLiteral("/limo_%1").arg(saved.code.mid(2));
            row.level = Tokens::Level::Offline;
            row.statusDetail = QStringLiteral("오프라인");
            row.nodeSummary = Tokens::Format::EMPTY;
        } else {
            row = robots_[static_cast<size_t>(rowIndex)];
        }
        row.name = saved.name;
        row.model = saved.model;
        row.rosDomainId = saved.rosDomainId;
        row.recipientLabel = saved.recipientLabel;
        if (isNew || saved.recipientLabel != previousRecipient) {
            row.mapRecipient = saved.recipientLabel;
        }
        if (isNew) {
            robots_.push_back(row);
        } else {
            robots_[static_cast<size_t>(rowIndex)] = row;
        }
        refreshRobotTable();
    });
    connect(dialog, &RobotDialog::robotDeregisterRequested, this, [this, rowIndex](const QString&) {
        if (rowIndex >= 0) {
            robots_.erase(robots_.begin() + rowIndex);
            refreshRobotTable();
        }
    });
    dialog->exec();
    dialog->deleteLater();
}

namespace {
AccountDialog::Role koreanRoleToEnum(const QString& role) {
    if (role == QStringLiteral("관리자")) return AccountDialog::Role::Admin;
    if (role == QStringLiteral("보호자")) return AccountDialog::Role::Guardian;
    return AccountDialog::Role::Operator;
}

QString roleEnumToCode(AccountDialog::Role role) {
    switch (role) {
        case AccountDialog::Role::Admin: return QStringLiteral("admin");
        case AccountDialog::Role::Guardian: return QStringLiteral("guardian");
        case AccountDialog::Role::Operator: return QStringLiteral("operator");
    }
    return QString();
}

// role.name(예: "관제 담당자")이 아니라 화면 표에서 그대로 쓰는 짧은 표기로 맞춘다.
QString roleCodeToKorean(const QString& code) {
    if (code == QStringLiteral("admin")) return QStringLiteral("관리자");
    if (code == QStringLiteral("guardian")) return QStringLiteral("보호자");
    return QStringLiteral("관제");
}

QString formatServerTimestamp(const QString& iso) {
    if (iso.isEmpty()) return QString();
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!dt.isValid()) return QString();
    return dt.toString(QStringLiteral("MM.dd HH:mm"));
}
}  // namespace

bool P7SettingsPage::isLastActiveAdmin(int rowIndex) const {
    const AccountRow& target = accounts_[static_cast<size_t>(rowIndex)];
    if (target.role != QStringLiteral("관리자") || !target.active) return false;
    int activeAdmins = 0;
    for (const auto& acc : accounts_) {
        if (acc.role == QStringLiteral("관리자") && acc.active) ++activeAdmins;
    }
    return activeAdmins <= 1;
}

void P7SettingsPage::populateAccountRow(int r) {
    const AccountRow& acc = accounts_[static_cast<size_t>(r)];
    const QColor rowBg = acc.active ? Tokens::BG_SURFACE : Tokens::BG_SUNKEN;
    const QColor textColor = acc.active ? Tokens::TEXT_PRIMARY : Tokens::TEXT_MUTED;

    auto* idItem = new QTableWidgetItem(acc.id);
    idItem->setFont(QFont(Tokens::Font::MONO));
    idItem->setTextAlignment(Qt::AlignCenter);
    idItem->setBackground(rowBg);
    idItem->setForeground(textColor);
    accountTable_->setItem(r, 0, idItem);

    auto* nameItem = new QTableWidgetItem(acc.name);
    nameItem->setTextAlignment(Qt::AlignCenter);
    nameItem->setBackground(rowBg);
    nameItem->setForeground(textColor);
    accountTable_->setItem(r, 1, nameItem);

    auto* roleItem = new QTableWidgetItem(acc.role);
    roleItem->setTextAlignment(Qt::AlignCenter);
    roleItem->setBackground(rowBg);
    roleItem->setForeground(acc.role == QStringLiteral("관리자") ? Tokens::ACCENT_HOVER : textColor);
    QFont roleFont = roleItem->font();
    roleFont.setWeight(acc.role == QStringLiteral("관리자") ? QFont::DemiBold : QFont::Normal);
    roleItem->setFont(roleFont);
    accountTable_->setItem(r, 2, roleItem);

    auto* chargeItem = new QTableWidgetItem(acc.charge);
    chargeItem->setTextAlignment(Qt::AlignCenter);
    chargeItem->setBackground(rowBg);
    chargeItem->setForeground(textColor);
    accountTable_->setItem(r, 3, chargeItem);

    auto* loginItem = new QTableWidgetItem(acc.lastLogin);
    loginItem->setFont(QFont(Tokens::Font::MONO));
    loginItem->setTextAlignment(Qt::AlignCenter);
    loginItem->setBackground(rowBg);
    loginItem->setForeground(acc.active ? Tokens::TEXT_SECONDARY : Tokens::TEXT_MUTED);
    accountTable_->setItem(r, 4, loginItem);

    accountTable_->setCellWidget(
        r, 5,
        wrapCellWidget(makeBadge(acc.active ? Tokens::Prop::OK : Tokens::Prop::OFFLINE,
                                  acc.active ? QStringLiteral("활성") : QStringLiteral("비활성")),
                        rowBg));

    if (acc.active) {
        const bool canEdit = isCurrentUserAdmin();
        auto* editBtn = new QPushButton(QStringLiteral("수정 · 비밀번호 변경"));
        editBtn->setObjectName("linkBtn");
        editBtn->setEnabled(canEdit);
        editBtn->setCursor(canEdit ? Qt::PointingHandCursor : Qt::ArrowCursor);
        connect(editBtn, &QPushButton::clicked, this, [this, r] { openAccountDialog(r); });
        accountTable_->setCellWidget(r, 6, wrapCellWidget(editBtn, rowBg));
    } else {
        auto* activateBtn = new QPushButton(QStringLiteral("활성화"));
        activateBtn->setObjectName("linkBtn");
        activateBtn->setCursor(Qt::PointingHandCursor);
        connect(activateBtn, &QPushButton::clicked, this, [this, r] {
            const AccountRow& target = accounts_[static_cast<size_t>(r)];
            QMessageBox box(this);
            box.setWindowTitle(QStringLiteral("계정 활성화"));
            box.setText(QStringLiteral("%1(%2) 계정을 다시 활성화하시겠습니까?").arg(target.name, target.id));
            box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
            box.setDefaultButton(QMessageBox::Cancel);
            box.button(QMessageBox::Ok)->setText(QStringLiteral("확인"));
            box.button(QMessageBox::Cancel)->setText(QStringLiteral("취소"));
            if (box.exec() == QMessageBox::Ok) activateAccount(r);
        });
        accountTable_->setCellWidget(r, 6, wrapCellWidget(activateBtn, rowBg));
    }

    // 삭제용 체크박스 — 관리자 권한이 아니면 컬럼 자체가 숨겨져 있어 보이지 않는다.
    auto* selectCheck = new QCheckBox();
    accountTable_->setCellWidget(r, 7, wrapCellWidget(selectCheck, rowBg));
}

void P7SettingsPage::refreshAccountTable() {
    if (accountCountLabel_ != nullptr) {
        const int activeCount = static_cast<int>(
            std::count_if(accounts_.begin(), accounts_.end(), [](const AccountRow& a) { return a.active; }));
        accountCountLabel_->setText(QStringLiteral("계정 %1 · 활성 %2").arg(accounts_.size()).arg(activeCount));
    }
    accountTable_->setRowCount(static_cast<int>(accounts_.size()));
    for (int r = 0; r < static_cast<int>(accounts_.size()); ++r) {
        populateAccountRow(r);
    }

    // 계정 삭제는 관리자 권한 계정만 — 로그인 세션 연동 전이라 currentUserLoginId_로
    // 본인 계정을 찾아 그 역할로 판단한다.
    const bool canDelete = isCurrentUserAdmin();
    accountTable_->setColumnHidden(7, !canDelete);
    if (deleteAccountBtn_ != nullptr) deleteAccountBtn_->setVisible(canDelete);
}

void P7SettingsPage::fetchAccounts() {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/accounts")));
    QNetworkReply* reply = netManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleAccountsReply(reply);
        reply->deleteLater();
    });
}

void P7SettingsPage::handleAccountsReply(QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        // 서버 연결 실패 시 목록을 비운 채로 둔다 — P4 영상 목록과 동일한 방침.
        accounts_.clear();
        if (accountTable_ != nullptr) refreshAccountTable();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    std::vector<AccountRow> rows;
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject obj = value.toObject();
        AccountRow row;
        row.dbId = obj.value("id").toInt();
        row.id = obj.value("login_id").toString();
        row.name = obj.value("display_name").toString();
        row.role = roleCodeToKorean(obj.value("role_code").toString());
        const QString chargeSummary = obj.value("charge_summary").toString();
        row.charge = chargeSummary.isEmpty() ? QStringLiteral("미배정") : chargeSummary;
        row.lastLogin = formatServerTimestamp(obj.value("last_login_at").toString());
        row.active = obj.value("is_active").toBool();
        rows.push_back(row);
    }
    accounts_ = rows;
    if (accountTable_ != nullptr) refreshAccountTable();
}

void P7SettingsPage::registerAccount(const AccountDialog::AccountData& data) {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/accounts")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{
        {"login_id", data.loginId},
        {"password", data.password},
        {"display_name", data.displayName},
        {"role_code", roleEnumToCode(data.role)},
        {"is_active", data.active},
    };
    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, assignedRecipientIds = data.assignedRecipientIds] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200 && statusCode != 201) {
            const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
            const QString detail = err.value("detail").toString();
            reply->deleteLater();
            QMessageBox::warning(this, QStringLiteral("계정 등록 실패"),
                                  detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
            return;
        }
        const QJsonObject created = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (assignedRecipientIds.isEmpty()) {
            fetchAccounts();  // 등록 결과를 DB 기준으로 다시 불러와 반영한다.
        } else {
            // 등록과 동시에 담당 대상자를 골랐으면 이어서 배정한다. assignRecipients()가
            // 성공 시 fetchAccounts()/fetchRecipients()까지 처리한다.
            assignRecipients(created.value("id").toInt(), assignedRecipientIds);
        }
    });
}

void P7SettingsPage::updateAccount(int rowIndex, const AccountDialog::AccountData& data) {
    const int dbId = accounts_[static_cast<size_t>(rowIndex)].dbId;
    QNetworkRequest request(QUrl(QStringLiteral("%1/accounts/%2").arg(serverUrl_).arg(dbId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{
        {"display_name", data.displayName},
        {"role_code", roleEnumToCode(data.role)},
        {"is_active", data.active},
    };
    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PATCH", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, dbId, assignedRecipientIds = data.assignedRecipientIds] {
                const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (statusCode != 200) {
                    const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
                    const QString detail = err.value("detail").toString();
                    reply->deleteLater();
                    QMessageBox::warning(this, QStringLiteral("계정 수정 실패"),
                                          detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
                    return;
                }
                reply->deleteLater();
                // assignRecipients()가 성공 시 fetchAccounts()까지 다시 불러와 반영한다.
                assignRecipients(dbId, assignedRecipientIds);
            });
}

void P7SettingsPage::changePassword(int userId, const QString& newPassword) {
    QNetworkRequest request(QUrl(QStringLiteral("%1/accounts/%2/password").arg(serverUrl_).arg(userId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{{"new_password", newPassword}};
    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PATCH", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (statusCode != 204) {
            QMessageBox::warning(this, QStringLiteral("비밀번호 변경 실패"),
                                  QStringLiteral("서버에 연결할 수 없습니다"));
            return;
        }
        QMessageBox::information(this, QStringLiteral("비밀번호 변경"), QStringLiteral("비밀번호가 변경되었습니다."));
    });
}

void P7SettingsPage::activateAccount(int rowIndex) {
    const AccountRow& acc = accounts_[static_cast<size_t>(rowIndex)];
    QNetworkRequest request(QUrl(QStringLiteral("%1/accounts/%2").arg(serverUrl_).arg(acc.dbId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{
        {"display_name", acc.name},
        {"role_code", roleEnumToCode(koreanRoleToEnum(acc.role))},
        {"is_active", true},
    };
    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PATCH", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) {
            const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
            const QString detail = err.value("detail").toString();
            reply->deleteLater();
            QMessageBox::warning(this, QStringLiteral("계정 활성화 실패"),
                                  detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
            return;
        }
        reply->deleteLater();
        fetchAccounts();
    });
}

bool P7SettingsPage::isCurrentUserAdmin() const {
    for (const auto& acc : accounts_) {
        if (acc.id == currentUserLoginId_) return acc.role == QStringLiteral("관리자");
    }
    return false;
}

void P7SettingsPage::handleDeleteAccountsClicked() {
    QVector<int> ids;
    QStringList labels;
    for (int r = 0; r < accountTable_->rowCount(); ++r) {
        auto* wrap = accountTable_->cellWidget(r, 7);
        auto* checkbox = (wrap != nullptr) ? wrap->findChild<QCheckBox*>() : nullptr;
        if (checkbox != nullptr && checkbox->isChecked()) {
            const AccountRow& acc = accounts_[static_cast<size_t>(r)];
            ids.append(acc.dbId);
            labels.append(QStringLiteral("%1(%2)").arg(acc.name, acc.id));
        }
    }
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("계정 삭제"), QStringLiteral("삭제할 계정을 먼저 선택하세요"));
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("계정 삭제"));
    box.setText(QStringLiteral("%1 계정을 삭제하시겠습니까?\n이 작업은 되돌릴 수 없습니다.").arg(labels.join(QStringLiteral(", "))));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("확인"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("취소"));
    if (box.exec() != QMessageBox::Ok) return;

    deleteAccounts(ids);
}

void P7SettingsPage::deleteAccounts(const QVector<int>& userIds) {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/accounts/delete")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonArray idsArray;
    for (int id : userIds) idsArray.append(id);
    const QJsonObject body{{"user_ids", idsArray}};

    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (statusCode != 204) {
            QMessageBox::warning(this, QStringLiteral("계정 삭제 실패"), QStringLiteral("서버에 연결할 수 없습니다"));
            return;
        }
        fetchAccounts();
    });
}

void P7SettingsPage::fetchRecipients() {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/accounts/recipients")));
    QNetworkReply* reply = netManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleRecipientsReply(reply);
        reply->deleteLater();
    });
}

void P7SettingsPage::handleRecipientsReply(QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        recipients_.clear();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    std::vector<RecipientRow> rows;
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject obj = value.toObject();
        RecipientRow row;
        row.id = obj.value("id").toInt();
        row.code = obj.value("code").toString();
        const QString robotCode = obj.value("robot_code").toString();
        row.label = robotCode.isEmpty()
                        ? QStringLiteral("%1 (%2)").arg(obj.value("name").toString(), obj.value("code").toString())
                        : QStringLiteral("%1 (%2) · %3")
                              .arg(obj.value("name").toString(), obj.value("code").toString(), robotCode);
        row.operatorId = obj.value("operator_id").isNull() ? -1 : obj.value("operator_id").toInt();
        rows.push_back(row);
    }
    recipients_ = rows;
}

void P7SettingsPage::assignRecipients(int userId, const QVector<int>& recipientIds) {
    QNetworkRequest request(QUrl(QStringLiteral("%1/accounts/%2/recipients").arg(serverUrl_).arg(userId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonArray idsArray;
    for (int id : recipientIds) idsArray.append(id);
    const QJsonObject body{{"recipient_ids", idsArray}};

    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PUT", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (statusCode != 204) {
            QMessageBox::warning(this, QStringLiteral("담당 배정 실패"),
                                  QStringLiteral("계정 정보는 저장됐지만 담당 대상자 배정에는 실패했습니다"));
            return;
        }
        fetchAccounts();
        fetchRecipients();
    });
}

void P7SettingsPage::openAccountDialog(int rowIndex) {
    const bool isNew = (rowIndex < 0);
    AccountDialog::AccountData data;
    QStringList existingIds;
    for (const auto& acc : accounts_) existingIds << acc.id;

    // 신규 등록은 아직 dbId가 없으니 전부 미체크로 보여준다 — 등록과 동시에 담당을
    // 고를 수 있게 하되(공백으로 두면 미배정), 기존 배정 여부는 수정 모드에서만 따진다.
    QVector<AccountDialog::RecipientOption> recipientOptions;
    for (const RecipientRow& rc : recipients_) {
        AccountDialog::RecipientOption option;
        option.id = rc.id;
        option.code = rc.code;
        option.label = rc.label;
        option.assigned = !isNew && rc.operatorId == accounts_[static_cast<size_t>(rowIndex)].dbId;
        recipientOptions.push_back(option);
    }

    if (!isNew) {
        const AccountRow& acc = accounts_[static_cast<size_t>(rowIndex)];
        data.loginId = acc.id;
        data.displayName = acc.name;
        data.role = koreanRoleToEnum(acc.role);
        data.active = acc.active;
        data.lastLogin = acc.lastLogin;
        data.isSelf = (acc.id == currentUserLoginId_);
        data.isLastActiveAdmin = isLastActiveAdmin(rowIndex);
    }

    auto* dialog = new AccountDialog(isNew ? AccountDialog::Mode::Register : AccountDialog::Mode::Edit, data,
                                      existingIds, recipientOptions, this);
    connect(dialog, &AccountDialog::accountSaved, this,
            [this, rowIndex, isNew](const AccountDialog::AccountData& saved) {
                if (isNew) {
                    // 신규 등록은 서버(DB)에 저장하고, 응답 후 목록을 다시 불러와 반영한다.
                    registerAccount(saved);
                    return;
                }
                // 기존 계정 수정도 서버(DB)에 반영한다. 성공 시 updateAccount()가
                // assignRecipients()를 거쳐 fetchAccounts()까지 다시 불러와 반영한다.
                updateAccount(rowIndex, saved);
            });
    connect(dialog, &AccountDialog::passwordChangeRequested, this,
            [this, rowIndex](const QString&, const QString& newPassword) {
                const int dbId = accounts_[static_cast<size_t>(rowIndex)].dbId;
                changePassword(dbId, newPassword);
            });
    dialog->exec();
    dialog->deleteLater();
}
