#include "p1_overview_page.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "dialogs/robot_control_modal.h"
#include "net/ws_client.h"
#include "theme/tokens.h"

namespace {

constexpr int kBarColorRole = Qt::UserRole + 1;

// ApiClient::parseEvent()가 하는 것과 같은 ISO 파싱(서버 datetime.isoformat()의
// 마이크로초(6자리)를 QDateTime이 기대하는 밀리초(3자리)로 자름) — 그건 api_client.cpp
// 익명 네임스페이스 안에 있어 여기서 못 가져다 쓴다. WS로 오는 event_created 하나만
// 파싱하면 되므로, ApiClient에 새 public 함수를 추가하기보다 여기 짧게 복제한다.
QDateTime parseIsoDateTimeLocal(const QString& raw) {
    if (raw.isEmpty()) return QDateTime();
    QString trimmed = raw;
    static const QRegularExpression fracRe(QStringLiteral("\\.(\\d{3})\\d*"));
    trimmed.replace(fracRe, QStringLiteral(".\\1"));
    QDateTime dt = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (!dt.isValid()) dt = QDateTime::fromString(trimmed, Qt::ISODate);
    return dt;
}

void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QColor levelMainColor(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return Tokens::EMERGENCY.main;
        case Tokens::Level::Attention: return Tokens::ATTENTION.main;
        case Tokens::Level::Offline:   return Tokens::OFFLINE.main;
        case Tokens::Level::Normal:    return Tokens::TEXT_PRIMARY;
    }
    return Tokens::TEXT_PRIMARY;
}

QColor levelSoftColor(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return Tokens::EMERGENCY.soft;
        case Tokens::Level::Attention: return Tokens::ATTENTION.soft;
        case Tokens::Level::Offline:   return Tokens::OFFLINE.soft;
        case Tokens::Level::Normal:    return Tokens::BG_SURFACE;
    }
    return Tokens::BG_SURFACE;
}

QString levelLabel(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return QStringLiteral("긴급");
        case Tokens::Level::Attention: return QStringLiteral("주의");
        case Tokens::Level::Offline:   return QStringLiteral("오프라인");
        case Tokens::Level::Normal:    return QStringLiteral("정상");
    }
    return QString();
}

// 테이블 "상태" 컬럼 전용: 좌측 4px 상태 바를 셀 위에 덧그린다
// (행 배경·좌측 바·펄스는 QSS가 아닌 델리게이트로 구현 — careguard.qss §테이블 주석 참고).
class StatusBarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);

        const QVariant barColor = index.data(kBarColorRole);
        if (!barColor.canConvert<QColor>()) {
            return;
        }
        painter->save();
        painter->fillRect(option.rect.left(), option.rect.top(), Tokens::Layout::STATUS_BAR_W,
                           option.rect.height(), barColor.value<QColor>());
        painter->restore();
    }
};

// 복약 3단계 진행 표시(투약 준비 → 개봉 → 복용 확인). QSS에 없는 전용 위젯이라 직접 그린다.
class MedStepsWidget : public QWidget {
public:
    explicit MedStepsWidget(int completedSteps, QWidget* parent = nullptr)
        : QWidget(parent), completed_(completedSteps) {
        setFixedSize(44, 16);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        constexpr int kDot = 8;
        constexpr int kGap = 6;
        const int totalW = kDot * 3 + kGap * 2;
        int x = (width() - totalW) / 2;
        const int y = (height() - kDot) / 2;
        for (int i = 0; i < 3; ++i) {
            const bool done = i < completed_;
            painter.setPen(Qt::NoPen);
            painter.setBrush(done ? Tokens::OK.main : Tokens::BORDER_STRONG);
            painter.drawEllipse(x, y, kDot, kDot);
            x += kDot + kGap;
        }
    }

private:
    int completed_;
};

QWidget* wrapCellWidget(QWidget* inner, const QColor& rowBg) {
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

QLabel* mutedDash() {
    auto* label = new QLabel(Tokens::Format::EMPTY);
    label->setProperty("role", "muted");
    label->setAlignment(Qt::AlignCenter);
    return label;
}

QLabel* makeBadge(const char* level, const QString& text) {
    auto* badge = new QLabel(text);
    badge->setObjectName("badge");
    badge->setProperty("level", level);
    badge->setAlignment(Qt::AlignCenter);
    return badge;
}

// server/app/schemas.py::RecipientItem.level 대응. p3_recipient_page.cpp::riskLevelFromCode와
// 같은 매핑 — "offline"은 여기 없다(로봇 온라인 여부는 별도로 합성한다).
Tokens::Level riskLevelFromCode(const QString& code) {
    if (code == QStringLiteral("emergency")) return Tokens::Level::Emergency;
    if (code == QStringLiteral("attention")) return Tokens::Level::Attention;
    return Tokens::Level::Normal;
}

}  // namespace

