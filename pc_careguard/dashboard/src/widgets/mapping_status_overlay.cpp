#include "mapping_status_overlay.h"

#include <QDateTime>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {
struct MapRow {
    QString name;
    QString recipient;
    QString robot;
    QString wp;
    QString route;
    QString lastDeployed;
    bool warn;
};

const QVector<MapRow> kRows = {
    {QStringLiteral("행복빌라 302"), QStringLiteral("김순자"), QStringLiteral("R-01"), QStringLiteral("5"),
     QStringLiteral("2"), QStringLiteral("08.24 16:02"), false},
    {QStringLiteral("은빛아파트 101"), QStringLiteral("박영호"), QStringLiteral("R-02"), QStringLiteral("5"),
     QStringLiteral("2"), QStringLiteral("08.24 16:40"), false},
    {QStringLiteral("솔마을 7호"), QStringLiteral("이말순"), QStringLiteral("R-03"), QStringLiteral("4"),
     QStringLiteral("1"), QStringLiteral("08.24 17:15"), false},
    {QStringLiteral("동산주택 2층"), QStringLiteral("최금례"), QStringLiteral("R-04"), QStringLiteral("0"),
     QStringLiteral("0"), QStringLiteral("배포 전 — 순찰 불가"), true},
};
}  // namespace

MappingStatusOverlay::MappingStatusOverlay(QWidget* parent)
    : QFrame(parent), panel_(nullptr), open_(false) {
    setObjectName("mappingStatusHeader");
    setCursor(Qt::PointingHandCursor);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 9, 12, 9);
    layout->setSpacing(8);

    chevron_ = new QLabel(this);
    chevron_->setPixmap(SimpleIcons::chevronRight(Tokens::TEXT_SECONDARY, 13).pixmap(13, 13));
    layout->addWidget(chevron_);

    auto* title = new QLabel(QStringLiteral("매핑 현황"), this);
    title->setProperty("role", "h3");
    layout->addWidget(title);

    summary_ = new QLabel(
        QStringLiteral("등록 맵 4 · 배포 완료 3 · <span style=\"color:#C27500\">웨이포인트 미설정 1</span>"),
        this);
    summary_->setProperty("role", "muted");
    summary_->setTextFormat(Qt::RichText);
    layout->addWidget(summary_);
    layout->addStretch();

    hint_ = new QLabel(QStringLiteral("펼치기"), this);
    hint_->setStyleSheet(QStringLiteral("color: #0B7A6E; font-size: 11px;"));
    layout->addWidget(hint_);

    buildPanel();
}

