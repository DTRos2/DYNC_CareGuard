#include "camera_live_view.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
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

    placeholderIcon_ = new QLabel(this);
    placeholderIcon_->setPixmap(SimpleIcons::video(Tokens::OFFLINE.main, 20).pixmap(20, 20));
    placeholderIcon_->setAlignment(Qt::AlignHCenter);
    layout->addWidget(placeholderIcon_);

    placeholderText_ = new QLabel(QStringLiteral("카메라 연결 중"), this);
    placeholderText_->setAlignment(Qt::AlignHCenter);
    placeholderText_->setStyleSheet(QStringLiteral("color:#8A97A3; font-size:10px; margin-top:4px;"));
    layout->addWidget(placeholderText_);

    layout->addStretch();

    // 실시간 영상 — placeholder 뒤에 프레임 크기 그대로 깔아두고, 프레임이 오면
    // placeholder를 감추고 이걸 보여준다(레이아웃에는 안 넣는다 — LIVE 배지·placeholder와
    // 겹쳐서 항상 박스 전체를 꽉 채워야 하기 때문).
    imageLabel_ = new QLabel(this);
    imageLabel_->setGeometry(0, 0, 148, 96);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setScaledContents(false);
    imageLabel_->hide();
    imageLabel_->lower();
}

void CameraLiveView::setFrame(const QImage& frame) {
    if (frame.isNull()) return;
    const QPixmap scaled = QPixmap::fromImage(frame).scaled(imageLabel_->size(), Qt::KeepAspectRatioByExpanding,
                                                              Qt::SmoothTransformation);
    imageLabel_->setPixmap(scaled);
    imageLabel_->show();
    placeholderIcon_->hide();
    placeholderText_->hide();
}

void CameraLiveView::clearFrame() {
    imageLabel_->hide();
    imageLabel_->clear();
    placeholderIcon_->show();
    placeholderText_->show();
}
