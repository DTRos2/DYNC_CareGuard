#include "p2_event_page.h"

#include <QButtonGroup>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include "dialogs/event_detail_modal.h"
#include "net/api_client.h"
#include "theme/calendar_style.h"
#include "theme/tokens.h"
#include "widgets/careguard_date_edit.h"
#include "widgets/event_filter_proxy_model.h"
#include "widgets/event_row_delegate.h"
#include "widgets/event_table_model.h"
#include "widgets/live_search_line_edit.h"
#include "widgets/simple_icons.h"

namespace {

QPushButton* makeToolbarButton(const QString& text, const QIcon& icon, QWidget* parent) {
    auto* btn = new QPushButton(icon, text, parent);
    btn->setProperty("variant", "secondary");
    btn->setProperty("size", "s");
    return btn;
}

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

P2EventPage::P2EventPage(QWidget* parent) : QWidget(parent) {
    apiClient_ = new ApiClient(this);
    model_ = new EventTableModel(this);
    proxy_ = new EventFilterProxyModel(this);
    proxy_->setSourceModel(model_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("이벤트 관제"), this);
    title->setObjectName("pageTitle");

    auto* csvBtn = makeToolbarButton(QStringLiteral("CSV"), SimpleIcons::download(Tokens::TEXT_PRIMARY, 12), this);
    connect(csvBtn, &QPushButton::clicked, this, [this] { showComingSoon(QStringLiteral("CSV 내보내기")); });

    auto* titleRow = new QHBoxLayout();
    titleRow->addWidget(title);
    titleRow->addStretch();
    titleRow->addWidget(csvBtn);
    layout->addLayout(titleRow);

    layout->addWidget(buildFilterBar());
    bulkBar_ = buildBulkBar();
    layout->addWidget(bulkBar_);
    layout->addWidget(buildTable(), /*stretch=*/1);
    layout->addWidget(buildPagination());

    // GET /events가 비동기로 도착하기 전까지는 화면이 비어 보이므로, 응답 오기 전 첫 화면은
    // mock으로 그려두고 loadEventsFromServer()가 도착하는 대로 덮어쓴다(P3 대상자 페이지와
    // 같은 패턴).
    seedMockData();
    loadEventsFromServer();

    connect(model_, &EventTableModel::countsChanged, this, &P2EventPage::refreshCounts);

    elapsedTimer_ = new QTimer(this);
    connect(elapsedTimer_, &QTimer::timeout, model_, &EventTableModel::tickElapsed);
    elapsedTimer_->start(1000);

    pulseTimer_ = new QTimer(this);
    connect(pulseTimer_, &QTimer::timeout, this, [this] {
        pulseOn_ = !pulseOn_;
        tableView_->viewport()->update();
    });
    pulseTimer_->start(Tokens::Motion::PULSE_PERIOD_MS);

    // MainWindow가 unresolvedCountsChanged를 연결하는 건 이 생성자가 끝난 뒤이므로,
    // 여기서 바로 refreshCounts()를 부르면 최초 emit을 아무도 못 듣는다. 다음 이벤트
    // 루프 턴으로 미뤄서 연결이 끝난 뒤 발행되게 한다.
    QTimer::singleShot(0, this, &P2EventPage::refreshCounts);
}

QWidget* P2EventPage::buildFilterBar() {
    auto* bar = new QWidget(this);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    const QDate today = QDate::currentDate();

    layout->addWidget(makeMutedLabel(QStringLiteral("기간"), bar));

    // 기간 세그먼트 (gui/wireframe/v1/p2_filter_bar_period_segment_wireframe.html).
    // "직접" 전에는 세그먼트가 날짜를 계산해서 쓰기만 하고, 날짜 칸은 그 결과를
    // 읽기 전용으로 보여준다.
    auto* periodSeg = new QWidget(bar);
    auto* periodSegLayout = new QHBoxLayout(periodSeg);
    periodSegLayout->setContentsMargins(0, 0, 0, 0);
    periodSegLayout->setSpacing(0);

    auto* periodTodayBtn = new QPushButton(QStringLiteral("오늘"), periodSeg);
    auto* periodWeekBtn = new QPushButton(QStringLiteral("7일"), periodSeg);
    periodMonthBtn_ = new QPushButton(QStringLiteral("30일"), periodSeg);
    periodCustomBtn_ = new QPushButton(QStringLiteral("직접"), periodSeg);
    periodTodayBtn->setProperty("pos", "first");
    periodCustomBtn_->setProperty("pos", "last");

    periodGroup_ = new QButtonGroup(this);
    periodGroup_->setExclusive(true);
    for (auto* btn : {periodTodayBtn, periodWeekBtn, periodMonthBtn_, periodCustomBtn_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        periodSegLayout->addWidget(btn);
        periodGroup_->addButton(btn);
    }
    periodTodayBtn->setChecked(true);
    layout->addWidget(periodSeg);

    connect(periodTodayBtn, &QPushButton::clicked, this, [this] { applyPeriodPreset(0); });
    connect(periodWeekBtn, &QPushButton::clicked, this, [this] { applyPeriodPreset(6); });
    connect(periodMonthBtn_, &QPushButton::clicked, this, [this] { applyPeriodPreset(29); });
    connect(periodCustomBtn_, &QPushButton::clicked, this, [this] { applyPeriodPreset(-1); });

    startDateEdit_ = new CareGuardDateEdit(today, bar);
    CareGuard::applyCareGuardDateEdit(startDateEdit_);
    layout->addWidget(startDateEdit_);

    layout->addWidget(makeMutedLabel(QStringLiteral("~"), bar));
    endDateEdit_ = new CareGuardDateEdit(today, bar);
    CareGuard::applyCareGuardDateEdit(endDateEdit_);
    layout->addWidget(endDateEdit_);

    connect(startDateEdit_, &QDateEdit::dateChanged, this, &P2EventPage::onDateRangeChanged);
    connect(endDateEdit_, &QDateEdit::dateChanged, this, &P2EventPage::onDateRangeChanged);
    // 읽기 전용 칸은 클릭이 편집으로 이어지지 않으니, 클릭 자체를 잡아 "직접"로 전환한다.
    startDateEdit_->installEventFilter(this);
    endDateEdit_->installEventFilter(this);

    applyPeriodPreset(0);  // 초기 상태: 오늘 ~ 오늘, 읽기 전용

    searchEdit_ = new LiveSearchLineEdit(bar);
    searchEdit_->setPlaceholderText(QStringLiteral("이벤트 ID·로봇·대상자·담당자 검색"));
    connect(searchEdit_, &LiveSearchLineEdit::liveTextChanged, proxy_,
            &EventFilterProxyModel::setSearchKeyword);
    layout->addWidget(searchEdit_, /*stretch=*/1);

    unresolvedOnlyChip_ = new QPushButton(QStringLiteral("미처리만 ✓"), bar);
    unresolvedOnlyChip_->setObjectName("routeChip");
    unresolvedOnlyChip_->setProperty("size", "s");
    unresolvedOnlyChip_->setCheckable(true);
    unresolvedOnlyChip_->setChecked(true);
    connect(unresolvedOnlyChip_, &QPushButton::toggled, this, [this](bool checked) {
        unresolvedOnlyChip_->setText(checked ? QStringLiteral("미처리만 ✓") : QStringLiteral("미처리만"));
        proxy_->setUnresolvedOnly(checked);
    });
    layout->addWidget(unresolvedOnlyChip_);

    filterCountLabel_ = makeMutedLabel(QString(), bar);
    layout->addWidget(filterCountLabel_);

    return bar;
}

QWidget* P2EventPage::buildBulkBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName("banner");
    bar->setProperty("level", "info");
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(8);

    bulkCountLabel_ = new QLabel(bar);
    layout->addWidget(bulkCountLabel_);

    auto* completeBtn = new QPushButton(QStringLiteral("일괄 완료 처리"), bar);
    completeBtn->setProperty("variant", "primary");
    completeBtn->setProperty("size", "s");
    connect(completeBtn, &QPushButton::clicked, this, &P2EventPage::onBulkComplete);
    layout->addWidget(completeBtn);

    auto* assignBtn = new QPushButton(QStringLiteral("담당자 배정"), bar);
    assignBtn->setProperty("variant", "secondary");
    assignBtn->setProperty("size", "s");
    connect(assignBtn, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("담당자 배정")); });
    layout->addWidget(assignBtn);

    layout->addStretch();

    auto* clearBtn = new QPushButton(QStringLiteral("선택 해제"), bar);
    clearBtn->setObjectName("linkBtn");
    connect(clearBtn, &QPushButton::clicked, model_, &EventTableModel::clearAllChecks);
    layout->addWidget(clearBtn);

    bar->setVisible(false);
    return bar;
}

