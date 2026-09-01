#ifndef SPLASHSCREENCONTENTWIDGET_H
#define SPLASHSCREENCONTENTWIDGET_H

#include <QWidget>
#include <QImage>

class QLabel;
class QFile;
class QMediaPlayer;
class QVideoSink;
class QVideoFrame;
class QResizeEvent;

class SplashScreenContentWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SplashScreenContentWidget(QWidget *parent = nullptr);

    void setStatusText(const QString &text);
    void startAnimation(const QString &resourcePath);
    void showFallbackText(const QString &text);
    void stopAnimation();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void handleVideoFrame(const QVideoFrame &frame);
    void updateVideoLabelPixmap();

    QLabel *m_fallbackLabel;
    QLabel *m_statusLabel;
    // OpenGL 기반 QVideoWidget 대신 QVideoSink로 받은 프레임을 QImage로 변환해
    // 일반 QLabel에 그린다. 이 VM의 가상 GPU(vmwgfx)가 QVideoWidget의 EGL 렌더링
    // 경로를 제대로 못 받쳐줘서 재생이 간헐적으로 멎는 문제가 있었기 때문.
    QLabel *m_videoLabel;
    QVideoSink *m_videoSink;
    QMediaPlayer *m_mediaPlayer;
    QFile *m_sourceFile;
    QImage m_currentFrame;
};

#endif // SPLASHSCREENCONTENTWIDGET_H
