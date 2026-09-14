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

// isServerReachable()이 "이미 떠 있는지" 확인할 때 쓰는 주소 — loopback으로 확인하면
// 충분하다(0.0.0.0으로 띄운 서버도 127.0.0.1로 접속된다).
constexpr auto kServerHost = "127.0.0.1";
constexpr quint16 kServerPort = 8000;

// 실제로 uvicorn을 띄울 때 바인드할 주소. 로봇(careguard_agent)이 같은 LAN에서
// ws://이_PC_IP:8000/ws/robot 로 접속해야 해서 기본값을 0.0.0.0(모든 인터페이스
// 허용)으로 둔다 — loopback(127.0.0.1)만 열면 로봇이 아예 접속할 수 없다.
// 신뢰 안 되는 네트워크에 있는 노트북 등에서는 CAREGUARD_SERVER_BIND_HOST=127.0.0.1로
// 되돌릴 수 있다.
QString serverBindHost() {
    return QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_BIND_HOST"), QStringLiteral("0.0.0.0"));
}

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

    const QString bindHost = serverBindHost();
    const QString venvUvicorn = serverDir + QStringLiteral("/.venv/bin/uvicorn");
    QString program;
    QStringList args{QStringLiteral("app.main:app"), QStringLiteral("--host"), bindHost,
                      QStringLiteral("--port"), QString::number(kServerPort)};
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
    qInfo("CareGuard server started (pid %lld, bound to %s:%u)", static_cast<long long>(process->processId()),
          qPrintable(bindHost), kServerPort);
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

    QObject::connect(login, &LoginWindow::loginSucceeded,
                      [&](const QString& username, const QString& role, const QString& token) {
        if (!mainWindow) {
            mainWindow = new MainWindow();
            QObject::connect(mainWindow, &MainWindow::logoutRequested, [&]() {
                mainWindow->hide();
                login->show();
            });
        }
        mainWindow->setUserName(username);
        mainWindow->setUserRole(role);  
        mainWindow->setAuthToken(token); 
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
