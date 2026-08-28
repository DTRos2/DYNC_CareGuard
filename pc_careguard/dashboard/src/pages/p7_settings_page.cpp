#include "p7_settings_page.h"

#include <QLabel>
#include <QVBoxLayout>

#include "theme/tokens.h"

P7SettingsPage::P7SettingsPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("시스템 설정"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* placeholder = new QLabel(
        QStringLiteral("TODO: QTabWidget 4탭 — 로봇 관리 / 계정 관리 / 임계값·알림 / 감사 로그 (담당 팀원 구현)"),
        this);
    placeholder->setProperty("role", "muted");
    layout->addWidget(placeholder);
    layout->addStretch();
}
