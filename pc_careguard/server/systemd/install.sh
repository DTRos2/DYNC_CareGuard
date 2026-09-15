#!/bin/bash
# 이 PC에 CareGuard 서버(FastAPI/uvicorn)를 상시 기동 systemd 서비스로 설치한다.
#
# 배경: pc_careguard/dashboard/src/main.cpp가 대시보드 실행 시 서버를 자동으로
# 같이 띄우고(startBackendServer), 대시보드 종료 시 그 서버 프로세스도 같이 죽인다
# (aboutToQuit -> stopBackendServer). 그러면 "대시보드를 켜둔 사람의 PC"만 서버
# 역할을 할 수 있고, 그 사람이 대시보드를 끄면 로봇도 팀원 대시보드(CAREGUARD_SERVER_URL로
# 이 PC를 보게 설정된)도 전부 접속이 끊긴다. 이 서비스는 서버를 대시보드 생명주기와
# 분리해서 독립적으로 상시 띄워둔다 — main.cpp::isServerReachable()이 이미 떠 있으면
# 새로 안 띄우고 그대로 붙으므로, 이 서비스를 설치한 뒤 대시보드를 같이 켜도 충돌 없다.
#
# 사용법: 서버를 상시 호스팅할 PC(팀원들이 CAREGUARD_SERVER_URL로 가리킬 그 PC)에서
# 이 스크립트가 있는 위치 기준으로 실행한다.
#   ./install.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVICE_USER="$(whoami)"
UNIT_NAME=careguard-server.service

if [ ! -x "$SERVER_DIR/.venv/bin/uvicorn" ]; then
    echo "에러: $SERVER_DIR/.venv/bin/uvicorn 이 없음 — 먼저 서버 venv를 만들고 requirements.txt를 설치하세요" >&2
    exit 1
fi
if [ ! -f "$SERVER_DIR/.env" ]; then
    echo "경고: $SERVER_DIR/.env 가 없음 — .env.example을 복사해 DB 접속 정보를 채워야 서버가 정상 기동합니다" >&2
fi

sed -e "s#@SERVICE_USER@#${SERVICE_USER}#g" -e "s#@SERVER_DIR@#${SERVER_DIR}#g" \
    "$SCRIPT_DIR/careguard-server.service.in" | sudo tee "/etc/systemd/system/${UNIT_NAME}" > /dev/null

sudo systemctl daemon-reload
sudo systemctl enable --now "$UNIT_NAME"

echo "설치 완료. 상태 확인:"
sudo systemctl status "$UNIT_NAME" --no-pager -l | head -30