QWidget* P2EventPage::buildTable() {
    tableView_ = new QTableView(this);
    tableView_->setModel(proxy_);
    delegate_ = new EventRowDelegate(&pulseOn_, &hoveredRow_, this);
    tableView_->setItemDelegate(delegate_);

    tableView_->setSelectionMode(QAbstractItemView::NoSelection);
    tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView_->setFocusPolicy(Qt::NoFocus);
    tableView_->setMouseTracking(true);
    tableView_->viewport()->installEventFilter(this);
    tableView_->setShowGrid(false);
    tableView_->setAlternatingRowColors(false);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    tableView_->horizontalHeader()->setHighlightSections(false);
    tableView_->horizontalHeader()->setStretchLastSection(false);

    tableView_->setColumnWidth(EventTableModel::ColCheck, 32);
    tableView_->setColumnWidth(EventTableModel::ColSeverity, 90);
    tableView_->setColumnWidth(EventTableModel::ColEventId, 130);
    tableView_->setColumnWidth(EventTableModel::ColTime, 70);
    tableView_->setColumnWidth(EventTableModel::ColRobot, 60);
    tableView_->setColumnWidth(EventTableModel::ColRecipient, 70);
    tableView_->setColumnWidth(EventTableModel::ColType, 100);
    tableView_->setColumnWidth(EventTableModel::ColStatus, 80);
    tableView_->setColumnWidth(EventTableModel::ColAssignee, 70);
    tableView_->setColumnWidth(EventTableModel::ColElapsed, 80);
    tableView_->horizontalHeader()->setSectionResizeMode(EventTableModel::ColSummary,
                                                          QHeaderView::Stretch);

    proxy_->sort(0);

    connect(tableView_, &QTableView::clicked, this, &P2EventPage::onCellClicked);

    return tableView_;
}

