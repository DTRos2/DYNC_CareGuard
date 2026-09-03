#include "camera_live_view.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"

CameraLiveView::CameraLiveView(QWidget* parent) : QFrame(parent) {
    setObjectName("cameraLiveView");
    // 영상 피드는 관례상 어두운 바탕 위에 놓인다 — Lcd::* 토큰은 로봇 LCD 전용이라 쓰지
    // 않고, 대시보드에서도 쓸 수 있는 TEXT_PRIMARY(#16202B)를 배경으로 재사용한다.
    setStyleSheet(QStringLiteral(
        "QFrame#cameraLiveView { background:#16202B; border:1px solid #2A3542; border-radius:8px; }"));
    setFixedSize(148, 96);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 7, 8, 7);
    layout->setSpacing(0);

    auto* liveRow = new QHBoxLayout();
    liveRow->setSpacing(4);
    auto* dot = new QLabel(this);
    dot->setFixedSize(6, 6);
    dot->setStyleSheet(QStringLiteral("background:#D93B47; border-radius:3px;"));
    liveRow->addWidget(dot);
    auto* liveLabel = new QLabel(QStringLiteral("LIVE"), this);
    liveLabel->setStyleSheet(QStringLiteral("color:#FFFFFF; font-size:10px; font-weight:600;"));
    liveRow->addWidget(liveLabel);
    liveRow->addStretch();
    layout->addLayout(liveRow);

    layout->addStretch();

    auto* icon = new QLabel(this);
    icon->setPixmap(SimpleIcons::video(Tokens::OFFLINE.main, 20).pixmap(20, 20));
    icon->setAlignment(Qt::AlignHCenter);
    layout->addWidget(icon);

    auto* statusLabel = new QLabel(QStringLiteral("카메라 연결 중"), this);
    statusLabel->setAlignment(Qt::AlignHCenter);
    statusLabel->setStyleSheet(QStringLiteral("color:#8A97A3; font-size:10px; margin-top:4px;"));
    layout->addWidget(statusLabel);

    layout->addStretch();
}
