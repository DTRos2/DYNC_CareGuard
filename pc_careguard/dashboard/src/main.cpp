#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpSocket>
#include <QUrl>

#include "net/server_config.h"
#include "theme/tokens.h"
#include "windows/loginwindow.h"
#include "windows/mainwindow.h"
#include "widgets/splashscreenwidget.h"

#include <QTimer>

namespace {

// 로컬 자동 기동 기본 포트 — CAREGUARD_SERVER_URL에 포트가 없을 때만 쓴다.
constexpr quint16 kDefaultServerPort = 8000;

// 실제로 uvicorn을 띄울 때 바인드할 주소. 로봇(careguard_agent)이 같은 LAN에서
// ws://이_PC_IP:8000/ws/robot 로 접속해야 해서 기본값을 0.0.0.0(모든 인터페이스
// 허용)으로 둔다 — loopback(127.0.0.1)만 열면 로봇이 아예 접속할 수 없다.
// 신뢰 안 되는 네트워크에 있는 노트북 등에서는 CAREGUARD_SERVER_BIND_HOST=127.0.0.1로
// 되돌릴 수 있다.
QString serverBindHost() {
    return QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_BIND_HOST"), QStringLiteral("0.0.0.0"));
}

bool isHostReachable(const QString& host, quint16 port) {
    QTcpSocket socket;
    socket.connectToHost(host, port);
    return socket.waitForConnected(200);
}

// localhost/127.0.0.1이 아닌 호스트가 CAREGUARD_SERVER_URL로 명시돼 있으면, 그건
// "이 PC가 아니라 다른 PC(예: 팀원의 중앙 서버)를 보라"는 의도다 — 이 경우 로컬에
// 새 서버를 띄워봤자 그 원격 DB/MQTT 브로커에 연결되지 않으니 자동 기동 대상이
// 아니다(원래 목적: 개발 중 서버를 깜빡하고 안 띄웠을 때만 대신 띄워주는 편의 기능).
//
// 예전엔 이 값을 안 보고 무조건 127.0.0.1:8000만 확인했다 — 그래서 팀원이
// CAREGUARD_SERVER_URL을 중앙 서버로 정확히 설정해놔도, 자기 PC 로컬에 서버가
// 없으면 로컬 백엔드를 하나 더 띄워버렸다. 그 로컬 백엔드가 팀원 자신의
// server/.env(MQTT_HOST=중앙 브로커 주소)를 읽어 같은 MQTT 브로커에 각자 구독하면서,
// 버튼 한 번 눌림이 (중앙 서버 1개 + 팀원 로컬 서버 N개)만큼 care_event로 중복
// 삽입되는 사고로 이어졌다.
bool isRemoteServerConfigured(const QUrl& serverUrl) {
    const QString host = serverUrl.host();
    return !host.isEmpty() && host != QStringLiteral("127.0.0.1") &&
           host != QStringLiteral("localhost");
}

// 대시보드 실행 시 FastAPI 서버(pc_careguard/server)를 자동으로 같이 띄운다.
// LoginWindow가 기대하는 CAREGUARD_SERVER_URL(기본 http://127.0.0.1:8000)과 짝을 맞춘다.
// 이미 떠 있으면(수동 실행 등) 새로 띄우지 않고 그대로 둔다.
QProcess* startBackendServer() {
    const QUrl serverUrl(ServerConfig::resolveServerUrl());
    const QString serverHost =
        serverUrl.host().isEmpty() ? QStringLiteral("127.0.0.1") : serverUrl.host();
    const quint16 serverPort =
        serverUrl.port() > 0 ? static_cast<quint16>(serverUrl.port()) : kDefaultServerPort;

    if (isHostReachable(serverHost, serverPort)) {
        qInfo("CareGuard server already reachable on %s:%u - skip auto start",
              qPrintable(serverHost), serverPort);
        return nullptr;
    }

    if (isRemoteServerConfigured(serverUrl)) {
        qWarning(
            "CAREGUARD_SERVER_URL points to remote host %s:%u but it is not reachable - "
            "not auto-starting a local server (starting one here would connect to the "
            "wrong DB/MQTT broker)",
            qPrintable(serverHost), serverPort);
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
                      QStringLiteral("--port"), QString::number(serverPort)};
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
          qPrintable(bindHost), serverPort);
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