P1OverviewPage::P1OverviewPage(QWidget* parent) : QWidget(parent) {
    apiClient_ = new ApiClient(this);

    batteryRefreshTimer_ = new QTimer(this);
    batteryRefreshTimer_->setSingleShot(true);
    connect(batteryRefreshTimer_, &QTimer::timeout, this, &P1OverviewPage::rebuildRowsFromData);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    layout->addWidget(buildHeaderRow());
    kpiRow_ = buildKpiRow();
    layout->addWidget(kpiRow_);
    layout->addWidget(buildTableSection(), /*stretch=*/2);
    layout->addWidget(buildAlertSection(), /*stretch=*/1);

    populateKpiRow();
    populateTable();
    populateAlertFeed();
    loadFromServer();
}

void P1OverviewPage::setWsClient(WsClient* client) {
    wsClient_ = client;
    connect(wsClient_, &WsClient::messageReceived, this, &P1OverviewPage::handleWsMessage);
}

QWidget* P1OverviewPage::buildHeaderRow() {
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("통합 관제"), row);
    title->setObjectName("pageTitle");
    rowLayout->addWidget(title);
    rowLayout->addStretch();

    return row;
}

QWidget* P1OverviewPage::buildKpiRow() {
    // 카드 내용은 실제 데이터가 로드되기 전에 populateKpiRow()가 채운다 — 여긴 빈
    // 레이아웃만 만들어서 kpiRow_에 보관해두고, 데이터가 (재)로드될 때마다 자식을
    // 지우고 다시 채운다(populateTable()과 같은 패턴).
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(Tokens::Layout::CARD_GAP);
    return row;
}

