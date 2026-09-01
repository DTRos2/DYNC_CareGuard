#include "splashassetresolver.h"

#include <QFile>

QString SplashAssetResolver::resolveSplashAnimationPath()
{
    const QString resourcePath = QStringLiteral(":/assets/careguard_splash.mp4");
    if (QFile::exists(resourcePath)) {
        return resourcePath;
    }

    return QString();
}
