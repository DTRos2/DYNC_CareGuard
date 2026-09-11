#include "loginwindow.h"

#include <QCheckBox>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStyle>
#include <QSvgWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include "theme/tokens.h"
#include "widgets/icon_line_edit.h"
#include "widgets/simple_icons.h"

namespace {

QLabel* makeStyledLabel(const QString& text, int pixelSize, QFont::Weight weight,
                         const QColor& color, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    label->setFont(font);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, color);
    label->setPalette(pal);
    return label;
}

void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

// 76x76 라운드(radius 20) 배지 배경 — 로그인 로고 전용, ACCENT_SOFT 채움.
// (RADIUS_SM/MD/LG 토큰에 없는 값이지만 p0_login_final_wireframe.html에 명시된 로고 배지 고유 값이다.)
class RoundedBadge : public QWidget {
public:
    RoundedBadge(int size, int radius, const QColor& bg, QWidget* parent = nullptr)
        : QWidget(parent), radius_(radius), bg_(bg) {
        setFixedSize(size, size);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath path;
        path.addRoundedRect(rect(), radius_, radius_);
        painter.fillPath(path, bg_);
    }

private:
    int radius_;
    QColor bg_;
};

}  // namespace

LoginWindow::LoginWindow(QWidget* parent) : QWidget(parent) {
    serverUrl_ = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_URL"), QStringLiteral("http://127.0.0.1:8000"));
    netManager_ = new QNetworkAccessManager(this);

    setWindowTitle(QStringLiteral("CareGuard 로그인"));
    setObjectName("centralWidget");  // careguard.qss: QWidget#centralWidget → BG_BASE 배경 재사용
    setFixedSize(560, 660);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* accentStrip = new QFrame(this);
    accentStrip->setFixedHeight(3);
    accentStrip->setAutoFillBackground(true);
    QPalette stripPal = accentStrip->palette();
    stripPal.setColor(QPalette::Window, Tokens::ACCENT);
    accentStrip->setPalette(stripPal);
    outer->addWidget(accentStrip);

    outer->addStretch();

    auto* card = new QFrame(this);
    card->setObjectName("card");
    card->setFixedWidth(400);
    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(Tokens::SHADOW_MD.blur);
    shadow->setOffset(0, Tokens::SHADOW_MD.dy);
    shadow->setColor(Tokens::SHADOW_MD.color);
    card->setGraphicsEffect(shadow);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(28, 26, 28, 26);
    cardLayout->setSpacing(0);

    auto* badgeRow = new QHBoxLayout();
    auto* badge = new RoundedBadge(76, 20, Tokens::ACCENT_SOFT, card);
    auto* badgeInner = new QVBoxLayout(badge);
    badgeInner->setContentsMargins(15, 15, 15, 15);
    auto* mark = new QSvgWidget(QStringLiteral(":/icons/careguard_mark.svg"), badge);
    mark->setFixedSize(46, 46);
    badgeInner->addWidget(mark);
    badgeRow->addStretch();
    badgeRow->addWidget(badge);
    badgeRow->addStretch();
    cardLayout->addLayout(badgeRow);
    cardLayout->addSpacing(10);

    auto* brand = makeStyledLabel(QStringLiteral("CareGuard"), 21, QFont::DemiBold, Tokens::ACCENT, card);
    brand->setAlignment(Qt::AlignHCenter);
    cardLayout->addWidget(brand);

    auto* subtitle = makeStyledLabel(QStringLiteral("노인 안전 돌봄 로봇 · 통합 관제"), Tokens::Font::CAPTION,
                                      QFont::Normal, Tokens::TEXT_MUTED, card);
    subtitle->setAlignment(Qt::AlignHCenter);
    cardLayout->addWidget(subtitle);
    cardLayout->addSpacing(20);

    auto* idLabel = new QLabel(QStringLiteral("아이디"), card);
    idLabel->setProperty("role", "label");
    cardLayout->addWidget(idLabel);
    cardLayout->addSpacing(4);

    idInput_ = new IconLineEdit(card);
    idInput_->setPlaceholderText(QStringLiteral("아이디 입력"));
    idInput_->setFixedHeight(36);
    idInput_->setLeadingIcon(SimpleIcons::user(Tokens::TEXT_MUTED, 14));
    cardLayout->addWidget(idInput_);
    cardLayout->addSpacing(12);

    auto* pwLabel = new QLabel(QStringLiteral("비밀번호"), card);
    pwLabel->setProperty("role", "label");
    cardLayout->addWidget(pwLabel);
    cardLayout->addSpacing(4);

    passwordInput_ = new IconLineEdit(card);
    passwordInput_->setPlaceholderText(QStringLiteral("비밀번호 입력"));
    passwordInput_->setEchoMode(QLineEdit::Password);
    passwordInput_->setFixedHeight(36);
    passwordInput_->setLeadingIcon(SimpleIcons::lock(Tokens::TEXT_MUTED, 14));
    passwordInput_->setTrailingIcon(SimpleIcons::eye(Tokens::TEXT_MUTED, 14));
    connect(passwordInput_, &IconLineEdit::trailingIconClicked, this,
            &LoginWindow::togglePasswordVisibility);
    cardLayout->addWidget(passwordInput_);
    cardLayout->addSpacing(8);

    inlineErrorRow_ = new QWidget(card);
    auto* inlineErrorLayout = new QHBoxLayout(inlineErrorRow_);
    inlineErrorLayout->setContentsMargins(0, 0, 0, 0);
    inlineErrorLayout->setSpacing(4);
    inlineErrorIcon_ = new QLabel(inlineErrorRow_);
    inlineErrorIcon_->setPixmap(SimpleIcons::alertCircle(Tokens::EMERGENCY.main, 12).pixmap(12, 12));
    inlineErrorText_ = makeStyledLabel(QString(), Tokens::Font::CAPTION, QFont::Normal,
                                        Tokens::EMERGENCY.main, inlineErrorRow_);
    inlineErrorText_->setWordWrap(false);
    inlineErrorLayout->addWidget(inlineErrorIcon_, 0, Qt::AlignVCenter);
    inlineErrorLayout->addWidget(inlineErrorText_, 1, Qt::AlignVCenter);
    cardLayout->addWidget(inlineErrorRow_);
    inlineErrorRow_->setVisible(false);

    banner_ = new QFrame(card);
    banner_->setObjectName("banner");
    auto* bannerOuter = new QHBoxLayout(banner_);
    bannerOuter->setContentsMargins(10, 9, 10, 9);
    bannerOuter->setSpacing(8);
    bannerIcon_ = new QLabel(banner_);
    bannerIcon_->setFixedSize(14, 14);
    auto* bannerText = new QVBoxLayout();
    bannerText->setContentsMargins(0, 0, 0, 0);
    bannerText->setSpacing(3);
    bannerTitle_ = new QLabel(banner_);
    QFont bannerTitleFont = bannerTitle_->font();
    bannerTitleFont.setPixelSize(Tokens::Font::CAPTION);
    bannerTitleFont.setWeight(QFont::DemiBold);
    bannerTitle_->setFont(bannerTitleFont);
    bannerSubtitle_ = new QLabel(banner_);
    QFont bannerSubFont = bannerSubtitle_->font();
    bannerSubFont.setPixelSize(Tokens::Font::CAPTION);
    bannerSubtitle_->setFont(bannerSubFont);
    QPalette bannerSubPal = bannerSubtitle_->palette();
    bannerSubPal.setColor(QPalette::WindowText, Tokens::TEXT_SECONDARY);
    bannerSubtitle_->setPalette(bannerSubPal);
    bannerSubtitle_->setWordWrap(true);  // 텍스트가 잘려 안 보이는 대신 2줄로 감싸도록 하는 안전장치
    bannerText->addWidget(bannerTitle_);
    bannerText->addWidget(bannerSubtitle_);
    bannerOuter->addWidget(bannerIcon_, 0, Qt::AlignTop);
    bannerOuter->addLayout(bannerText, 1);
    cardLayout->addWidget(banner_);
    banner_->setVisible(false);
    cardLayout->addSpacing(12);

    rememberCheckbox_ = new QCheckBox(QStringLiteral("로그인 유지"), card);
    cardLayout->addWidget(rememberCheckbox_);
    cardLayout->addSpacing(16);

    loginBtn_ = new QPushButton(QStringLiteral("로그인"), card);
    loginBtn_->setProperty("variant", "primary");
    loginBtn_->setDefault(true);
    cardLayout->addWidget(loginBtn_);

    connect(loginBtn_, &QPushButton::clicked, this, &LoginWindow::onSubmit);
    connect(idInput_, &QLineEdit::returnPressed, this, &LoginWindow::onSubmit);
    connect(passwordInput_, &QLineEdit::returnPressed, this, &LoginWindow::onSubmit);

    auto* cardRow = new QHBoxLayout();
    cardRow->addStretch();
    cardRow->addWidget(card);
    cardRow->addStretch();
    outer->addLayout(cardRow);
    outer->addStretch();

    lockTimer_ = new QTimer(this);
    connect(lockTimer_, &QTimer::timeout, this, &LoginWindow::onLockTick);
}

