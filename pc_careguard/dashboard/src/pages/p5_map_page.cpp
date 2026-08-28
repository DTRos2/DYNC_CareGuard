#include "p5_map_page.h"

#include <QLabel>
#include <QVBoxLayout>

#include "theme/tokens.h"

P5MapPage::P5MapPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("맵·로봇 매핑"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* placeholder = new QLabel(
        QStringLiteral("TODO: 로봇 셀렉터 · SLAM 캔버스 · 텔레옵 패널 (담당 팀원 구현)"), this);
    placeholder->setProperty("role", "muted");
    layout->addWidget(placeholder);
    layout->addStretch();
}
