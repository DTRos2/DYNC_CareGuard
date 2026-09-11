#include "p8_logs_page.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "theme/calendar_style.h"
#include "theme/tokens.h"
#include "widgets/careguard_date_edit.h"
#include "widgets/live_search_line_edit.h"

namespace {

QLabel* makeMutedLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", "muted");
    return label;
}

void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

}  // namespace

bool P8LogsPage::eventFilter(QObject* watched, QEvent* event) {
    // P2EventPage::eventFilter와 동일한 패턴 — 읽기 전용 날짜 칸 클릭을 "직접"로 전환.
    if (event->type() == QEvent::MouseButtonPress &&
        (watched == auditStartDateEdit_ || watched == auditEndDateEdit_) &&
        auditStartDateEdit_->isReadOnly()) {
        applyAuditPeriodPreset(-1);
    }
    if (event->type() == QEvent::MouseButtonPress &&
        (watched == systemEventStartDateEdit_ || watched == systemEventEndDateEdit_) &&
        systemEventStartDateEdit_->isReadOnly()) {
        applySystemEventPeriodPreset(-1);
    }
    if (event->type() == QEvent::MouseButtonPress &&
        (watched == patrolRunStartDateEdit_ || watched == patrolRunEndDateEdit_) &&
        patrolRunStartDateEdit_->isReadOnly()) {
        applyPatrolRunPeriodPreset(-1);
    }
    return QWidget::eventFilter(watched, event);
}

P8LogsPage::P8LogsPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("통합 로그"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildAuditTab(), QStringLiteral("사용자 로그"));
    tabs->addTab(buildSystemEventLogTab(), QStringLiteral("임계값·알림 로그"));
    tabs->addTab(buildPatrolRunLogTab(), QStringLiteral("경로순찰 로그"));
    layout->addWidget(tabs);

    serverUrl_ = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_URL"), QStringLiteral("http://127.0.0.1:8000"));
    netManager_ = new QNetworkAccessManager(this);
    fetchAuditLog();
    fetchSystemEventLog();
    fetchPatrolRunLog();
}