void LoginWindow::onSubmit() {
    if (submitting_ || state_ == State::Locked) return;

    const QString id = idInput_->text().trimmed();
    const QString pw = passwordInput_->text();

    if (id.isEmpty() || pw.isEmpty()) {
        hideBanner();
        setInlineErrorText(QStringLiteral("아이디와 비밀번호를 입력하세요"));
        inlineErrorRow_->setVisible(true);
        return;
    }

    submitting_ = true;
    setState(State::Authenticating);
    resolveLogin(id, pw);
}

void LoginWindow::resolveLogin(const QString& id, const QString& password) {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/auth/login")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonObject body{{"login_id", id}, {"password", password}};
    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());

    connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
        handleLoginReply(reply, id);
        reply->deleteLater();
    });
}

void LoginWindow::handleLoginReply(QNetworkReply* reply, const QString& id) {
    submitting_ = false;

    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (statusCode == 0) {
        // 네트워크 에러(연결 거부, 타임아웃 등) — 서버에 도달하지 못함.
        setState(State::ServerError);
        return;
    }

    if (statusCode == 200) {
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        const QString role = body.value(QStringLiteral("role")).toString();
        const QString token = body.value(QStringLiteral("token")).toString();

        failCount_ = 0;
        setState(State::Idle);
        emit loginSucceeded(id, role, token);
        return;
    }

    // 401 등 인증 실패 — 계정 미존재/비활성 여부와 무관하게 동일하게 처리.
    failCount_++;
    if (failCount_ >= 5) {
        enterLocked();
        return;
    }
    setState(State::AuthFailed);
}

