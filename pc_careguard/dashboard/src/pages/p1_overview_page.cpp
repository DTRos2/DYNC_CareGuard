#include "p1_overview_page.h"

#include <QLabel>
#include <QVBoxLayout>

#include "theme/tokens.h"

P1OverviewPage::P1OverviewPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("통합 관제"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* placeholder = new QLabel(
        QStringLiteral("TODO: KPI 스트립 · 통합 상태 테이블 · 알림 피드 (담당 팀원 구현)"), this);
    placeholder->setProperty("role", "muted");
    layout->addWidget(placeholder);
    layout->addStretch();
}