void P1OverviewPage::populateKpiRow() {
    auto* rowLayout = qobject_cast<QHBoxLayout*>(kpiRow_->layout());
    QLayoutItem* item;
    while ((item = rowLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    const int totalRobots = robots_.size();
    int onlineRobots = 0, offlineRobots = 0;
    for (const auto& robot : robots_) {
        if (robot.isOnline) ++onlineRobots; else ++offlineRobots;
    }

    int unresolvedEmergency = 0;
    for (const auto& event : events_) {
        if (!event.resolved && event.severity == QStringLiteral("emergency")) ++unresolvedEmergency;
    }

    int medMissedRecipients = 0;
    for (const auto& recipient : recipients_) {
        for (const auto& med : recipient.medsToday) {
            if (med.state == QStringLiteral("missed")) {
                ++medMissedRecipients;
                break;
            }
        }
    }

    // 2026-09-12 — "웨어러블 이상" 카드는 뺐다. 이 프로젝트에 웨어러블 기기·데이터
    // 자체가 없어(DB 테이블도, API도 없음) 실제 값을 만들 방법이 없다.
    struct KpiSpec {
        QString label;
        QString value;
        const char* level;  // nullptr = 기본 흰 배경
    };
    const KpiSpec specs[] = {
        {QStringLiteral("가동 로봇"), QStringLiteral("%1/%2").arg(onlineRobots).arg(totalRobots), nullptr},
        {QStringLiteral("미처리 경보"), QString::number(unresolvedEmergency),
         unresolvedEmergency > 0 ? Tokens::Prop::EMERGENCY : nullptr},
        {QStringLiteral("복약 미수령"), QString::number(medMissedRecipients),
         medMissedRecipients > 0 ? Tokens::Prop::ATTENTION : nullptr},
        {QStringLiteral("통신 두절"), QString::number(offlineRobots), nullptr},
    };

    for (const auto& spec : specs) {
        auto* card = new QFrame(kpiRow_);
        card->setObjectName("kpiCard");
        if (spec.level != nullptr) {
            card->setProperty("level", spec.level);
        }

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, 10, Tokens::Layout::CARD_PADDING, 10);
        cardLayout->setSpacing(4);

        auto* label = new QLabel(spec.label, card);
        label->setProperty("role", "caption");
        cardLayout->addWidget(label);

        auto* value = new QLabel(spec.value, card);
        value->setObjectName("kpiValue");
        QFont valueFont = value->font();
        valueFont.setPixelSize(17);
        valueFont.setWeight(QFont::DemiBold);
        value->setFont(valueFont);
        cardLayout->addWidget(value);

        rowLayout->addWidget(card);
    }
}

QWidget* P1OverviewPage::buildTableSection() {
    auto* section = new QWidget(this);
    auto* sectionLayout = new QVBoxLayout(section);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(8);

    auto* headerRow = new QWidget(section);
    auto* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    auto* sectionTitle = new QLabel(QStringLiteral("로봇·대상자 현황"), headerRow);
    sectionTitle->setProperty("role", "h3");
    headerLayout->addWidget(sectionTitle);
    headerLayout->addStretch();

    auto* sortGroup = new QButtonGroup(headerRow);
    const struct { QString text; SortMode mode; const char* pos; } sortSpecs[] = {
        {QStringLiteral("위험순"), SortMode::Risk, "first"},
        {QStringLiteral("로봇ID"), SortMode::RobotId, ""},
        {QStringLiteral("이름순"), SortMode::Name, "last"},
    };
    for (const auto& s : sortSpecs) {
        auto* btn = new QPushButton(s.text, headerRow);
        btn->setObjectName("segBtn");
        btn->setProperty("pos", s.pos);
        btn->setCheckable(true);
        btn->setChecked(s.mode == SortMode::Risk);
        sortGroup->addButton(btn, static_cast<int>(s.mode));
        headerLayout->addWidget(btn);
    }
    connect(sortGroup, &QButtonGroup::idClicked, this, [this](int id) {
        currentSortMode_ = static_cast<SortMode>(id);
        sortRows(currentSortMode_);
        populateTable();
    });

    sectionLayout->addWidget(headerRow);

    table_ = new QTableWidget(section);
    const QStringList headers = {QStringLiteral("상태"),   QStringLiteral("로봇ID"),
                                  QStringLiteral("배터리%"), QStringLiteral("대상자"),
                                  QStringLiteral("복약"),
                                  QStringLiteral("현재 미션"), QStringLiteral("가정(주소)"),
                                  QStringLiteral("액션"),    QStringLiteral("정지")};
    table_->setColumnCount(headers.size());
    table_->setHorizontalHeaderLabels(headers);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setShowGrid(false);
    table_->setItemDelegateForColumn(0, new StatusBarDelegate(table_));
    table_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(6, QHeaderView::Stretch);  // 가정(주소) — 남는 폭을 전부 흡수
    table_->setColumnWidth(0, 135);   // 상태
    table_->setColumnWidth(1, 115);   // 로봇ID
    table_->setColumnWidth(2, 155);   // 배터리
    table_->setColumnWidth(3, 135);   // 대상자
    table_->setColumnWidth(4, 120);   // 복약
    table_->setColumnWidth(5, 420);   // 현재 미션
    table_->setColumnWidth(7, 80);    // 액션
    table_->setColumnWidth(8, 80);    // 정지

    sectionLayout->addWidget(table_);
    return section;
}

QWidget* P1OverviewPage::buildAlertSection() {
    auto* section = new QWidget(this);
    auto* sectionLayout = new QVBoxLayout(section);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(8);

    auto* sectionTitle = new QLabel(QStringLiteral("실시간 알림"), section);
    sectionTitle->setProperty("role", "h3");
    sectionLayout->addWidget(sectionTitle);

    alertList_ = new QListWidget(section);
    alertList_->setObjectName("alertList");
    alertList_->setFocusPolicy(Qt::NoFocus);
    alertList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    alertList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 내용(alerts_)은 populateAlertFeed()가 채운다 — GET /events가 돌아오는 대로,
    // 그리고 이후 로드마다 다시 채워야 해서(populateTable()과 같은 이유) 여기서는
    // 빈 리스트만 만든다.

    sectionLayout->addWidget(alertList_);
    return section;
}

void P1OverviewPage::populateAlertFeed() {
    // events_는 GET /events 응답 순서를 그대로 안 믿고(서버가 정렬을 보장 안 함) 여기서
    // occurredAt 내림차순으로 다시 정렬한다 — 최신 알림이 위로 오는 게 이 피드의 목적.
    QVector<ApiClient::EventInfo> sorted = events_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const ApiClient::EventInfo& a, const ApiClient::EventInfo& b) {
        return a.occurredAt > b.occurredAt;
    });

    // events_ 자체(GET /events)는 날짜 제한이 없는 전체 이력이라, kMaxAlerts로만
    // 자르면 오늘 이벤트가 적은 날엔 어제 이전 이벤트가 섞여 들어온다("실시간 알림"
    // 이름과 안 맞음, 2026-09-14 리포트). rebuildRowsFromData가 쓰는 "진행 중 이벤트"
    // (latestUnresolvedEventByRobot)는 미해결 이벤트를 날짜 상관없이 계속 보여줘야
    // 하므로 events_ 자체는 안 건드리고, 이 피드에서만 오늘 날짜(로컬 기준)로 거른다.
    const QDate today = QDate::currentDate();

    alerts_.clear();
    constexpr int kMaxAlerts = 20;
    for (const auto& event : sorted) {
        if (event.occurredAt.toLocalTime().date() != today) continue;
        if (alerts_.size() >= kMaxAlerts) break;
        const char* level = Tokens::Prop::INFO;
        if (event.severity == QStringLiteral("emergency")) level = Tokens::Prop::EMERGENCY;
        else if (event.severity == QStringLiteral("attention")) level = Tokens::Prop::ATTENTION;

        const QString who = !event.robotCode.isEmpty() ? event.robotCode : event.recipientName;
        QString text = event.eventTypeLabel;
        if (!event.summary.isEmpty()) text += QStringLiteral(" — ") + event.summary;
        if (!who.isEmpty()) text = who + QStringLiteral(" ") + text;

        alerts_.push_back({event.occurredAt.toString(QStringLiteral("HH:mm:ss")), level, text});
    }

    alertList_->clear();
    constexpr int kAlertRowH = 36;
    for (const auto& alert : alerts_) {
        auto* row = new QWidget(alertList_);
        row->setFixedHeight(kAlertRowH);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 2, 2, 2);
        rowLayout->setSpacing(10);

        auto* time = new QLabel(alert.time, row);
        time->setProperty("role", "mono");
        time->setFixedWidth(64);
        rowLayout->addWidget(time);
        rowLayout->setAlignment(time, Qt::AlignVCenter);

        QString badgeText = QStringLiteral("정보");
        if (alert.level == Tokens::Prop::EMERGENCY) badgeText = QStringLiteral("긴급");
        else if (alert.level == Tokens::Prop::ATTENTION) badgeText = QStringLiteral("주의");
        auto* badge = makeBadge(alert.level, badgeText);
        badge->setProperty("size", "xs");
        rowLayout->addWidget(badge);
        rowLayout->setAlignment(badge, Qt::AlignVCenter);

        auto* text = new QLabel(alert.text, row);
        rowLayout->addWidget(text, /*stretch=*/1);
        rowLayout->setAlignment(text, Qt::AlignVCenter);

        auto* item = new QListWidgetItem(alertList_);
        item->setSizeHint(QSize(row->sizeHint().width(), kAlertRowH));
        alertList_->addItem(item);
        alertList_->setItemWidget(item, row);
    }
}