QWidget* P2EventPage::buildPagination() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addStretch();

    auto* prev = new QPushButton(SimpleIcons::chevronLeft(Tokens::TEXT_SECONDARY, 11), QString(), row);
    prev->setObjectName("segBtn");
    prev->setProperty("pos", "first");
    layout->addWidget(prev);

    auto* page1 = new QPushButton(QStringLiteral("1"), row);
    page1->setObjectName("segBtn");
    page1->setCheckable(true);
    page1->setChecked(true);
    layout->addWidget(page1);

    auto* next = new QPushButton(SimpleIcons::chevronRight(Tokens::TEXT_SECONDARY, 11), QString(), row);
    next->setObjectName("segBtn");
    next->setProperty("pos", "last");
    layout->addWidget(next);

    layout->addStretch();
    return row;
}

void P2EventPage::seedMockData() {
    using Row = EventTableModel::EventRow;
    using Sev = EventTableModel::Severity;

    const QDate today = QDate::currentDate();
    const QDate yesterday = today.addDays(-1);

    QVector<Row> rows;
    rows.append(Row{Sev::Emergency, QStringLiteral("EV-260826-014"), today, QStringLiteral("14:29"),
                     QStringLiteral("R-01"), QStringLiteral("김순자"), QStringLiteral("안부 무응답"),
                     QStringLiteral("안부 질문 2회 무응답 → 출동 · 음성 확인 대기"),
                     QStringLiteral("도희정"), false, 192, false});
    rows.append(Row{Sev::Attention, QStringLiteral("EV-260826-011"), today, QStringLiteral("12:41"),
                     QStringLiteral("R-02"), QStringLiteral("박영호"), QStringLiteral("복약 미수령"),
                     QStringLiteral("아침 약 미수령 (대상자 미발견) · 보호자 문자 발송"), QString(),
                     false, 6667, false});
    rows.append(Row{Sev::Attention, QStringLiteral("EV-260826-012"), today, QStringLiteral("13:58"),
                     QStringLiteral("R-04"), QStringLiteral("최금례"), QStringLiteral("통신 단절"),
                     QStringLiteral("관제 연결 끊김 · 30초 주기 재시도 · 로컬 기록 유지"), QString(),
                     false, 2042, false});
    rows.append(Row{Sev::Info, QStringLiteral("EV-260826-010"), today, QStringLiteral("12:34"),
                     QStringLiteral("R-03"), QStringLiteral("이말순"), QStringLiteral("복약 완료"),
                     QStringLiteral("점심 약 수령 확인 · event_logger 기록"), QStringLiteral("자동"),
                     true, 0, false});
    rows.append(Row{Sev::Emergency, QStringLiteral("EV-260825-041"), yesterday, QStringLiteral("어제"),
                     QStringLiteral("R-03"), QStringLiteral("이말순"), QStringLiteral("음성 무응답"),
                     QStringLiteral("2회 무응답 → 보호자 통화 연결 · 정상 확인 후 종료"),
                     QStringLiteral("김예은"), true, 0, false});

    model_->setRows(rows);
}