void MappingStatusOverlay::buildPanel() {
    panel_ = new QFrame(parentWidget());
    panel_->setObjectName("mappingStatusPanel");
    panel_->hide();

    auto* shadow = new QGraphicsDropShadowEffect(panel_);
    shadow->setBlurRadius(Tokens::SHADOW_LG.blur);
    shadow->setOffset(0, Tokens::SHADOW_LG.dy);
    shadow->setColor(Tokens::SHADOW_LG.color);
    panel_->setGraphicsEffect(shadow);

    auto* grid = new QGridLayout(panel_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);

    const QStringList headers = {QStringLiteral("맵 · 가정"),   QStringLiteral("대상자"),
                                  QStringLiteral("할당 로봇"),   QStringLiteral("WP"),
                                  QStringLiteral("경로"),       QStringLiteral("최종 배포"),
                                  QString()};  // 마지막 열은 미리보기 버튼 — 헤더 텍스트 없음.
    auto* headerRow = new QFrame(panel_);
    headerRow->setObjectName("mappingStatusRowHeader");
    auto* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(12, 6, 12, 6);
    headerLayout->setSpacing(6);
    const QList<int> stretches = {1, 0, 0, 0, 0, 0, 0};
    const QList<int> widths = {0, 72, 72, 46, 46, 100, 28};
    for (int i = 0; i < headers.size(); ++i) {
        auto* lbl = new QLabel(headers[i], headerRow);
        lbl->setProperty("role", "label");
        if (widths[i] > 0) lbl->setFixedWidth(widths[i]);
        headerLayout->addWidget(lbl, stretches[i]);
    }
    grid->addWidget(headerRow, 0, 0);

    int row = 1;
    for (const MapRow& r : kRows) {
        auto* rowFrame = new QFrame(panel_);
        rowFrame->setObjectName("mappingStatusRow");
        rowFrame->setProperty("warn", r.warn);
        auto* rowLayout = new QHBoxLayout(rowFrame);
        rowLayout->setContentsMargins(12, 7, 12, 7);
        rowLayout->setSpacing(6);

        auto addCell = [&](const QString& text, int width, bool mono, const QColor* color) -> QLabel* {
            auto* lbl = new QLabel(text, rowFrame);
            if (mono) lbl->setProperty("role", "mono");
            if (color) lbl->setStyleSheet(QStringLiteral("color:%1;").arg(color->name()));
            if (width > 0) lbl->setFixedWidth(width);
            rowLayout->addWidget(lbl, width > 0 ? 0 : 1);
            return lbl;
        };

        const QColor* warnColor = r.warn ? &Tokens::ATTENTION.main : nullptr;
        QLabel* nameCell = addCell(r.name, 0, false, nullptr);
        QLabel* recipientCell = addCell(r.recipient, 72, false, nullptr);
        addCell(r.robot, 72, true, nullptr);
        QLabel* wpCell = addCell(r.wp, 46, true, warnColor);
        QLabel* routeCell = addCell(r.route, 46, true, warnColor);
        QLabel* lastCell =
            addCell(r.lastDeployed, 100, true, r.warn ? &Tokens::ATTENTION.main : &Tokens::TEXT_SECONDARY);

        // 미리보기 버튼 — 이 위젯 자신은 맵 이미지를 안 들고 있으므로(kRows는 텍스트
        // 더미), robotId만 담아 시그널로 알리고 실제 조회·모달 표시는 상위(P5MapPage)가 한다.
        auto* previewBtn = new QPushButton(SimpleIcons::eye(Tokens::TEXT_MUTED, 12), QString(), rowFrame);
        previewBtn->setProperty("variant", "ghost");
        previewBtn->setFixedSize(24, 24);
        previewBtn->setIconSize(QSize(12, 12));
        previewBtn->setToolTip(QStringLiteral("맵 미리보기"));
        const QString robotIdForPreview = r.robot;
        connect(previewBtn, &QPushButton::clicked, this,
                [this, robotIdForPreview] { emit mapPreviewRequested(robotIdForPreview); });
        rowLayout->addWidget(previewBtn, 0);

        rows_.append(RowWidgets{r.robot, rowFrame, nameCell, recipientCell, wpCell, routeCell, lastCell});

        grid->addWidget(rowFrame, row, 0);
        ++row;
    }

    connect(this, &MappingStatusOverlay::destroyed, panel_, &QObject::deleteLater);
}

void MappingStatusOverlay::markMapDeployed(const QString& robotId, const QString& mapName,
                                            const QString& recipientName) {
    for (RowWidgets& r : rows_) {
        if (r.robotId != robotId) continue;

        if (!mapName.isEmpty()) r.name->setText(mapName);
        if (!recipientName.isEmpty()) r.recipient->setText(recipientName);
        r.wp->setText(QStringLiteral("5"));
        r.wp->setStyleSheet(QString());
        r.route->setText(QStringLiteral("1"));
        r.route->setStyleSheet(QString());
        r.lastDeployed->setText(QDateTime::currentDateTime().toString(QStringLiteral("MM.dd HH:mm")));
        r.lastDeployed->setStyleSheet(QStringLiteral("color:%1;").arg(Tokens::TEXT_SECONDARY.name()));
        r.frame->setProperty("warn", false);
        r.frame->style()->unpolish(r.frame);
        r.frame->style()->polish(r.frame);
        break;
    }

    summary_->setText(QStringLiteral("등록 맵 4 · 배포 완료 4"));
}

void MappingStatusOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) toggle();
    QFrame::mousePressEvent(event);
}

void MappingStatusOverlay::toggle() {
    open_ = !open_;
    setProperty("open", open_);
    style()->unpolish(this);
    style()->polish(this);
    chevron_->setPixmap((open_ ? SimpleIcons::chevronDown(Tokens::TEXT_SECONDARY, 13)
                                : SimpleIcons::chevronRight(Tokens::TEXT_SECONDARY, 13))
                             .pixmap(13, 13));
    hint_->setText(open_ ? QStringLiteral("접기") : QStringLiteral("펼치기"));

    if (open_) {
        reposition();
        panel_->raise();
        panel_->show();
    } else {
        panel_->hide();
    }
}

void MappingStatusOverlay::reposition() {
    if (!panel_) return;
    panel_->setFixedWidth(width());
    panel_->move(x(), y() + height() + 4);
    panel_->adjustSize();
}

void MappingStatusOverlay::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    if (open_) reposition();
}