void P1OverviewPage::sortRows(SortMode mode) {
    switch (mode) {
        case SortMode::Risk:
            std::stable_sort(rows_.begin(), rows_.end(), [](const RobotRow& a, const RobotRow& b) {
                return static_cast<int>(a.level) > static_cast<int>(b.level);
            });
            break;
        case SortMode::RobotId:
            std::stable_sort(rows_.begin(), rows_.end(),
                              [](const RobotRow& a, const RobotRow& b) { return a.id < b.id; });
            break;
        case SortMode::Name:
            std::stable_sort(rows_.begin(), rows_.end(),
                              [](const RobotRow& a, const RobotRow& b) { return a.recipient < b.recipient; });
            break;
    }
}

void P1OverviewPage::populateTable() {
    table_->setRowCount(static_cast<int>(rows_.size()));

    for (int r = 0; r < static_cast<int>(rows_.size()); ++r) {
        const RobotRow& row = rows_[static_cast<size_t>(r)];
        const bool offline = row.level == Tokens::Level::Offline;
        const QColor rowBg = levelSoftColor(row.level);
        const QColor textColor = offline ? Tokens::OFFLINE.main : Tokens::TEXT_PRIMARY;

        auto* statusItem = new QTableWidgetItem(levelLabel(row.level));
        statusItem->setTextAlignment(Qt::AlignCenter);
        statusItem->setBackground(rowBg);
        statusItem->setForeground(levelMainColor(row.level));
        if (row.level == Tokens::Level::Emergency || row.level == Tokens::Level::Attention) {
            statusItem->setData(kBarColorRole, levelMainColor(row.level));
        }
        table_->setItem(r, 0, statusItem);

        auto* idItem = new QTableWidgetItem(row.id);
        QFont monoFont(Tokens::Font::MONO);
        idItem->setFont(monoFont);
        idItem->setTextAlignment(Qt::AlignCenter);
        idItem->setBackground(rowBg);
        idItem->setForeground(textColor);
        table_->setItem(r, 1, idItem);

        if (row.battery >= 0) {
            auto* bar = new QProgressBar();
            bar->setRange(0, 100);
            bar->setValue(row.battery);
            bar->setTextVisible(false);
            bar->setFixedWidth(70);
            if (row.battery < 20) bar->setProperty("level", Tokens::Prop::EMERGENCY);
            else if (row.battery < 40) bar->setProperty("level", Tokens::Prop::ATTENTION);
            repolish(bar);

            auto* batteryCell = new QWidget();
            auto* batteryLayout = new QHBoxLayout(batteryCell);
            batteryLayout->setContentsMargins(0, 0, 0, 0);
            batteryLayout->setSpacing(6);
            batteryLayout->addWidget(bar);
            auto* batteryLabel = new QLabel(QStringLiteral("%1%").arg(row.battery), batteryCell);
            batteryLabel->setStyleSheet(QStringLiteral("color:%1;").arg(textColor.name()));
            batteryLayout->addWidget(batteryLabel);

            table_->setCellWidget(r, 2, wrapCellWidget(batteryCell, rowBg));
        } else {
            table_->setCellWidget(r, 2, wrapCellWidget(mutedDash(), rowBg));
        }

        auto* recipientItem = new QTableWidgetItem(row.recipient);
        recipientItem->setTextAlignment(Qt::AlignCenter);
        recipientItem->setBackground(rowBg);
        recipientItem->setForeground(textColor);
        table_->setItem(r, 3, recipientItem);

        if (row.medSteps >= 0) {
            table_->setCellWidget(r, 4, wrapCellWidget(new MedStepsWidget(row.medSteps), rowBg));
        } else {
            table_->setCellWidget(r, 4, wrapCellWidget(mutedDash(), rowBg));
        }

        auto* missionItem = new QTableWidgetItem(row.mission);
        missionItem->setTextAlignment(Qt::AlignCenter);
        missionItem->setBackground(rowBg);
        missionItem->setForeground(textColor);
        table_->setItem(r, 5, missionItem);

        auto* addressItem = new QTableWidgetItem(row.address);
        addressItem->setTextAlignment(Qt::AlignCenter);
        addressItem->setBackground(rowBg);
        addressItem->setForeground(textColor);
        table_->setItem(r, 6, addressItem);

        auto* detailBtn = new QPushButton(QStringLiteral("상세"));
        detailBtn->setObjectName("detailBtn");
        detailBtn->setProperty("variant", "secondary");
        connect(detailBtn, &QPushButton::clicked, this, [this, row]() {
            RobotControlModal::Data data;
            data.robotCode = row.id;
            data.headerTitle = QStringLiteral("%1  %2 · %3").arg(row.id, row.address, row.recipient);
            data.urgencyBadge = (row.level == Tokens::Level::Emergency || row.level == Tokens::Level::Attention)
                                     ? levelLabel(row.level)
                                     : QString();
            data.mapLabel = QStringLiteral("%1 맵").arg(row.address);
            data.batteryPct = row.battery;
            data.eventCode = row.eventCode;
            data.eventText = row.eventText;
            // 통신 지연(ms)·카메라/마이크/Nav2 개별 상태·현재 위치는 서버에 그 자체를
            // 재는/보고하는 경로가 없어(§P1OverviewPage.h 상단 주석) 여전히 못 채운다.
            // "최근 미션" 이력도 로봇별로 조회하는 API가 따로 없어 비워둔다 — 지어낸
            // 값을 보여주는 것보단 빈 채로 두는 쪽이 낫다.

            auto* modal = new RobotControlModal(data, this);
            modal->setAttribute(Qt::WA_DeleteOnClose);
            connect(modal, &RobotControlModal::eventNavigationRequested, this,
                    &P1OverviewPage::eventNavigationRequested);
            modal->show();
        });
        table_->setCellWidget(r, 7, wrapCellWidget(detailBtn, rowBg));

        auto* estopBtn = new QPushButton(QStringLiteral("정지"));
        estopBtn->setObjectName("rowEstopBtn");
        if (offline) {
            estopBtn->setEnabled(false);
            estopBtn->setToolTip(QStringLiteral("통신 두절 — 정지 명령 전달 불가"));
        } else {
            // 로봇 개별 비상정지(모터 즉시 정지) API가 서버에 없어, 헤더의 "전체
            // 비상정지"와 같은 이유로 순찰/이동 취소(cancelPatrolRun)로 대신한다.
            connect(estopBtn, &QPushButton::clicked, this, [this, robotCode = row.id]() {
                apiClient_->cancelPatrolRun(robotCode, [this, robotCode](bool ok, const QString& err) {
                    if (ok) {
                        markRobotIdle(robotCode);
                    } else {
                        QMessageBox::warning(this, QStringLiteral("정지 실패"),
                                              err.isEmpty() ? QStringLiteral("%1 연결을 확인해주세요.").arg(robotCode)
                                                            : err);
                    }
                });
            });
        }
        table_->setCellWidget(r, 8, wrapCellWidget(estopBtn, rowBg));
    }
}