void P2EventPage::loadEventsFromServer(std::function<void()> onLoaded) {
    apiClient_->fetchEvents([this, onLoaded](bool ok, const QVector<ApiClient::EventInfo>& items) {
        if (!ok) return;  // 서버 미기동 등: seedMockData()로 채워둔 화면을 그대로 둔다

        using Row = EventTableModel::EventRow;
        using Sev = EventTableModel::Severity;
        const QDateTime now = QDateTime::currentDateTime();
        const QDate today = QDate::currentDate();

        QVector<Row> rows;
        rows.reserve(items.size());
        for (const auto& info : items) {
            Row row;
            if (info.severity == QStringLiteral("emergency")) {
                row.severity = Sev::Emergency;
            } else if (info.severity == QStringLiteral("attention")) {
                row.severity = Sev::Attention;
            } else {
                row.severity = Sev::Info;
            }
            row.eventId = info.eventUid;
            const QDateTime occurred = info.occurredAt.toLocalTime();
            row.date = occurred.date();
            row.timeText = occurred.date() == today ? occurred.toString(QStringLiteral("HH:mm"))
                                                      : occurred.toString(QStringLiteral("MM.dd HH:mm"));
            row.robotId = info.robotCode;
            row.recipientName = info.recipientName;
            row.type = info.eventTypeLabel;
            row.summary = info.summary;
            row.assignee = QString();  // 담당자 배정 API 아직 없음(P2 "담당자 배정" 버튼도 추후 구현)
            row.resolved = info.resolved;
            row.elapsedSec = info.resolved ? 0 : qMax(0, static_cast<int>(occurred.secsTo(now)));
            row.checked = false;
            rows.append(row);
        }
        model_->setRows(rows);
        if (onLoaded) onLoaded();
    });
}

void P2EventPage::resolveEvent(const QString& eventId) {
    apiClient_->resolveEvent(eventId, [this, eventId](bool ok, const ApiClient::EventInfo&, const QString& err) {
        if (ok) {
            model_->resolveByEventId(eventId);
            return;
        }
        QMessageBox::warning(this, QStringLiteral("CareGuard"),
                              QStringLiteral("완료 처리에 실패했습니다: %1").arg(err));
    });
}

void P2EventPage::refreshCounts() {
    const int checked = model_->checkedCount();
    bulkBar_->setVisible(checked > 0);
    bulkCountLabel_->setText(QStringLiteral("%1건 선택됨").arg(checked));

    filterCountLabel_->setText(QStringLiteral("전체 %1건 중 미처리 %2건")
                                    .arg(model_->totalCount())
                                    .arg(model_->unresolvedCount()));

    emit unresolvedCountsChanged(model_->emergencyUnresolvedCount(), model_->attentionUnresolvedCount());
}

void P2EventPage::onCellClicked(const QModelIndex& index) {
    if (!index.isValid() || index.column() == EventTableModel::ColCheck) return;

    const QModelIndex sourceIndex = proxy_->mapToSource(index);
    if (!sourceIndex.isValid()) return;

    const EventTableModel::EventRow row = model_->rowAt(sourceIndex.row());
    auto* modal = new EventDetailModal(row, this);
    modal->setAttribute(Qt::WA_DeleteOnClose);
    connect(modal, &EventDetailModal::eventResolved, this, &P2EventPage::resolveEvent);
    connect(modal, &EventDetailModal::videoViewRequested, this, &P2EventPage::videoViewRequested);
    modal->exec();
}

void P2EventPage::onBulkComplete() {
    if (model_->hasEmergencyChecked()) {
        QMessageBox::information(
            this, QStringLiteral("CareGuard"),
            QStringLiteral("긴급 이벤트는 일괄 완료 처리에서 제외됩니다. 개별로 확인해 주세요."));
    }
    for (const QString& eventId : model_->checkedResolvableEventIds()) {
        resolveEvent(eventId);
    }
}

