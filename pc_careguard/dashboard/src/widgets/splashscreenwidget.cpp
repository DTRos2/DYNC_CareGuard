#include "splashscreenwidget.h"

#include "splashassetresolver.h"
#include "splashscreencontentwidget.h"

#include <QDebug>
#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QPropertyAnimation>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>

namespace {
constexpr qreal kScreenUsageRatio = 0.92;
constexpr int kMinSplashWindowSize = 600;
}

SplashScreenWidget::SplashScreenWidget(QWidget *parent)
    : QWidget(parent, Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_contentWidget(nullptr)
    , m_opacityEffect(nullptr)
    , m_fadeInAnimation(nullptr)
    , m_fadeOutAnimation(nullptr)
    , m_finishTimer(new QTimer(this))
    , m_ready(false)
    , m_closing(false)
    , m_fadeInStarted(false)
    , m_minimumDurationElapsed(false)
{
    configureWindow();
    buildUi();
    centerOnPrimaryScreen();
    startAnimation();

    m_finishTimer->setSingleShot(true);
    connect(m_finishTimer, &QTimer::timeout, this, &SplashScreenWidget::handleMinimumDurationElapsed);
}

SplashScreenWidget::~SplashScreenWidget() = default;

void SplashScreenWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    if (!m_fadeInStarted) {
        m_fadeInStarted = true;
        startFadeIn();
        m_finishTimer->start(kVisibleDurationMs);
    }
}

void SplashScreenWidget::setStatus(const QString &text)
{
    if (m_contentWidget != nullptr) {
        m_contentWidget->setStatusText(text);
    }
}

void SplashScreenWidget::notifyReady()
{
    m_ready = true;
    if (m_minimumDurationElapsed) {
        startFadeOut();
    }
}

void SplashScreenWidget::closeNow()
{
    m_ready = true;
    startFadeOut();
}

void SplashScreenWidget::handleMinimumDurationElapsed()
{
    m_minimumDurationElapsed = true;
    if (m_ready) {
        startFadeOut();
    }
}

void SplashScreenWidget::startFadeOut()
{
    if (m_closing || !m_ready || !m_minimumDurationElapsed) {
        return;
    }

    m_closing = true;
    m_finishTimer->stop();
    m_fadeInAnimation->stop();
    m_fadeOutAnimation->setStartValue(m_opacityEffect->opacity());
    m_fadeOutAnimation->start();
}

void SplashScreenWidget::configureWindow()
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName(QStringLiteral("splashRoot"));
    setStyleSheet(QStringLiteral("QWidget#splashRoot { background-color: #030304; }"));
}

void SplashScreenWidget::buildUi()
{
    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_contentWidget = new SplashScreenContentWidget(this);
    rootLayout->addWidget(m_contentWidget);

    m_opacityEffect = new QGraphicsOpacityEffect(m_contentWidget);
    m_opacityEffect->setOpacity(0.0);
    m_contentWidget->setGraphicsEffect(m_opacityEffect);

    m_fadeInAnimation = new QPropertyAnimation(m_opacityEffect, "opacity", this);
    m_fadeInAnimation->setDuration(kFadeInMs);
    m_fadeInAnimation->setStartValue(0.0);
    m_fadeInAnimation->setEndValue(1.0);

    m_fadeOutAnimation = new QPropertyAnimation(m_opacityEffect, "opacity", this);
    m_fadeOutAnimation->setDuration(kFadeOutMs);
    m_fadeOutAnimation->setStartValue(1.0);
    m_fadeOutAnimation->setEndValue(0.0);

    connect(m_fadeOutAnimation, &QPropertyAnimation::finished, this, [this]() {
        m_contentWidget->stopAnimation();
        hide();
        emit finished();
    });
}

void SplashScreenWidget::centerOnPrimaryScreen()
{
    QScreen *screen = QApplication::primaryScreen();
    if (screen == nullptr) {
        setFixedSize(kMinSplashWindowSize, kMinSplashWindowSize);
        return;
    }

    const QRect availableGeometry = screen->availableGeometry();
    const int squareSize = qMax(
        kMinSplashWindowSize,
        static_cast<int>(qMin(availableGeometry.width(), availableGeometry.height()) * kScreenUsageRatio));

    setFixedSize(squareSize, squareSize);
    move(availableGeometry.center() - rect().center());
}

void SplashScreenWidget::startAnimation()
{
    const QString animationPath = SplashAssetResolver::resolveSplashAnimationPath();
    if (animationPath.isEmpty()) {
        qDebug() << "Failed to resolve splash video resource path:" << QStringLiteral(":/assets/careguard_splash.mp4");
        m_contentWidget->showFallbackText(QStringLiteral("CareGuard"));
        return;
    }

    qDebug() << "Loaded splash video from:" << animationPath;
    m_contentWidget->startAnimation(animationPath);
}

void SplashScreenWidget::startFadeIn()
{
    m_fadeInAnimation->start();
}
