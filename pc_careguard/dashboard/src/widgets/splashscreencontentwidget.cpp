#include "splashscreencontentwidget.h"

#include <QFile>
#include <QLabel>
#include <QMediaPlayer>
#include <QResizeEvent>
#include <QVideoSink>
#include <QVideoFrame>

namespace {
constexpr int kStatusAreaHeight = 64;
}

SplashScreenContentWidget::SplashScreenContentWidget(QWidget *parent)
    : QWidget(parent)
    , m_fallbackLabel(new QLabel(this))
    , m_statusLabel(new QLabel(QStringLiteral("중앙 서버에 연결하는 중입니다..."), this))
    , m_videoLabel(new QLabel(this))
    , m_videoSink(new QVideoSink(this))
    , m_mediaPlayer(new QMediaPlayer(this))
    , m_sourceFile(nullptr)
{
    setObjectName(QStringLiteral("splashContent"));
    setStyleSheet(QStringLiteral("QWidget#splashContent { background-color: #030304; }"));

    // 영상/폴백 라벨은 별도 레이아웃 슬롯 없이 위젯 전체를 채우는 배경으로 그린다.
    // 상태 텍스트는 그 위에 하단 고정 오버레이로 raise()하여 겹쳐 놓는다.
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setStyleSheet(QStringLiteral("background-color: #030304;"));
    m_videoLabel->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_fallbackLabel->setAlignment(Qt::AlignCenter);
    m_fallbackLabel->setStyleSheet(QStringLiteral("background-color: #030304;"));
    m_fallbackLabel->hide();

    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setFixedHeight(kStatusAreaHeight);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "background-color: #030304;"
        "color: #f8cd77;"
        "font-size: 18px;"
        "font-weight: 600;"));
    m_statusLabel->raise();

    // QVideoWidget은 OpenGL/EGL 네이티브 창으로 렌더링하는데, 이 VM의 가상 GPU
    // (vmwgfx)가 그 경로를 제대로 못 받쳐줘서 재생이 간헐적으로 멎는 문제가 있었다.
    // QVideoSink로 프레임을 받아 QLabel에 QImage로 그리면 폴백 텍스트와 동일한
    // 순수 래스터 경로를 타므로 이 환경에서도 안정적으로 동작한다.
    m_mediaPlayer->setVideoOutput(m_videoSink);
    m_mediaPlayer->setLoops(QMediaPlayer::Once);
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, &SplashScreenContentWidget::handleVideoFrame);
    connect(m_mediaPlayer, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString &errorString) {
        if (error != QMediaPlayer::NoError) {
            showFallbackText(QStringLiteral("CareGuard"));
        }
    });
}

void SplashScreenContentWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    m_videoLabel->setGeometry(rect());
    m_fallbackLabel->setGeometry(rect());
    m_statusLabel->setGeometry(0, height() - kStatusAreaHeight, width(), kStatusAreaHeight);
    updateVideoLabelPixmap();
}

void SplashScreenContentWidget::setStatusText(const QString &text)
{
    m_statusLabel->setText(text);
}

void SplashScreenContentWidget::startAnimation(const QString &resourcePath)
{
    delete m_sourceFile;
    m_sourceFile = new QFile(resourcePath, this);
    if (!m_sourceFile->open(QIODevice::ReadOnly)) {
        showFallbackText(QStringLiteral("CareGuard"));
        return;
    }

    m_fallbackLabel->hide();
    m_videoLabel->show();
    m_statusLabel->raise();

    m_mediaPlayer->setSourceDevice(m_sourceFile);
    m_mediaPlayer->play();
}

void SplashScreenContentWidget::showFallbackText(const QString &text)
{
    m_mediaPlayer->stop();
    m_videoLabel->hide();
    m_fallbackLabel->setText(text);
    m_fallbackLabel->setStyleSheet(QStringLiteral(
        "background-color: #030304;"
        "color: #f8cd77;"
        "font-size: 52px;"
        "font-weight: 700;"));
    m_fallbackLabel->show();
    m_statusLabel->raise();
}

void SplashScreenContentWidget::stopAnimation()
{
    m_mediaPlayer->stop();
}

void SplashScreenContentWidget::handleVideoFrame(const QVideoFrame &frame)
{
    if (!frame.isValid()) {
        return;
    }

    QImage image = frame.toImage();
    if (image.isNull()) {
        return;
    }

    m_currentFrame = image;
    updateVideoLabelPixmap();
}

void SplashScreenContentWidget::updateVideoLabelPixmap()
{
    if (m_currentFrame.isNull() || m_videoLabel->size().isEmpty()) {
        return;
    }

    const QPixmap scaled = QPixmap::fromImage(m_currentFrame)
        .scaled(m_videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_videoLabel->setPixmap(scaled);
}