QWidget* P8LogsPage::buildAuditTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto* filterRow = new QWidget(tab);
    auto* filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);

    // 기간 세그먼트 + 날짜 범위 + 검색 — P2EventPage::buildFilterBar()와 동일한 구성을 그대로 가져왔다.
    const QDate today = QDate::currentDate();

    auto* periodSeg = new QWidget(filterRow);
    auto* periodSegLayout = new QHBoxLayout(periodSeg);
    periodSegLayout->setContentsMargins(0, 0, 0, 0);
    periodSegLayout->setSpacing(0);

    auto* periodTodayBtn = new QPushButton(QStringLiteral("오늘"), periodSeg);
    auto* periodWeekBtn = new QPushButton(QStringLiteral("7일"), periodSeg);
    auto* periodMonthBtn = new QPushButton(QStringLiteral("30일"), periodSeg);
    auditPeriodCustomBtn_ = new QPushButton(QStringLiteral("직접"), periodSeg);
    periodTodayBtn->setProperty("pos", "first");
    auditPeriodCustomBtn_->setProperty("pos", "last");

    auto* periodGroup = new QButtonGroup(this);
    periodGroup->setExclusive(true);
    for (auto* btn : {periodTodayBtn, periodWeekBtn, periodMonthBtn, auditPeriodCustomBtn_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        periodSegLayout->addWidget(btn);
        periodGroup->addButton(btn);
    }
    periodWeekBtn->setChecked(true);  // 기존 "최근 7일" 기본값을 그대로 유지
    filterLayout->addWidget(periodSeg);

    connect(periodTodayBtn, &QPushButton::clicked, this, [this] { applyAuditPeriodPreset(0); });
    connect(periodWeekBtn, &QPushButton::clicked, this, [this] { applyAuditPeriodPreset(6); });
    connect(periodMonthBtn, &QPushButton::clicked, this, [this] { applyAuditPeriodPreset(29); });
    connect(auditPeriodCustomBtn_, &QPushButton::clicked, this, [this] { applyAuditPeriodPreset(-1); });

    auditStartDateEdit_ = new CareGuardDateEdit(today, filterRow);
    CareGuard::applyCareGuardDateEdit(auditStartDateEdit_);
    filterLayout->addWidget(auditStartDateEdit_);

    filterLayout->addWidget(makeMutedLabel(QStringLiteral("~"), filterRow));
    auditEndDateEdit_ = new CareGuardDateEdit(today, filterRow);
    CareGuard::applyCareGuardDateEdit(auditEndDateEdit_);
    filterLayout->addWidget(auditEndDateEdit_);

    connect(auditStartDateEdit_, &QDateEdit::dateChanged, this, &P8LogsPage::onAuditDateRangeChanged);
    connect(auditEndDateEdit_, &QDateEdit::dateChanged, this, &P8LogsPage::onAuditDateRangeChanged);
    // 읽기 전용 칸은 클릭이 편집으로 이어지지 않으니, 클릭 자체를 잡아 "직접"로 전환한다.
    auditStartDateEdit_->installEventFilter(this);
    auditEndDateEdit_->installEventFilter(this);

    applyAuditPeriodPreset(6);  // 초기 상태: 최근 7일, 읽기 전용

    auditSearchEdit_ = new LiveSearchLineEdit(filterRow);
    auditSearchEdit_->setPlaceholderText(QStringLiteral("계정·행위·대상 검색"));
    connect(auditSearchEdit_, &LiveSearchLineEdit::liveTextChanged, this, [this](const QString& text) {
        auditSearchText_ = text;
        fetchAuditLog();
    });
    filterLayout->addWidget(auditSearchEdit_, /*stretch=*/1);
    layout->addWidget(filterRow);

    auditTable_ = new QTableWidget(tab);
    const QStringList headers = {QStringLiteral("시각"), QStringLiteral("계정"), QStringLiteral("행위"),
                                  QStringLiteral("대상 · 상세")};
    auditTable_->setColumnCount(headers.size());
    auditTable_->setHorizontalHeaderLabels(headers);
    auditTable_->verticalHeader()->setVisible(false);
    auditTable_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    auditTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auditTable_->setSelectionMode(QAbstractItemView::NoSelection);
    auditTable_->setFocusPolicy(Qt::NoFocus);
    auditTable_->setShowGrid(false);

    auto* header = auditTable_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    auditTable_->setColumnWidth(0, 200);
    auditTable_->setColumnWidth(1, 220);
    auditTable_->setColumnWidth(2, 260);
    layout->addWidget(auditTable_, /*stretch=*/1);

    // netManager_는 생성자 맨 마지막에 초기화되므로, 최초 조회는 여기서 바로 하지 않고
    // 생성자 끝에서 한다.

    return tab;
}

void P8LogsPage::onAuditDateRangeChanged() {
    // 시작이 끝보다 늦으면 끝을 시작에 맞춰 밀어준다 (P2EventPage::onDateRangeChanged와 동일 규칙).
    if (auditStartDateEdit_->date() > auditEndDateEdit_->date()) {
        auditEndDateEdit_->setDate(auditStartDateEdit_->date());
        return;  // setDate가 다시 dateChanged를 내서 아래 fetchAuditLog()까지 이어진다.
    }
    fetchAuditLog();
}

void P8LogsPage::applyAuditPeriodPreset(int spanDays) {
    if (spanDays >= 0) {
        const QDate today = QDate::currentDate();
        auditStartDateEdit_->setDate(today.addDays(-spanDays));
        auditEndDateEdit_->setDate(today);
        setAuditDateEditsReadOnly(true);
    } else {
        setAuditDateEditsReadOnly(false);
        if (!auditPeriodCustomBtn_->isChecked()) {
            auditPeriodCustomBtn_->setChecked(true);  // 날짜 칸을 직접 클릭해 진입한 경우
        }
    }
}

