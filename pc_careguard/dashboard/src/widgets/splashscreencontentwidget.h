#ifndef SPLASHSCREENCONTENTWIDGET_H
#define SPLASHSCREENCONTENTWIDGET_H

#include <QWidget>

class QLabel;
class QFile;
class QMediaPlayer;
class QVideoWidget;
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
    QLabel *m_fallbackLabel;
    QLabel *m_statusLabel;
    QVideoWidget *m_videoWidget;
    QMediaPlayer *m_mediaPlayer;
    QFile *m_sourceFile;
};

#endif // SPLASHSCREENCONTENTWIDGET_H