void P2EventPage::onDateRangeChanged() {
    // 시작이 끝보다 늦으면 끝을 시작에 맞춰 밀어준다 (반대 방향은 두지 않음 — 사용자가
    // 마지막으로 조작한 쪽 의도를 존중).
    if (startDateEdit_->date() > endDateEdit_->date()) {
        endDateEdit_->setDate(startDateEdit_->date());
        return;  // setDate가 다시 dateChanged를 내서 아래 setDateRange까지 이어진다.
    }
    proxy_->setDateRange(startDateEdit_->date(), endDateEdit_->date());
}

void P2EventPage::applyPeriodPreset(int spanDays) {
    if (spanDays >= 0) {
        const QDate today = QDate::currentDate();
        startDateEdit_->setDate(today.addDays(-spanDays));
        endDateEdit_->setDate(today);
        setDateEditsReadOnly(true);
    } else {
        setDateEditsReadOnly(false);
        if (!periodCustomBtn_->isChecked()) {
            periodCustomBtn_->setChecked(true);  // 날짜 칸을 직접 클릭해 진입한 경우
        }
    }
}

void P2EventPage::setDateEditsReadOnly(bool readOnly) {
    for (auto* edit : {startDateEdit_, endDateEdit_}) {
        edit->setReadOnly(readOnly);
        edit->setProperty("state", readOnly ? QStringLiteral("off") : QVariant());
        repolish(edit);
    }
}

bool P2EventPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress &&
        (watched == startDateEdit_ || watched == endDateEdit_) && startDateEdit_->isReadOnly()) {
        applyPeriodPreset(-1);  // "직접"로 전환 — 이 클릭 자체는 그대로 통과시켜 캘린더가 열리게 둔다
    }

    // Qt 기본 State_MouseOver는 셀 단위라 마우스 아래 셀만 칠해진다. 행 전체를 칠하려면
    // 뷰포트에서 직접 마우스 위치의 행을 추적해 델리게이트에 전달해야 한다.
    if (tableView_ && watched == tableView_->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const int row = tableView_->indexAt(mouseEvent->pos()).row();
            if (row != hoveredRow_) {
                hoveredRow_ = row;
                tableView_->viewport()->update();
            }
        } else if (event->type() == QEvent::Leave) {
            if (hoveredRow_ != -1) {
                hoveredRow_ = -1;
                tableView_->viewport()->update();
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void P2EventPage::setRecipientNameFilter(const QStringList& names) {
    proxy_->setRecipientNameAllowlist(names);
}

void P2EventPage::filterToRecipient(const QString& recipientName) {
    periodMonthBtn_->setChecked(true);  // 오늘 범위 밖(어제 등) 이벤트도 가리지 않도록 30일로 확장
    applyPeriodPreset(29);
    searchEdit_->setText(recipientName);
}

void P2EventPage::openEventByUid(const QString& eventUid) {
    periodMonthBtn_->setChecked(true);  // filterToRecipient()와 동일 — 오늘 범위 밖이어도 가리지 않음
    applyPeriodPreset(29);
    searchEdit_->setText(eventUid);

    // P4에서 방금 막 업로드가 끝나 새로 생긴 이벤트일 수 있어, 페이지 로드 시점 스냅샷
    // (model_)만 보지 않고 항상 한 번 다시 받아온 뒤에 연다.
    loadEventsFromServer([this, eventUid] { openEventRowIfPresent(eventUid); });
}

void P2EventPage::openEventRowIfPresent(const QString& eventUid) {
    for (int row = 0; row < model_->rowCount(); ++row) {
        const EventTableModel::EventRow eventRow = model_->rowAt(row);
        if (eventRow.eventId != eventUid) continue;
        auto* modal = new EventDetailModal(eventRow, this);
        modal->setAttribute(Qt::WA_DeleteOnClose);
        connect(modal, &EventDetailModal::eventResolved, this, &P2EventPage::resolveEvent);
        connect(modal, &EventDetailModal::videoViewRequested, this, &P2EventPage::videoViewRequested);
        modal->exec();
        return;
    }
    // 취소된 이벤트(GET /events가 status='cancelled'는 제외함) 등으로 못 찾는 경우의 안내.
    QMessageBox::information(this, QStringLiteral("CareGuard"),
                              QStringLiteral("연동된 이벤트(%1)를 찾을 수 없습니다.").arg(eventUid));
}

void P2EventPage::showComingSoon(const QString& feature) {
    QMessageBox::information(this, QStringLiteral("CareGuard"),
                              QStringLiteral("추후 구현: %1").arg(feature));
}