void P8LogsPage::setAuditDateEditsReadOnly(bool readOnly) {
    for (auto* edit : {auditStartDateEdit_, auditEndDateEdit_}) {
        edit->setReadOnly(readOnly);
        edit->setProperty("state", readOnly ? QStringLiteral("off") : QVariant());
        repolish(edit);
    }
}

void P8LogsPage::fetchAuditLog() {
    // 생성자에서 탭을 먼저 만들고 netManager_는 맨 마지막에 초기화한다.
    // 날짜 칸 초기값 설정(applyAuditPeriodPreset)이 dateChanged를 통해 이 함수를 이 시점에
    // 먼저 불러버리므로, 아직 준비 안 된 상태면 조용히 넘어가고 생성자 마지막의 명시적
    // 호출이 실제 최초 조회를 담당한다.
    if (auditStartDateEdit_ == nullptr || netManager_ == nullptr) return;

    QUrl url(serverUrl_ + QStringLiteral("/audit-log"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("start"), auditStartDateEdit_->date().toString(Qt::ISODate));
    query.addQueryItem(QStringLiteral("end"), auditEndDateEdit_->date().toString(Qt::ISODate));
    if (!auditSearchText_.trimmed().isEmpty()) {
        query.addQueryItem(QStringLiteral("q"), auditSearchText_.trimmed());
    }
    url.setQuery(query);

    QNetworkReply* reply = netManager_->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleAuditLogReply(reply);
        reply->deleteLater();
    });
}

void P8LogsPage::handleAuditLogReply(QNetworkReply* reply) {
    auditLog_.clear();
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 200) {
        for (const QJsonValue& value : QJsonDocument::fromJson(reply->readAll()).array()) {
            const QJsonObject obj = value.toObject();

            // occurred_at은 ISO 8601("...+09:00")로 온다. 초 이하 자릿수가 서버·행마다
            // 다를 수 있어(마이크로초 vs 없음) 두 포맷을 순서대로 시도한다.
            const QString iso = obj.value("occurred_at").toString();
            QDateTime occurredAt = QDateTime::fromString(iso, Qt::ISODateWithMs);
            if (!occurredAt.isValid()) occurredAt = QDateTime::fromString(iso, Qt::ISODate);

            AuditRow row;
            row.time = occurredAt.toString(QStringLiteral("MM.dd HH:mm"));
            row.account = obj.value("account").toString(QStringLiteral("-"));
            row.action = obj.value("action").toString();
            row.detail = obj.value("detail").toString();
            row.highlight = obj.value("highlight").toBool();
            auditLog_.push_back(row);
        }
    }
    refreshAuditTable();
}

void P8LogsPage::refreshAuditTable() {
    if (auditTable_ == nullptr) return;

    auditTable_->setRowCount(static_cast<int>(auditLog_.size()));
    for (int r = 0; r < static_cast<int>(auditLog_.size()); ++r) {
        const AuditRow& log = auditLog_[static_cast<size_t>(r)];

        auto* timeItem = new QTableWidgetItem(log.time);
        timeItem->setFont(QFont(Tokens::Font::MONO));
        timeItem->setTextAlignment(Qt::AlignCenter);
        timeItem->setForeground(Tokens::TEXT_SECONDARY);
        auditTable_->setItem(r, 0, timeItem);

        auto* accountItem = new QTableWidgetItem(log.account);
        accountItem->setTextAlignment(Qt::AlignCenter);
        auditTable_->setItem(r, 1, accountItem);

        auto* actionItem = new QTableWidgetItem(log.action);
        actionItem->setTextAlignment(Qt::AlignCenter);
        if (log.highlight) {
            actionItem->setForeground(Tokens::EMERGENCY.main);
            QFont f = actionItem->font();
            f.setWeight(QFont::DemiBold);
            actionItem->setFont(f);
        }
        auditTable_->setItem(r, 2, actionItem);

        auto* detailItem = new QTableWidgetItem(log.detail);
        detailItem->setTextAlignment(Qt::AlignCenter);
        auditTable_->setItem(r, 3, detailItem);
    }
}