void P1OverviewPage::loadFromServer() {
    // 셋 다 서로 독립적으로 로드되고, 하나가 도착할 때마다 rebuildRowsFromData()를
    // 다시 불러 그때까지 모인 데이터로 화면을 갱신한다 — 셋 다 오기를 기다렸다가
    // 한 번에 그리지 않는 이유는, 느린 쪽(대개 이벤트) 하나 때문에 나머지(로봇 목록 등)
    // 까지 화면이 텅 빈 채로 오래 남는 걸 피하기 위해서다.
    apiClient_->fetchRobots([this](bool ok, const QVector<ApiClient::RobotInfo>& items) {
        if (!ok) return;
        robots_ = items;
        rebuildRowsFromData();
        populateKpiRow();
    });
    apiClient_->fetchRecipients([this](bool ok, const QVector<ApiClient::RecipientInfo>& items) {
        if (!ok) return;
        recipients_ = items;
        rebuildRowsFromData();
        populateKpiRow();
    });
    apiClient_->fetchEvents([this](bool ok, const QVector<ApiClient::EventInfo>& items) {
        if (!ok) return;
        events_ = items;
        rebuildRowsFromData();
        populateKpiRow();
        populateAlertFeed();
    });
    refreshPatrolRunStatus();
}

void P1OverviewPage::refreshPatrolRunStatus() {
    // robotId<=0 -> robot_id 필터 없이 전체 로봇의 실행 중(status=running) 목록을
    // 한 번에 받아온다(ApiClient::fetchPatrolRuns 주석 참고).
    apiClient_->fetchPatrolRuns(0, QStringLiteral("running"),
                                [this](bool ok, const QVector<ApiClient::PatrolRunInfo>& runs) {
        if (!ok) return;
        patrollingRobotIds_.clear();
        for (const auto& run : runs) patrollingRobotIds_.insert(run.robotId);
        rebuildRowsFromData();
    });
}

