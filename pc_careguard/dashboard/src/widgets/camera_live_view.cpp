#include "camera_live_view.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {
// 148x96 -> 1.6배. 원격조종 때 화면 보고 조작하는데 너무 작아서 잘 안 보인다는
// 실사용 요청으로 키웠다 — 가로세로 비율은 유지.
constexpr int kViewWidth = 236;
constexpr int kViewHeight = 154;
constexpr QLatin1String kLiveDotColor("#D93B47");     // 빨강 — 최근에 실제로 프레임이 옴
constexpr QLatin1String kStaleDotColor("#5A6774");    // 회색 — 끊김(kStaleTimeoutMs 동안 새 프레임 없음)
}  // namespace

CameraLiveView::CameraLiveView(QWidget* parent) : QFrame(parent) {
    setObjectName("cameraLiveView");
    // 영상 피드는 관례상 어두운 바탕 위에 놓인다 — Lcd::* 토큰은 로봇 LCD 전용이라 쓰지
    // 않고, 대시보드에서도 쓸 수 있는 TEXT_PRIMARY(#16202B)를 배경으로 재사용한다.
    setStyleSheet(QStringLiteral(
        "QFrame#cameraLiveView { background:#16202B; border:1px solid #2A3542; border-radius:8px; }"));
    setFixedSize(kViewWidth, kViewHeight);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 7, 8, 7);
    layout->setSpacing(0);

    auto* liveRow = new QHBoxLayout();
    liveRow->setSpacing(4);
    liveDot_ = new QLabel(this);
    liveDot_->setFixedSize(6, 6);
    liveDot_->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(kStaleDotColor));
    liveRow->addWidget(liveDot_);
    liveLabel_ = new QLabel(QStringLiteral("LIVE"), this);
    liveLabel_->setStyleSheet(QStringLiteral("color:#FFFFFF; font-size:10px; font-weight:600;"));
    liveRow->addWidget(liveLabel_);
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
    imageLabel_->setGeometry(0, 0, kViewWidth, kViewHeight);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setScaledContents(false);
    imageLabel_->hide();
    imageLabel_->lower();

    // 새 프레임이 kStaleTimeoutMs 안에 안 오면 markStale()이 불려 dot을 회색으로
    // 바꾼다 — setFrame()이 매번 이 타이머를 재시작한다(정상 수신 중엔 절대 안 울림).
    staleTimer_ = new QTimer(this);
    staleTimer_->setSingleShot(true);
    connect(staleTimer_, &QTimer::timeout, this, &CameraLiveView::markStale);
}

void CameraLiveView::setFrame(const QImage& frame) {
    if (frame.isNull()) return;
    const QPixmap scaled = QPixmap::fromImage(frame).scaled(imageLabel_->size(), Qt::KeepAspectRatioByExpanding,
                                                              Qt::SmoothTransformation);
    imageLabel_->setPixmap(scaled);
    imageLabel_->show();
    placeholderIcon_->hide();
    placeholderText_->hide();

    liveDot_->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(kLiveDotColor));
    liveLabel_->setText(QStringLiteral("LIVE"));
    staleTimer_->start(kStaleTimeoutMs);
}

void CameraLiveView::clearFrame() {
    imageLabel_->hide();
    imageLabel_->clear();
    placeholderIcon_->show();
    placeholderText_->show();
    staleTimer_->stop();
    liveDot_->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(kStaleDotColor));
}

void CameraLiveView::markStale() {
    // 프레임은 그대로 화면에 남겨둔다(뭐가 마지막으로 보였는지는 알 수 있게) — 다만
    // dot을 회색으로 바꾸고 문구를 바꿔서, "지금 보이는 게 실시간이 아니라 멈춘
    // 화면"이라는 걸 명확히 알린다. 원격조종 중 이 화면만 보고 로봇을 움직이는데
    // 끊긴 걸 못 알아채면 실제로 위험하다.
    liveDot_->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(kStaleDotColor));
    liveLabel_->setText(QStringLiteral("연결 끊김"));
}