// ---------- 임계값·알림 로그 탭 — buildAuditTab()과 같은 필터 UI 관례를 그대로 따른다 ----------

QWidget* P8LogsPage::buildSystemEventLogTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto* filterRow = new QWidget(tab);
    auto* filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);

    const QDate today = QDate::currentDate();

    auto* periodSeg = new QWidget(filterRow);
    auto* periodSegLayout = new QHBoxLayout(periodSeg);
    periodSegLayout->setContentsMargins(0, 0, 0, 0);
    periodSegLayout->setSpacing(0);

    auto* periodTodayBtn = new QPushButton(QStringLiteral("오늘"), periodSeg);
    auto* periodWeekBtn = new QPushButton(QStringLiteral("7일"), periodSeg);
    auto* periodMonthBtn = new QPushButton(QStringLiteral("30일"), periodSeg);
    systemEventPeriodCustomBtn_ = new QPushButton(QStringLiteral("직접"), periodSeg);
    periodTodayBtn->setProperty("pos", "first");
    systemEventPeriodCustomBtn_->setProperty("pos", "last");

    auto* periodGroup = new QButtonGroup(this);
    periodGroup->setExclusive(true);
    for (auto* btn : {periodTodayBtn, periodWeekBtn, periodMonthBtn, systemEventPeriodCustomBtn_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        periodSegLayout->addWidget(btn);
        periodGroup->addButton(btn);
    }
    periodWeekBtn->setChecked(true);
    filterLayout->addWidget(periodSeg);

    connect(periodTodayBtn, &QPushButton::clicked, this, [this] { applySystemEventPeriodPreset(0); });
    connect(periodWeekBtn, &QPushButton::clicked, this, [this] { applySystemEventPeriodPreset(6); });
    connect(periodMonthBtn, &QPushButton::clicked, this, [this] { applySystemEventPeriodPreset(29); });
    connect(systemEventPeriodCustomBtn_, &QPushButton::clicked, this,
            [this] { applySystemEventPeriodPreset(-1); });

    systemEventStartDateEdit_ = new CareGuardDateEdit(today, filterRow);
    CareGuard::applyCareGuardDateEdit(systemEventStartDateEdit_);
    filterLayout->addWidget(systemEventStartDateEdit_);

    filterLayout->addWidget(makeMutedLabel(QStringLiteral("~"), filterRow));
    systemEventEndDateEdit_ = new CareGuardDateEdit(today, filterRow);
    CareGuard::applyCareGuardDateEdit(systemEventEndDateEdit_);
    filterLayout->addWidget(systemEventEndDateEdit_);

    connect(systemEventStartDateEdit_, &QDateEdit::dateChanged, this,
            &P8LogsPage::onSystemEventDateRangeChanged);
    connect(systemEventEndDateEdit_, &QDateEdit::dateChanged, this,
            &P8LogsPage::onSystemEventDateRangeChanged);
    systemEventStartDateEdit_->installEventFilter(this);
    systemEventEndDateEdit_->installEventFilter(this);

    applySystemEventPeriodPreset(6);

    systemEventSearchEdit_ = new LiveSearchLineEdit(filterRow);
    systemEventSearchEdit_->setPlaceholderText(QStringLiteral("로봇·종류·메시지 검색"));
    connect(systemEventSearchEdit_, &LiveSearchLineEdit::liveTextChanged, this, [this](const QString& text) {
        systemEventSearchText_ = text;
        fetchSystemEventLog();
    });
    filterLayout->addWidget(systemEventSearchEdit_, /*stretch=*/1);
    layout->addWidget(filterRow);

    systemEventTable_ = new QTableWidget(tab);
    const QStringList headers = {QStringLiteral("시각"), QStringLiteral("로봇"), QStringLiteral("종류"),
                                  QStringLiteral("메시지")};
    systemEventTable_->setColumnCount(headers.size());
    systemEventTable_->setHorizontalHeaderLabels(headers);
    systemEventTable_->verticalHeader()->setVisible(false);
    systemEventTable_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    systemEventTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    systemEventTable_->setSelectionMode(QAbstractItemView::NoSelection);
    systemEventTable_->setFocusPolicy(Qt::NoFocus);
    systemEventTable_->setShowGrid(false);

    auto* seHeader = systemEventTable_->horizontalHeader();
    seHeader->setSectionResizeMode(QHeaderView::Interactive);
    seHeader->setSectionResizeMode(3, QHeaderView::Stretch);
    systemEventTable_->setColumnWidth(0, 160);
    systemEventTable_->setColumnWidth(1, 100);
    systemEventTable_->setColumnWidth(2, 160);
    layout->addWidget(systemEventTable_, /*stretch=*/1);

    return tab;
}