void P1OverviewPage::markRobotIdle(const QString& robotCode) {
    for (const auto& robot : robots_) {
        if (robot.code == robotCode) {
            patrollingRobotIds_.remove(robot.id);
            break;
        }
    }
    rebuildRowsFromData();
}

void P1OverviewPage::handleWsMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString robotCode = message.value(QStringLiteral("robot_code")).toString();

    if (type == QStringLiteral("battery_state")) {
        liveBattery_[robotCode] = static_cast<int>(message.value(QStringLiteral("percentage")).toDouble());
        // 값은 이미 서버가 다듬어서 보내주지만(스무딩은 robot_ws.py 쪽으로 옮김),
        // 테이블을 다시 그리는 것 자체는 계속 디바운스한다 — 여러 대가 동시에
        // battery_state를 쏟아내도 다시 그리기는 최대 1초에 한 번만 일어난다(안 그러면
        // 값이 안정적이어도 테이블 위젯이 자주 재생성되면서 호버 등이 깜빡였다).
        if (!batteryRefreshTimer_->isActive()) batteryRefreshTimer_->start(1000);
        return;
    }

    if (type == QStringLiteral("robot_status")) {
        const bool online = message.value(QStringLiteral("online")).toBool();
        for (auto& robot : robots_) {
            if (robot.code == robotCode) {
                robot.isOnline = online;
                break;
            }
        }
        rebuildRowsFromData();
        populateKpiRow();
        return;
    }

    if (type == QStringLiteral("nav_status")) {
        const bool running = message.value(QStringLiteral("running")).toBool();
        for (auto& robot : robots_) {
            if (robot.code == robotCode) {
                robot.isNavRunning = running;
                break;
            }
        }
        rebuildRowsFromData();
        return;
    }

    if (type == QStringLiteral("patrol_run_created") || type == QStringLiteral("patrol_run_updated")) {
        // 이 창에서 시작/취소한 것이든(낙관적 갱신은 이미 markRobotIdle이 처리) 다른
        // 대시보드 창·scheduler.py 자동 트리거로 시작/취소된 것이든, 서버가 같은
        // 이벤트로 브로드캐스트하므로 구분 없이 실제 상태를 다시 조회해 맞춘다.
        refreshPatrolRunStatus();
        return;
    }

    if (type == QStringLiteral("event_created")) {
        // scheduler.py::_medication_tick()가 care_event를 새로 커밋한 직후 보내는
        // 알림(2026-09-12 추가) — "실시간 알림"이라는 이름에 맞게, 페이지를 새로고침하지
        // 않아도 방금 생긴 이벤트가 바로 목록에 뜨게 한다. 필드 이름은 GET /events가
        // 내려주는 EventItem과 동일하게 맞췄다(ApiClient::parseEvent와 같은 매핑이지만,
        // 그 함수가 api_client.cpp 안에 private이라 여기서 직접 파싱한다).
        ApiClient::EventInfo event;
        event.eventUid = message.value(QStringLiteral("event_uid")).toString();
        event.severity = message.value(QStringLiteral("severity")).toString();
        event.eventType = message.value(QStringLiteral("event_type")).toString();
        event.eventTypeLabel = message.value(QStringLiteral("event_type_label")).toString();
        event.summary = message.value(QStringLiteral("summary")).toString();
        event.occurredAt = parseIsoDateTimeLocal(message.value(QStringLiteral("occurred_at")).toString());
        event.resolved = message.value(QStringLiteral("resolved")).toBool();
        event.robotCode = message.value(QStringLiteral("robot_code")).toString();
        event.recipientName = message.value(QStringLiteral("recipient_name")).toString();

        events_.push_front(event);
        rebuildRowsFromData();
        populateKpiRow();
        populateAlertFeed();
        return;
    }

    if (type == QStringLiteral("recipient_changed") || type == QStringLiteral("robot_recipient_changed")) {
        // recipient.py::update_recipient(이름 등 기본정보 저장)나 robots.py::
        // assign_robot_recipient(담당 로봇 재배정)가 보내는 알림(2026-09-13 추가,
        // P3/P5/P7의 같은 처리와 같은 이유) — recipients_는 loadFromServer()가 시작할
        // 때 한 번만 받아오므로, 이게 없으면 이미 켜져 있던 다른 대시보드 창의
        // 통합관제 테이블은 새로고침(재시작) 전까지 옛 값을 계속 보여줬다.
        apiClient_->fetchRecipients([this](bool ok, const QVector<ApiClient::RecipientInfo>& items) {
            if (!ok) return;
            recipients_ = items;
            rebuildRowsFromData();
            populateKpiRow();
        });
        return;
    }

    if (type == QStringLiteral("system_event_created")) {
        // robot_ws.py가 배터리 임계값 등 로봇 쪽 시스템 이벤트를 감지했을 때 보내는
        // 알림(robot_ws.py:462) — event_created(scheduler.py)와 달리 payload 없이 type만
        // 오므로 바로 반영할 수 없어 목록을 통째로 다시 받아온다(P8LogsPage::handleWsMessage의
        // fetchSystemEventLog()와 같은 이유). 이게 없으면 배터리 주의/복귀 필요 알림이
        // 통합관제 화면에 새로고침 전까지 안 뜬다.
        apiClient_->fetchEvents([this](bool ok, const QVector<ApiClient::EventInfo>& items) {
            if (!ok) return;
            events_ = items;
            rebuildRowsFromData();
            populateKpiRow();
            populateAlertFeed();
        });
        return;
    }
}

