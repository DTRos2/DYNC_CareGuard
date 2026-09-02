#include <QApplication>
#include <QFile>
#include <QFont>

#include "theme/tokens.h"
#include "windows/loginwindow.h"
#include "windows/mainwindow.h"
#include "widgets/splashscreenwidget.h"

#include <QTimer>

// 초기화 순서: CareGuard_DESIGN_SYSTEM.md §13 main.cpp 규약 참고.
// 번들 폰트(Pretendard/JetBrains Mono ttf)는 아직 resources/fonts/에 없음 — 추가되면
// QFontDatabase::addApplicationFont(":/fonts/...")를 이 자리에 넣는다. 그 전까지는
// 시스템에 설치된 동일 이름 폰트가 있으면 그것을, 없으면 OS 기본 산세리프로 대체된다.
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setFont(QFont(Tokens::Font::SANS, 10));

    QFile qss(":/careguard.qss");
    if (qss.open(QFile::ReadOnly | QFile::Text)) {
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
    }

    auto* login = new LoginWindow();
    MainWindow* mainWindow = nullptr;

    QObject::connect(login, &LoginWindow::loginSucceeded, [&](const QString& username) {
        if (!mainWindow) {
            mainWindow = new MainWindow();
            QObject::connect(mainWindow, &MainWindow::logoutRequested, [&]() {
                mainWindow->hide();
                login->show();
            });
        }
        mainWindow->setUserName(username);
        mainWindow->show();
        login->hide();
    });

    auto* splash = new SplashScreenWidget();
    QObject::connect(splash, &SplashScreenWidget::finished, [&]() {
        login->show();
        splash->deleteLater();
    });
    QTimer::singleShot(SplashScreenWidget::kVisibleDurationMs, splash, [splash]() {
        splash->notifyReady();
    });
    splash->show();

    return app.exec();
}