void P8LogsPage::onSystemEventDateRangeChanged() {
    if (systemEventStartDateEdit_->date() > systemEventEndDateEdit_->date()) {
        systemEventEndDateEdit_->setDate(systemEventStartDateEdit_->date());
        return;
    }
    fetchSystemEventLog();
}

void P8LogsPage::applySystemEventPeriodPreset(int spanDays) {
    if (spanDays >= 0) {
        const QDate today = QDate::currentDate();
        systemEventStartDateEdit_->setDate(today.addDays(-spanDays));
        systemEventEndDateEdit_->setDate(today);
        setSystemEventDateEditsReadOnly(true);
    } else {
        setSystemEventDateEditsReadOnly(false);
        if (!systemEventPeriodCustomBtn_->isChecked()) {
            systemEventPeriodCustomBtn_->setChecked(true);
        }
    }
}

void P8LogsPage::setSystemEventDateEditsReadOnly(bool readOnly) {
    for (auto* edit : {systemEventStartDateEdit_, systemEventEndDateEdit_}) {
        edit->setReadOnly(readOnly);
        edit->setProperty("state", readOnly ? QStringLiteral("off") : QVariant());
        repolish(edit);
    }
}

void P8LogsPage::fetchSystemEventLog() {
    if (systemEventStartDateEdit_ == nullptr || netManager_ == nullptr) return;

    QUrl url(serverUrl_ + QStringLiteral("/system-events"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("start"), systemEventStartDateEdit_->date().toString(Qt::ISODate));
    query.addQueryItem(QStringLiteral("end"), systemEventEndDateEdit_->date().toString(Qt::ISODate));
    if (!systemEventSearchText_.trimmed().isEmpty()) {
        query.addQueryItem(QStringLiteral("q"), systemEventSearchText_.trimmed());
    }
    url.setQuery(query);

    QNetworkReply* reply = netManager_->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleSystemEventLogReply(reply);
        reply->deleteLater();
    });
}

void P8LogsPage::handleSystemEventLogReply(QNetworkReply* reply) {
    systemEventLog_.clear();
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 200) {
        for (const QJsonValue& value : QJsonDocument::fromJson(reply->readAll()).array()) {
            const QJsonObject obj = value.toObject();

            const QString iso = obj.value("occurred_at").toString();
            QDateTime occurredAt = QDateTime::fromString(iso, Qt::ISODateWithMs);
            if (!occurredAt.isValid()) occurredAt = QDateTime::fromString(iso, Qt::ISODate);

            SystemEventLogRow row;
            row.time = occurredAt.toString(QStringLiteral("MM.dd HH:mm"));
            row.robotCode = obj.value("robot_code").toString(QStringLiteral("-"));
            row.eventType = obj.value("event_type").toString();
            row.message = obj.value("message").toString();
            row.highlight = obj.value("highlight").toBool();
            systemEventLog_.push_back(row);
        }
    }
    refreshSystemEventTable();
}