void LoginWindow::setState(State state) {
    state_ = state;

    switch (state) {
        case State::Idle:
            idInput_->setEnabled(true);
            passwordInput_->setEnabled(true);
            loginBtn_->setEnabled(true);
            loginBtn_->setIcon(QIcon());
            loginBtn_->setText(QStringLiteral("로그인"));
            loginBtn_->setProperty("variant", "primary");
            repolish(loginBtn_);
            clearPasswordError();
            inlineErrorRow_->setVisible(false);
            hideBanner();
            break;

        case State::Authenticating:
            idInput_->setEnabled(false);
            passwordInput_->setEnabled(false);
            loginBtn_->setIcon(SimpleIcons::refresh(Qt::white, 13));
            loginBtn_->setText(QStringLiteral("확인 중…"));
            loginBtn_->setProperty("variant", "primary");
            repolish(loginBtn_);
            inlineErrorRow_->setVisible(false);
            hideBanner();
            break;

        case State::AuthFailed:
            idInput_->setEnabled(true);
            passwordInput_->setEnabled(true);
            loginBtn_->setEnabled(true);
            loginBtn_->setIcon(QIcon());
            loginBtn_->setText(QStringLiteral("로그인"));
            loginBtn_->setProperty("variant", "primary");
            repolish(loginBtn_);
            passwordInput_->setProperty("state", "error");
            repolish(passwordInput_);
            hideBanner();
            setInlineErrorText(
                QStringLiteral("아이디 또는 비밀번호가 올바르지 않습니다 (%1/5)").arg(failCount_));
            inlineErrorRow_->setVisible(true);
            break;

        case State::ServerError:
            idInput_->setEnabled(true);
            passwordInput_->setEnabled(true);
            loginBtn_->setEnabled(true);
            loginBtn_->setIcon(SimpleIcons::refresh(Tokens::TEXT_PRIMARY, 13));
            loginBtn_->setText(QStringLiteral("다시 시도"));
            loginBtn_->setProperty("variant", "secondary");
            repolish(loginBtn_);
            clearPasswordError();
            inlineErrorRow_->setVisible(false);
            applyBanner(QStringLiteral("관제 서버에 연결할 수 없습니다"),
                        QStringLiteral("네트워크 상태를 확인해 주세요"), QStringLiteral("emergency"),
                        SimpleIcons::plugOff(Tokens::EMERGENCY.main, 14));
            break;

        case State::Locked:
            idInput_->setEnabled(false);
            passwordInput_->setEnabled(false);
            loginBtn_->setEnabled(false);
            loginBtn_->setIcon(QIcon());
            loginBtn_->setText(QStringLiteral("로그인 (잠금 해제 대기)"));
            loginBtn_->setProperty("variant", "primary");
            repolish(loginBtn_);
            clearPasswordError();
            inlineErrorRow_->setVisible(false);
            break;
    }
}

