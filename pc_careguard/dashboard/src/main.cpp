#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpSocket>

#include "theme/tokens.h"
#include "windows/loginwindow.h"
#include "windows/mainwindow.h"
#include "widgets/splashscreenwidget.h"

#include <QTimer>

namespace {

constexpr auto kServerHost = "127.0.0.1";
constexpr quint16 kServerPort = 8000;

bool isServerReachable() {
    QTcpSocket socket;
    socket.connectToHost(QString::fromLatin1(kServerHost), kServerPort);
    return socket.waitForConnected(200);
}

// 대시보드 실행 시 FastAPI 서버(pc_careguard/server)를 자동으로 같이 띄운다.
// LoginWindow가 기대하는 CAREGUARD_SERVER_URL(기본 http://127.0.0.1:8000)과 짝을 맞춘다.
// 이미 떠 있으면(수동 실행 등) 새로 띄우지 않고 그대로 둔다.
QProcess* startBackendServer() {
    if (isServerReachable()) {
        qInfo("CareGuard server already reachable on %s:%u - skip auto start", kServerHost,
              kServerPort);
        return nullptr;
    }

    const QString serverDir = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_DIR"), QStringLiteral(CAREGUARD_SERVER_DIR));
    if (!QFileInfo::exists(serverDir + QStringLiteral("/app/main.py"))) {
        qWarning("CareGuard server sources not found under %s - skip auto start",
                 qPrintable(serverDir));
        return nullptr;
    }

    const QString venvUvicorn = serverDir + QStringLiteral("/.venv/bin/uvicorn");
    QString program;
    QStringList args{QStringLiteral("app.main:app"), QStringLiteral("--host"),
                      QString::fromLatin1(kServerHost), QStringLiteral("--port"),
                      QString::number(kServerPort)};
    if (QFileInfo::exists(venvUvicorn)) {
        program = venvUvicorn;
    } else {
        program = QStringLiteral("python3");
        args.prepend(QStringLiteral("uvicorn"));
        args.prepend(QStringLiteral("-m"));
    }

    auto* process = new QProcess();
    process->setWorkingDirectory(serverDir);
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->start(program, args);
    if (!process->waitForStarted(3000)) {
        qWarning("CareGuard server failed to start (%s): %s", qPrintable(program),
                  qPrintable(process->errorString()));
        delete process;
        return nullptr;
    }
    qInfo("CareGuard server started (pid %lld)", static_cast<long long>(process->processId()));
    return process;
}

void stopBackendServer(QProcess* process) {
    if (!process) {
        return;
    }
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(3000)) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    delete process;
}

}  // namespace

// 초기화 순서: CareGuard_DESIGN_SYSTEM.md §13 main.cpp 규약 참고.
// 번들 폰트(Pretendard/JetBrains Mono ttf)는 아직 resources/fonts/에 없음 — 추가되면
// QFontDatabase::addApplicationFont(":/fonts/...")를 이 자리에 넣는다. 그 전까지는
// 시스템에 설치된 동일 이름 폰트가 있으면 그것을, 없으면 OS 기본 산세리프로 대체된다.
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setFont(QFont(Tokens::Font::SANS, 10));

    QProcess* serverProcess = startBackendServer();
    QObject::connect(&app, &QApplication::aboutToQuit,
                      [serverProcess]() { stopBackendServer(serverProcess); });

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