void P8LogsPage::refreshSystemEventTable() {
    if (systemEventTable_ == nullptr) return;

    systemEventTable_->setRowCount(static_cast<int>(systemEventLog_.size()));
    for (int r = 0; r < static_cast<int>(systemEventLog_.size()); ++r) {
        const SystemEventLogRow& log = systemEventLog_[static_cast<size_t>(r)];

        auto* timeItem = new QTableWidgetItem(log.time);
        timeItem->setFont(QFont(Tokens::Font::MONO));
        timeItem->setTextAlignment(Qt::AlignCenter);
        timeItem->setForeground(Tokens::TEXT_SECONDARY);
        systemEventTable_->setItem(r, 0, timeItem);

        auto* robotItem = new QTableWidgetItem(log.robotCode);
        robotItem->setTextAlignment(Qt::AlignCenter);
        systemEventTable_->setItem(r, 1, robotItem);

        auto* typeItem = new QTableWidgetItem(log.eventType);
        typeItem->setTextAlignment(Qt::AlignCenter);
        if (log.highlight) {
            typeItem->setForeground(Tokens::EMERGENCY.main);
            QFont f = typeItem->font();
            f.setWeight(QFont::DemiBold);
            typeItem->setFont(f);
        }
        systemEventTable_->setItem(r, 2, typeItem);

        auto* messageItem = new QTableWidgetItem(log.message);
        messageItem->setTextAlignment(Qt::AlignCenter);
        systemEventTable_->setItem(r, 3, messageItem);
    }
}

// ---------- 경로순찰 로그 탭 — buildAuditTab()과 같은 필터 UI 관례를 그대로 따른다 ----------

QWidget* P8LogsPage::buildPatrolRunLogTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto* filterRow = new QWidget(tab);
    auto* filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);

    const QDate today = QDate::currentDate();

    auto* periodSeg = new QWidget(filterRow);
    auto* periodSegLayout = new QHBoxLayout(periodSeg);
    periodSegLayout->setContentsMargins(0, 0, 0, 0);
    periodSegLayout->setSpacing(0);

    auto* periodTodayBtn = new QPushButton(QStringLiteral("오늘"), periodSeg);
    auto* periodWeekBtn = new QPushButton(QStringLiteral("7일"), periodSeg);
    auto* periodMonthBtn = new QPushButton(QStringLiteral("30일"), periodSeg);
    patrolRunPeriodCustomBtn_ = new QPushButton(QStringLiteral("직접"), periodSeg);
    periodTodayBtn->setProperty("pos", "first");
    patrolRunPeriodCustomBtn_->setProperty("pos", "last");

    auto* periodGroup = new QButtonGroup(this);
    periodGroup->setExclusive(true);
    for (auto* btn : {periodTodayBtn, periodWeekBtn, periodMonthBtn, patrolRunPeriodCustomBtn_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        periodSegLayout->addWidget(btn);
        periodGroup->addButton(btn);
    }
    periodWeekBtn->setChecked(true);
    filterLayout->addWidget(periodSeg);

    connect(periodTodayBtn, &QPushButton::clicked, this, [this] { applyPatrolRunPeriodPreset(0); });
    connect(periodWeekBtn, &QPushButton::clicked, this, [this] { applyPatrolRunPeriodPreset(6); });
    connect(periodMonthBtn, &QPushButton::clicked, this, [this] { applyPatrolRunPeriodPreset(29); });
    connect(patrolRunPeriodCustomBtn_, &QPushButton::clicked, this,
            [this] { applyPatrolRunPeriodPreset(-1); });

    patrolRunStartDateEdit_ = new CareGuardDateEdit(today, filterRow);
    CareGuard::applyCareGuardDateEdit(patrolRunStartDateEdit_);
    filterLayout->addWidget(patrolRunStartDateEdit_);

    filterLayout->addWidget(makeMutedLabel(QStringLiteral("~"), filterRow));
    patrolRunEndDateEdit_ = new CareGuardDateEdit(today, filterRow);
    CareGuard::applyCareGuardDateEdit(patrolRunEndDateEdit_);
    filterLayout->addWidget(patrolRunEndDateEdit_);

    connect(patrolRunStartDateEdit_, &QDateEdit::dateChanged, this,
            &P8LogsPage::onPatrolRunDateRangeChanged);
    connect(patrolRunEndDateEdit_, &QDateEdit::dateChanged, this,
            &P8LogsPage::onPatrolRunDateRangeChanged);
    patrolRunStartDateEdit_->installEventFilter(this);
    patrolRunEndDateEdit_->installEventFilter(this);

    applyPatrolRunPeriodPreset(6);

    patrolRunSearchEdit_ = new LiveSearchLineEdit(filterRow);
    patrolRunSearchEdit_->setPlaceholderText(QStringLiteral("로봇·대상자·경로·상태 검색"));
    connect(patrolRunSearchEdit_, &LiveSearchLineEdit::liveTextChanged, this, [this](const QString& text) {
        patrolRunSearchText_ = text;
        fetchPatrolRunLog();
    });
    filterLayout->addWidget(patrolRunSearchEdit_, /*stretch=*/1);
    layout->addWidget(filterRow);

    patrolRunTable_ = new QTableWidget(tab);
    const QStringList headers = {QStringLiteral("시작 시각"), QStringLiteral("로봇"), QStringLiteral("대상자"),
                                  QStringLiteral("경로"),     QStringLiteral("유형"), QStringLiteral("상태"),
                                  QStringLiteral("진행")};
    patrolRunTable_->setColumnCount(headers.size());
    patrolRunTable_->setHorizontalHeaderLabels(headers);
    patrolRunTable_->verticalHeader()->setVisible(false);
    patrolRunTable_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    patrolRunTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    patrolRunTable_->setSelectionMode(QAbstractItemView::NoSelection);
    patrolRunTable_->setFocusPolicy(Qt::NoFocus);
    patrolRunTable_->setShowGrid(false);

    auto* prHeader = patrolRunTable_->horizontalHeader();
    prHeader->setSectionResizeMode(QHeaderView::Interactive);
    prHeader->setSectionResizeMode(3, QHeaderView::Stretch);
    patrolRunTable_->setColumnWidth(0, 140);
    patrolRunTable_->setColumnWidth(1, 70);
    patrolRunTable_->setColumnWidth(2, 110);
    patrolRunTable_->setColumnWidth(4, 60);
    patrolRunTable_->setColumnWidth(5, 60);
    patrolRunTable_->setColumnWidth(6, 60);
    layout->addWidget(patrolRunTable_, /*stretch=*/1);

    return tab;
}