void LoginWindow::enterLocked() {
    lockRemainingSec_ = 5 * 60;  // 데모용 5분 — 실제 잠금 시간은 P7 설정값 확정 후 반영
    setState(State::Locked);
    const int m = lockRemainingSec_ / 60;
    const int s = lockRemainingSec_ % 60;
    applyBanner(QStringLiteral("계정이 잠겼습니다"),
                QStringLiteral("<span style='font-family:%1;color:%2;font-weight:600'>%3</span> 후 "
                                "재시도하거나 관리자에게 문의하세요")
                    .arg(Tokens::Font::MONO, Tokens::ATTENTION.main.name())
                    .arg(QStringLiteral("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))),
                QStringLiteral("attention"), SimpleIcons::lockExclamation(Tokens::ATTENTION.main, 14));
    lockTimer_->start(1000);
}

void LoginWindow::onLockTick() {
    lockRemainingSec_--;
    if (lockRemainingSec_ <= 0) {
        lockTimer_->stop();
        failCount_ = 0;
        setState(State::Idle);
        return;
    }
    const int m = lockRemainingSec_ / 60;
    const int s = lockRemainingSec_ % 60;
    bannerSubtitle_->setText(
        QStringLiteral("<span style='font-family:%1;color:%2;font-weight:600'>%3</span> 후 "
                        "재시도하거나 관리자에게 문의하세요")
            .arg(Tokens::Font::MONO, Tokens::ATTENTION.main.name())
            .arg(QStringLiteral("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))));
}

void LoginWindow::applyBanner(const QString& title, const QString& subtitleHtml, const QString& level,
                               const QIcon& icon) {
    banner_->setProperty("level", level);
    repolish(banner_);
    bannerIcon_->setPixmap(icon.pixmap(14, 14));
    bannerTitle_->setText(title);
    bannerSubtitle_->setTextFormat(Qt::RichText);
    bannerSubtitle_->setText(subtitleHtml);
    banner_->setVisible(true);
}

void LoginWindow::hideBanner() { banner_->setVisible(false); }

void LoginWindow::clearPasswordError() {
    passwordInput_->setProperty("state", QVariant());
    repolish(passwordInput_);
}

void LoginWindow::setInlineErrorText(const QString& text) {
    // 카드 콘텐츠 폭(400 - 좌우 여백 28*2) 중 아이콘(12)+간격(4)을 뺀 텍스트 가용 폭.
    // 한 줄 유지가 최우선이라 폰트가 예상보다 넓어도(번들 폰트 미탑재 시 대체 폰트) 줄바꿈 대신 가운데를 줄인다.
    constexpr int kMaxWidth = 328;
    const QFontMetrics fm(inlineErrorText_->font());
    inlineErrorText_->setText(fm.elidedText(text, Qt::ElideMiddle, kMaxWidth));
    inlineErrorText_->setToolTip(text);
}

void LoginWindow::togglePasswordVisibility() {
    passwordVisible_ = !passwordVisible_;
    passwordInput_->setEchoMode(passwordVisible_ ? QLineEdit::Normal : QLineEdit::Password);
    passwordInput_->setTrailingIcon(passwordVisible_ ? SimpleIcons::eyeOff(Tokens::TEXT_MUTED, 14)
                                                       : SimpleIcons::eye(Tokens::TEXT_MUTED, 14));
}
