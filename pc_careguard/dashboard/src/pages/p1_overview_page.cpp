#include "p1_overview_page.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "theme/tokens.h"

namespace {

constexpr int kBarColorRole = Qt::UserRole + 1;

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

}  // namespace

P1OverviewPage::P1OverviewPage(QWidget* parent) : QWidget(parent) {
    rows_ = {
        {QStringLiteral("R-01"), Tokens::Level::Emergency, QStringLiteral("서울시 강남구 역삼로 12"),
         QStringLiteral("김영희"), 22, 1, QStringLiteral("긴급 알림 대응 중")},
        {QStringLiteral("R-02"), Tokens::Level::Attention, QStringLiteral("서울시 마포구 합정동 45"),
         QStringLiteral("박철수"), 35, 3, QStringLiteral("웨어러블 재연결 시도 중")},
        {QStringLiteral("R-03"), Tokens::Level::Normal, QStringLiteral("서울시 송파구 잠실동 78"),
         QStringLiteral("이순자"), 81, 3, QStringLiteral("정기 순찰 중 (구간 2)")},
        {QStringLiteral("R-04"), Tokens::Level::Offline, QStringLiteral("서울시 노원구 상계동 34"),
         QStringLiteral("최민준"), -1, -1, QStringLiteral("마지막 통신 09:12:03")},
        {QStringLiteral("R-05"), Tokens::Level::Normal, QStringLiteral("서울시 강동구 천호동 21"),
         QStringLiteral("정하늘"), 64, 3, QStringLiteral("정기 순찰 중 (구간 1)")},
        {QStringLiteral("R-06"), Tokens::Level::Attention, QStringLiteral("서울시 서초구 방배동 9"),
         QStringLiteral("한지민"), 18, 2, QStringLiteral("배터리 부족 — 충전 필요")},
        {QStringLiteral("R-07"), Tokens::Level::Normal, QStringLiteral("서울시 은평구 불광동 56"),
         QStringLiteral("오세훈"), 92, 3, QStringLiteral("대기 중")},
        {QStringLiteral("R-08"), Tokens::Level::Normal, QStringLiteral("서울시 관악구 신림동 88"),
         QStringLiteral("문지훈"), 77, 3, QStringLiteral("정기 순찰 중 (구간 3)")},
        {QStringLiteral("R-09"), Tokens::Level::Offline, QStringLiteral("서울시 성북구 정릉동 14"),
         QStringLiteral("강수진"), -1, -1, QStringLiteral("마지막 통신 08:47:22")},
        {QStringLiteral("R-10"), Tokens::Level::Attention, QStringLiteral("서울시 중랑구 면목동 3"),
         QStringLiteral("배도윤"), 41, 1, QStringLiteral("복약 미수령 — 확인 필요")},
    };

    alerts_ = {
        {QStringLiteral("09:42:11"), Tokens::Prop::EMERGENCY, QStringLiteral("R-01 심박 138bpm — 긴급 임계치 초과")},
        {QStringLiteral("09:38:05"), Tokens::Prop::ATTENTION, QStringLiteral("R-01 복약 미수령 — 30분 경과")},
        {QStringLiteral("09:31:47"), Tokens::Prop::ATTENTION, QStringLiteral("R-02 웨어러블 신호 약함 감지")},
        {QStringLiteral("09:12:03"), Tokens::Prop::ATTENTION, QStringLiteral("R-04 통신 두절 — 마지막 응답 09:12:03")},
        {QStringLiteral("09:05:40"), Tokens::Prop::INFO, QStringLiteral("R-03 순찰 구간 2 진입 완료")},
        {QStringLiteral("08:58:12"), Tokens::Prop::ATTENTION, QStringLiteral("R-06 배터리 18% — 충전 필요")},
        {QStringLiteral("08:47:22"), Tokens::Prop::ATTENTION, QStringLiteral("R-09 통신 두절 — 마지막 응답 08:47:22")},
        {QStringLiteral("08:40:05"), Tokens::Prop::INFO, QStringLiteral("R-05 순찰 구간 1 진입 완료")},
        {QStringLiteral("08:31:19"), Tokens::Prop::ATTENTION, QStringLiteral("R-10 복약 미수령 — 확인 필요")},
        {QStringLiteral("08:20:44"), Tokens::Prop::INFO, QStringLiteral("R-07 대기 모드 전환")},
        {QStringLiteral("08:10:03"), Tokens::Prop::INFO, QStringLiteral("R-08 순찰 구간 3 진입 완료")},
        {QStringLiteral("08:02:51"), Tokens::Prop::INFO, QStringLiteral("R-02 웨어러블 신호 정상 복구")},
    };

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    layout->addWidget(buildHeaderRow());
    layout->addWidget(buildKpiRow());
    layout->addWidget(buildTableSection(), /*stretch=*/2);
    layout->addWidget(buildAlertSection(), /*stretch=*/1);

    sortRows(SortMode::Risk);
    populateTable();
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

    auto* returnHomeBtn = new QPushButton(QStringLiteral("충전 복귀"), row);
    returnHomeBtn->setProperty("variant", "secondary");
    rowLayout->addWidget(returnHomeBtn);

    auto* cancelPatrolBtn = new QPushButton(QStringLiteral("순찰 취소"), row);
    cancelPatrolBtn->setProperty("variant", "secondary");
    rowLayout->addWidget(cancelPatrolBtn);

    auto* estopAllBtn = new QPushButton(QStringLiteral("전체 비상정지"), row);
    estopAllBtn->setProperty("variant", "danger");
    rowLayout->addWidget(estopAllBtn);

    return row;
}

QWidget* P1OverviewPage::buildKpiRow() {
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(Tokens::Layout::CARD_GAP);

    struct KpiSpec {
        QString label;
        QString value;
        const char* level;  // nullptr = 기본 흰 배경
    };
    const KpiSpec specs[] = {
        {QStringLiteral("가동 로봇"), QStringLiteral("3/4"), nullptr},
        {QStringLiteral("미처리 경보"), QStringLiteral("1"), Tokens::Prop::EMERGENCY},
        {QStringLiteral("복약 미수령"), QStringLiteral("1"), Tokens::Prop::ATTENTION},
        {QStringLiteral("웨어러블 이상"), QStringLiteral("1"), Tokens::Prop::ATTENTION},
        {QStringLiteral("통신 두절"), QStringLiteral("1"), nullptr},
    };

    for (const auto& spec : specs) {
        auto* card = new QFrame(row);
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

    return row;
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
        sortRows(static_cast<SortMode>(id));
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

    sectionLayout->addWidget(alertList_);
    return section;
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
            table_->setCellWidget(r, 2, wrapCellWidget(bar, rowBg));
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
        table_->setCellWidget(r, 7, wrapCellWidget(detailBtn, rowBg));

        auto* estopBtn = new QPushButton(QStringLiteral("정지"));
        estopBtn->setObjectName("rowEstopBtn");
        if (offline) {
            estopBtn->setEnabled(false);
            estopBtn->setToolTip(QStringLiteral("통신 두절 — 정지 명령 전달 불가"));
        }
        table_->setCellWidget(r, 8, wrapCellWidget(estopBtn, rowBg));
    }
}