void P8LogsPage::onPatrolRunDateRangeChanged() {
    if (patrolRunStartDateEdit_->date() > patrolRunEndDateEdit_->date()) {
        patrolRunEndDateEdit_->setDate(patrolRunStartDateEdit_->date());
        return;
    }
    fetchPatrolRunLog();
}

void P8LogsPage::applyPatrolRunPeriodPreset(int spanDays) {
    if (spanDays >= 0) {
        const QDate today = QDate::currentDate();
        patrolRunStartDateEdit_->setDate(today.addDays(-spanDays));
        patrolRunEndDateEdit_->setDate(today);
        setPatrolRunDateEditsReadOnly(true);
    } else {
        setPatrolRunDateEditsReadOnly(false);
        if (!patrolRunPeriodCustomBtn_->isChecked()) {
            patrolRunPeriodCustomBtn_->setChecked(true);
        }
    }
}

void P8LogsPage::setPatrolRunDateEditsReadOnly(bool readOnly) {
    for (auto* edit : {patrolRunStartDateEdit_, patrolRunEndDateEdit_}) {
        edit->setReadOnly(readOnly);
        edit->setProperty("state", readOnly ? QStringLiteral("off") : QVariant());
        repolish(edit);
    }
}

void P8LogsPage::fetchPatrolRunLog() {
    if (patrolRunStartDateEdit_ == nullptr || netManager_ == nullptr) return;

    QUrl url(serverUrl_ + QStringLiteral("/patrol-runs/log"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("start"), patrolRunStartDateEdit_->date().toString(Qt::ISODate));
    query.addQueryItem(QStringLiteral("end"), patrolRunEndDateEdit_->date().toString(Qt::ISODate));
    if (!patrolRunSearchText_.trimmed().isEmpty()) {
        query.addQueryItem(QStringLiteral("q"), patrolRunSearchText_.trimmed());
    }
    url.setQuery(query);

    QNetworkReply* reply = netManager_->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handlePatrolRunLogReply(reply);
        reply->deleteLater();
    });
}

