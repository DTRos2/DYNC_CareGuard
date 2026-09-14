#include "net/server_config.h"

#include <QFile>
#include <QProcessEnvironment>
#include <QTextStream>

namespace ServerConfig {

namespace {

constexpr auto kKey = "CAREGUARD_SERVER_URL";
constexpr auto kDefault = "http://127.0.0.1:8000";

// <dashboard 소스 디렉터리>/.env에서 "CAREGUARD_SERVER_URL=..." 한 줄만 찾는다.
// server/.env 같은 python-dotenv 대상이 아니라 이 대시보드 전용 파일이라 형식을
// 최소한으로만("KEY=VALUE", '#' 주석, 빈 줄 무시) 지원한다 — 여러 키를 담을 일이
// 생기면 그때 확장한다.
QString readFromEnvFile() {
    const QString dashboardDir = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_DASHBOARD_DIR"), QStringLiteral(CAREGUARD_DASHBOARD_DIR));
    QFile file(dashboardDir + QStringLiteral("/.env"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            continue;
        }
        if (line.left(eq).trimmed() == QLatin1String(kKey)) {
            return line.mid(eq + 1).trimmed();
        }
    }
    return QString();
}

}  // namespace

QString resolveServerUrl() {
    const QString fromEnv =
        QProcessEnvironment::systemEnvironment().value(QLatin1String(kKey));
    if (!fromEnv.isEmpty()) {
        return fromEnv;
    }
    const QString fromFile = readFromEnvFile();
    if (!fromFile.isEmpty()) {
        return fromFile;
    }
    return QString(QLatin1String(kDefault));
}

}  // namespace ServerConfig
