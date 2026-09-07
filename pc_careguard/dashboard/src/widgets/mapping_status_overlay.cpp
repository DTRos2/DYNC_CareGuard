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

    summary_ = new QLabel(this);
    summary_->setProperty("role", "muted");
    summary_->setTextFormat(Qt::RichText);
    layout->addWidget(summary_);
    layout->addStretch();

    hint_ = new QLabel(QStringLiteral("펼치기"), this);
    hint_->setStyleSheet(QStringLiteral("color: #0B7A6E; font-size: 11px;"));
    layout->addWidget(hint_);

    buildPanel();
    updateSummary();
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

    grid_ = new QGridLayout(panel_);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(0);

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
    grid_->addWidget(headerRow, 0, 0);

    // 행은 여기서 미리 만들지 않는다 — 서버에 실제로 배포된 맵이 있어야만 addRow()로
    // 하나씩 생긴다 (markMapDeployed() 참고). DB가 비어 있으면 패널은 이 헤더 한 줄뿐이다.

    connect(this, &MappingStatusOverlay::destroyed, panel_, &QObject::deleteLater);
}

void MappingStatusOverlay::addRow(const QString& robotId, const QString& mapName,
                                   const QString& recipientName, int wpCount, int routeCount) {
    auto* rowFrame = new QFrame(panel_);
    rowFrame->setObjectName("mappingStatusRow");
    rowFrame->setProperty("warn", false);
    auto* rowLayout = new QHBoxLayout(rowFrame);
    rowLayout->setContentsMargins(12, 7, 12, 7);
    rowLayout->setSpacing(6);

    auto addCell = [&](const QString& text, int width, bool mono) -> QLabel* {
        auto* lbl = new QLabel(text, rowFrame);
        if (mono) lbl->setProperty("role", "mono");
        if (width > 0) lbl->setFixedWidth(width);
        rowLayout->addWidget(lbl, width > 0 ? 0 : 1);
        return lbl;
    };

    QLabel* nameCell = addCell(mapName, 0, false);
    QLabel* recipientCell = addCell(recipientName, 72, false);
    addCell(robotId, 72, true);
    QLabel* wpCell = addCell(QString::number(wpCount), 46, true);
    QLabel* routeCell = addCell(QString::number(routeCount), 46, true);
    QLabel* lastCell = addCell(QDateTime::currentDateTime().toString(QStringLiteral("MM.dd HH:mm")), 100, true);
    lastCell->setStyleSheet(QStringLiteral("color:%1;").arg(Tokens::TEXT_SECONDARY.name()));

    // 미리보기 버튼 — 이 위젯 자신은 맵 이미지를 안 들고 있으므로, robotId만 담아
    // 시그널로 알리고 실제 조회·모달 표시는 상위(P5MapPage)가 한다.
    auto* previewBtn = new QPushButton(SimpleIcons::eye(Tokens::TEXT_MUTED, 12), QString(), rowFrame);
    previewBtn->setProperty("variant", "ghost");
    previewBtn->setFixedSize(24, 24);
    previewBtn->setIconSize(QSize(12, 12));
    previewBtn->setToolTip(QStringLiteral("맵 미리보기"));
    connect(previewBtn, &QPushButton::clicked, this, [this, robotId] { emit mapPreviewRequested(robotId); });
    rowLayout->addWidget(previewBtn, 0);

    rows_.append(RowWidgets{robotId, rowFrame, nameCell, recipientCell, wpCell, routeCell, lastCell});
    grid_->addWidget(rowFrame, rows_.size(), 0);  // 0행은 헤더, 실제 행은 1부터.
}

void MappingStatusOverlay::updateSummary() {
    // 존재하는 행은 전부 실제 배포된 맵이라 등록 수 = 배포 완료 수.
    summary_->setText(QStringLiteral("등록 맵 %1 · 배포 완료 %1").arg(rows_.size()));
}

void MappingStatusOverlay::markMapDeployed(const QString& robotId, const QString& mapName,
                                            const QString& recipientName, int wpCount, int routeCount) {
    for (RowWidgets& r : rows_) {
        if (r.robotId != robotId) continue;

        if (!mapName.isEmpty()) r.name->setText(mapName);
        if (!recipientName.isEmpty()) r.recipient->setText(recipientName);
        r.wp->setText(QString::number(wpCount));
        r.route->setText(QString::number(routeCount));
        r.lastDeployed->setText(QDateTime::currentDateTime().toString(QStringLiteral("MM.dd HH:mm")));
        r.frame->setProperty("warn", false);
        r.frame->style()->unpolish(r.frame);
        r.frame->style()->polish(r.frame);
        updateSummary();
        return;
    }

    addRow(robotId, mapName, recipientName, wpCount, routeCount);
    updateSummary();
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