void P8LogsPage::handlePatrolRunLogReply(QNetworkReply* reply) {
    patrolRunLog_.clear();
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 200) {
        for (const QJsonValue& value : QJsonDocument::fromJson(reply->readAll()).array()) {
            const QJsonObject obj = value.toObject();

            const QString iso = obj.value("started_at").toString();
            QDateTime startedAt = QDateTime::fromString(iso, Qt::ISODateWithMs);
            if (!startedAt.isValid()) startedAt = QDateTime::fromString(iso, Qt::ISODate);

            PatrolRunLogRow row;
            row.time = startedAt.toString(QStringLiteral("MM.dd HH:mm"));
            row.robotCode = obj.value("robot_code").toString(QStringLiteral("-"));
            row.recipient = obj.value("recipient_name").toString(QStringLiteral("-"));
            row.route = obj.value("route_name").toString(QStringLiteral("-"));
            row.trigger = obj.value("trigger").toString();
            row.status = obj.value("status").toString();
            row.progress = QStringLiteral("%1/%2")
                                .arg(obj.value("waypoints_done").toInt())
                                .arg(obj.value("waypoints_total").toInt());
            row.highlight = obj.value("highlight").toBool();
            patrolRunLog_.push_back(row);
        }
    }
    refreshPatrolRunTable();
}

void P8LogsPage::refreshPatrolRunTable() {
    if (patrolRunTable_ == nullptr) return;

    patrolRunTable_->setRowCount(static_cast<int>(patrolRunLog_.size()));
    for (int r = 0; r < static_cast<int>(patrolRunLog_.size()); ++r) {
        const PatrolRunLogRow& log = patrolRunLog_[static_cast<size_t>(r)];

        auto* timeItem = new QTableWidgetItem(log.time);
        timeItem->setFont(QFont(Tokens::Font::MONO));
        timeItem->setTextAlignment(Qt::AlignCenter);
        timeItem->setForeground(Tokens::TEXT_SECONDARY);
        patrolRunTable_->setItem(r, 0, timeItem);

        auto* robotItem = new QTableWidgetItem(log.robotCode);
        robotItem->setTextAlignment(Qt::AlignCenter);
        patrolRunTable_->setItem(r, 1, robotItem);

        auto* recipientItem = new QTableWidgetItem(log.recipient);
        recipientItem->setTextAlignment(Qt::AlignCenter);
        patrolRunTable_->setItem(r, 2, recipientItem);

        auto* routeItem = new QTableWidgetItem(log.route);
        routeItem->setTextAlignment(Qt::AlignCenter);
        patrolRunTable_->setItem(r, 3, routeItem);

        auto* triggerItem = new QTableWidgetItem(log.trigger);
        triggerItem->setTextAlignment(Qt::AlignCenter);
        patrolRunTable_->setItem(r, 4, triggerItem);

        auto* statusItem = new QTableWidgetItem(log.status);
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (log.highlight) {
            statusItem->setForeground(Tokens::EMERGENCY.main);
            QFont f = statusItem->font();
            f.setWeight(QFont::DemiBold);
            statusItem->setFont(f);
        }
        patrolRunTable_->setItem(r, 5, statusItem);

        auto* progressItem = new QTableWidgetItem(log.progress);
        progressItem->setFont(QFont(Tokens::Font::MONO));
        progressItem->setTextAlignment(Qt::AlignCenter);
        patrolRunTable_->setItem(r, 6, progressItem);
    }
}
