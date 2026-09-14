#pragma once
#include <QString>

// 서버 주소(CAREGUARD_SERVER_URL) 결정 로직을 한 곳에 모은다 — 예전엔 ApiClient·WsClient·
// LoginWindow·P4VideoPage·P7SettingsPage·P8LogsPage 6곳이 각자
// QProcessEnvironment::systemEnvironment().value("CAREGUARD_SERVER_URL", "http://127.0.0.1:8000")를
// 복붙해서 썼다. 팀원마다 다른 서버(PC)를 봐야 하는데(같은 LAN에서 로봇이 실제로 붙어있는
// PC 하나만 서버 역할) 그때마다 환경변수를 매번 export하긴 번거로워서, 환경변수 없이도
// 영속되는 .env 파일 하나로 설정할 수 있게 우선순위를 추가했다.
namespace ServerConfig {

// 우선순위: 1) 환경변수 CAREGUARD_SERVER_URL 2) <대시보드 소스 디렉터리>/.env의
// CAREGUARD_SERVER_URL= 값 3) 기본값 http://127.0.0.1:8000.
// .env 파일 위치는 CMakeLists.txt가 심어주는 CAREGUARD_DASHBOARD_DIR(빌드 디렉터리가
// 아닌 소스 디렉터리 — CAREGUARD_SERVER_DIR과 같은 이유로, rm -rf build를 해도
// 안 날아가야 하기 때문) 기준이고, CAREGUARD_DASHBOARD_DIR 환경변수로도 재정의 가능하다.
QString resolveServerUrl();

}  // namespace ServerConfig
