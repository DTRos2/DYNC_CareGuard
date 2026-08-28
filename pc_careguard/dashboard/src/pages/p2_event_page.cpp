#include "p2_event_page.h"

#include <QLabel>
#include <QVBoxLayout>

#include "theme/tokens.h"

P2EventPage::P2EventPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("이벤트 관제"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* placeholder = new QLabel(
        QStringLiteral("TODO: 필터 바 · 이벤트 테이블 · 페이지네이션 (담당 팀원 구현)"), this);
    placeholder->setProperty("role", "muted");
    layout->addWidget(placeholder);
    layout->addStretch();
}
