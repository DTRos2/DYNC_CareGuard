#include "p6_patrol_page.h"

#include <QLabel>
#include <QVBoxLayout>

#include "theme/tokens.h"

P6PatrolPage::P6PatrolPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("순찰·경로"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* placeholder = new QLabel(
        QStringLiteral("TODO: 맵 셀렉터 · 웨이포인트 편집 캔버스 · 경로/스케줄 패널 (담당 팀원 구현)"), this);
    placeholder->setProperty("role", "muted");
    layout->addWidget(placeholder);
    layout->addStretch();
}