void P1OverviewPage::rebuildRowsFromData() {
    // 로봇 코드 -> 대상자 1건. 이 시스템은 로봇 한 대에 대상자 한 명이 배정되는
    // 구조라(RecipientInfo::robotCode) 1:1로 찾으면 된다.
    QMap<QString, const ApiClient::RecipientInfo*> recipientByRobot;
    for (const auto& recipient : recipients_) {
        if (!recipient.robotCode.isEmpty()) recipientByRobot[recipient.robotCode] = &recipient;
    }

    // 로봇 코드 -> 가장 최근 미해결 이벤트. RobotControlModal이 "이벤트로 이동" 버튼에
    // 쓸 이벤트 코드가 필요해서, 로봇별로 하나만 뽑아둔다(여러 건이면 제일 최신 것).
    QMap<QString, const ApiClient::EventInfo*> latestUnresolvedEventByRobot;
    for (const auto& event : events_) {
        if (event.resolved || event.robotCode.isEmpty()) continue;
        auto it = latestUnresolvedEventByRobot.find(event.robotCode);
        if (it == latestUnresolvedEventByRobot.end() || event.occurredAt > (*it)->occurredAt) {
            latestUnresolvedEventByRobot[event.robotCode] = &event;
        }
    }

    rows_.clear();
    for (const auto& robot : robots_) {
        RobotRow row;
        row.id = robot.code;

        const ApiClient::RecipientInfo* recipient = recipientByRobot.value(robot.code, nullptr);
        row.address = recipient && !recipient->homeAddress.isEmpty() ? recipient->homeAddress : QStringLiteral("-");
        row.recipient = recipient && !recipient->name.isEmpty() ? recipient->name : QStringLiteral("미배정");

        // 오프라인이면 배지 색을 무조건 OFFLINE으로 덮어쓴다(p3_recipient_page.cpp의
        // 온라인/오프라인 전환 로직과 같은 규칙) — 긴급/주의 배지가 "지금 이 순간에도
        // 유효한 경보"라는 뜻이라, 통신이 끊겨 최신 상태를 모르는 로봇에 그대로 남겨두면
        // 오해를 준다.
        row.level = robot.isOnline ? (recipient ? riskLevelFromCode(recipient->level) : Tokens::Level::Normal)
                                    : Tokens::Level::Offline;

        // row.level/mission과 같은 이유(바로 위 주석 참고) — 통신이 끊긴 로봇은
        // liveBattery_에 남아있는 마지막 값을 그대로 보여주면 "지금도 이 정도"라고
        // 오해를 준다. 2026-09-14 "LIMO 통신이 끊겼는데 배터리 값이 그대로 뜬다"는
        // 리포트로 실제 확인 — level/mission만 오프라인 처리가 돼 있고 배터리는
        // 빠져 있었다. -1이면 테이블이 "-"로 표시한다(row.battery >= 0 분기 참고).
        row.battery = robot.isOnline ? liveBattery_.value(robot.code, -1) : -1;

        row.medSteps = -1;
        if (recipient) {
            int done = 0;
            bool hasSchedule = false;
            for (const auto& med : recipient->medsToday) {
                if (med.slotNo < 1 || med.slotNo > 3) continue;
                hasSchedule = true;
                if (med.state == QStringLiteral("done")) ++done;
            }
            if (hasSchedule) row.medSteps = done;
        }

        if (!robot.isOnline) row.mission = QStringLiteral("통신 두절");
        else if (patrollingRobotIds_.contains(robot.id)) row.mission = QStringLiteral("순찰/이동 중");
        else row.mission = QStringLiteral("대기 중");

        if (const auto* event = latestUnresolvedEventByRobot.value(robot.code, nullptr)) {
            row.eventCode = event->eventUid;
            row.eventText = event->eventTypeLabel;
            if (!event->summary.isEmpty()) row.eventText += QStringLiteral(" — ") + event->summary;
        }

        rows_.push_back(row);
    }

    sortRows(currentSortMode_);
    populateTable();
}

