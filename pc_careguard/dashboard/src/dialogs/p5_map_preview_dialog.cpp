#include "p5_map_preview_dialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedLayout>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/image_zoom_view.h"
#include "widgets/simple_icons.h"

MapPreviewDialog::MapPreviewDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("맵 미리보기"));
    setModal(true);
    // OS 창관리자 타이틀바를 없애 커스텀 헤더(제목·X)만 남긴다 — 안 그러면 위쪽에
    // 회색 OS 타이틀바가 그대로 남고 X도 두 개로 보인다(EventDetailModal과 동일 패턴).
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    // 좁은 480px 다이얼로그(MODAL_W_S)에 눌려 맵이 작게 보이던 문제 — 더 넓은 M 사이즈로
    // 키우고 미리보기 영역 자체도 크게 잡는다.
    setFixedWidth(Tokens::Layout::MODAL_W_M);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());

    auto* body = new QVBoxLayout();
    body->setContentsMargins(16, 14, 16, 14);
    body->setSpacing(6);

    auto* previewBox = new QFrame(this);
    previewBox->setObjectName("previewBox");
    previewBox->setFixedHeight(460);
    auto* previewBoxLayout = new QVBoxLayout(previewBox);
    previewBoxLayout->setContentsMargins(1, 1, 1, 1);

    previewStack_ = new QStackedLayout();
    previewBoxLayout->addLayout(previewStack_);

    // 페이지 0 — 실제 맵 이미지, 휠 확대/축소 + 드래그 이동 가능.
    imageView_ = new ImageZoomView(previewBox);
    previewStack_->addWidget(imageView_);

    // 페이지 1 — 빈 상태 (robotId에 아직 등록된 맵 이미지가 없을 때).
    auto* emptyPage = new QWidget(previewBox);
    auto* emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->addStretch();
    emptyStateIcon_ = new QLabel(emptyPage);
    emptyStateIcon_->setPixmap(SimpleIcons::mapPin(Tokens::TEXT_MUTED, 28).pixmap(28, 28));
    emptyStateIcon_->setAlignment(Qt::AlignHCenter);
    emptyLayout->addWidget(emptyStateIcon_);
    emptyStateLabel_ = new QLabel(QStringLiteral("아직 매핑된 지도가 없습니다"), emptyPage);
    emptyStateLabel_->setAlignment(Qt::AlignHCenter);
    emptyStateLabel_->setProperty("role", "muted");
    emptyLayout->addWidget(emptyStateLabel_);
    emptyLayout->addStretch();
    previewStack_->addWidget(emptyPage);

    body->addWidget(previewBox);

    auto* zoomHint = new QLabel(QStringLiteral("휠로 확대/축소 · 드래그로 이동 · 더블클릭으로 전체 보기"), this);
    zoomHint->setProperty("role", "muted");
    zoomHint->setStyleSheet(QStringLiteral("font-size:11px;"));
    zoomHint->setAlignment(Qt::AlignRight);
    body->addWidget(zoomHint);
    body->addSpacing(4);

    auto* infoFrame = new QFrame(this);
    // 타입 셀렉터 없이("border:...") 스타일시트를 지정하면 QLabel도 QWidget의
    // 서브클래스라 이 프레임 안의 라벨(맵 이름·대상자·로봇 ID 값)에도 테두리가 그대로
    // 번져 각 텍스트마다 캡슐 테두리가 둘러지는 문제가 있었다(event_detail_modal.cpp의
    // makeBoxFrame()과 같은 원인) — objectName + ID 셀렉터로 이 프레임 자신에게만 한정한다.
    infoFrame->setObjectName(QStringLiteral("mapPreviewInfoFrame"));
    infoFrame->setStyleSheet(
        QStringLiteral("QFrame#mapPreviewInfoFrame{border:1px solid #E3E8ED; border-radius:8px; background:#FFFFFF;}"));
    auto* infoLayout = new QVBoxLayout(infoFrame);
    infoLayout->setContentsMargins(12, 10, 12, 10);
    infoLayout->setSpacing(6);

    auto addInfoRow = [&](const QString& label) -> QLabel* {
        auto* row = new QHBoxLayout();
        auto* labelWidget = new QLabel(label, infoFrame);
        labelWidget->setProperty("role", "secondary");
        labelWidget->setFixedWidth(64);
        row->addWidget(labelWidget);
        auto* valueWidget = new QLabel(infoFrame);
        valueWidget->setStyleSheet(QStringLiteral("color:#16202B; font-weight:500;"));
        row->addWidget(valueWidget, 1);
        infoLayout->addLayout(row);
        return valueWidget;
    };

    mapNameValue_ = addInfoRow(QStringLiteral("맵 이름"));
    recipientValue_ = addInfoRow(QStringLiteral("대상자"));
    robotIdValue_ = addInfoRow(QStringLiteral("로봇 ID"));
    robotIdValue_->setProperty("role", "mono");

    body->addWidget(infoFrame);
    root->addLayout(body);

    auto* footer = new QFrame(this);
    footer->setObjectName("modalFooter");
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 0, 16, 0);
    footerLayout->addStretch();
    auto* closeBtn = new QPushButton(QStringLiteral("닫기"), footer);
    closeBtn->setProperty("variant", "secondary");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    footerLayout->addWidget(closeBtn);
    root->addWidget(footer);
}

QFrame* MapPreviewDialog::buildHeader() {
    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    auto* icon = new QLabel(header);
    icon->setPixmap(SimpleIcons::mapPin(Tokens::ACCENT, 15).pixmap(15, 15));
    layout->addWidget(icon);

    auto* title = new QLabel(QStringLiteral("맵 미리보기"), header);
    title->setStyleSheet(QStringLiteral("font-size:14px; font-weight:500; color:#16202B;"));
    layout->addWidget(title);
    layout->addStretch();

    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_SECONDARY, 16), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(closeBtn);

    return header;
}

void MapPreviewDialog::setMapInfo(const QImage& image, const QString& mapName,
                                   const QString& recipientName, const QString& robotId) {
    mapNameValue_->setText(mapName.isEmpty() ? Tokens::Format::EMPTY : mapName);
    recipientValue_->setText(recipientName.isEmpty() ? Tokens::Format::EMPTY : recipientName);
    robotIdValue_->setText(robotId);

    imageView_->setImage(image);
    previewStack_->setCurrentIndex(image.isNull() ? 1 : 0);
}
