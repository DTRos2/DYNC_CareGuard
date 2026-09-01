#include "p4_video_page.h"

#include <QLabel>
#include <QVBoxLayout>

#include "theme/tokens.h"

QVector<VideoClip> mockVideoClips() {
    return {
        {QStringLiteral("R-01"), QStringLiteral("김순자"), QStringLiteral("심박 이상"),
         QStringLiteral("긴급"), VideoTagStyle::Emergency, QStringLiteral("08.26 14:29"),
         QStringLiteral("EV-260826-014"), QStringLiteral("02:14"), /*viewCount=*/1,
         /*expireDays=*/30},

        {QStringLiteral("R-02"), QStringLiteral("박영호"), QStringLiteral("복약 미수령"),
         QStringLiteral("주의"), VideoTagStyle::Attention, QStringLiteral("08.26 12:41"),
         QStringLiteral("EV-260826-011"), QStringLiteral("01:38"), /*viewCount=*/0,
         /*expireDays=*/30},

        {QStringLiteral("R-03"), QStringLiteral("이말순"), QStringLiteral("순찰 스냅샷"),
         QStringLiteral("순찰"), VideoTagStyle::Neutral, QStringLiteral("08.26 10:04"),
         Tokens::Format::EMPTY, QStringLiteral("00:22"), /*viewCount=*/0,
         /*expireDays=*/5},

        {QStringLiteral("R-03"), QStringLiteral("이말순"), QStringLiteral("음성 무응답"),
         QStringLiteral("긴급"), VideoTagStyle::Emergency, QStringLiteral("08.25 16:12"),
         QStringLiteral("EV-260825-041"), QStringLiteral("03:05"), /*viewCount=*/3,
         /*expireDays=*/29},

        {QStringLiteral("R-01"), QStringLiteral("김순자"), QStringLiteral("수동 확인"),
         QStringLiteral("수동 열람"), VideoTagStyle::Neutral, QStringLiteral("08.25 09:30"),
         Tokens::Format::EMPTY, QStringLiteral("00:47"), /*viewCount=*/1,
         /*expireDays=*/6},
    };
}

P4VideoPage::P4VideoPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("영상 관리"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* placeholder = new QLabel(
        QStringLiteral("TODO: 필터·용량 게이지 · 썸네일 그리드/테이블 토글 (담당 팀원 구현)"), this);
    placeholder->setProperty("role", "muted");
    layout->addWidget(placeholder);
    layout->addStretch();
}
