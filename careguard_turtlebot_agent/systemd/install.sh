#!/bin/bash
# 이 로봇(라즈베리파이)에 CareGuard 자동 기동 systemd 서비스를 설치한다.
#
# 사용법 (로봇 코드를 소문자로, 이 스크립트가 있는 디렉토리에서 실행):
#   ./install.sh r05
#   ./install.sh r06
#
# 새 로봇을 추가할 때는 careguard-bringup-<code>.service /
# careguard-uplink-<code>.service 한 쌍을 복사해서 만들고, 이 스크립트로
# 설치하면 된다.
set -euo pipefail

CODE="${1:?사용법: ./install.sh r05 (또는 r06 등 소문자 로봇 코드)}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRINGUP_UNIT="careguard-bringup-${CODE}.service"
UPLINK_UNIT="careguard-uplink-${CODE}.service"

for unit in "$BRINGUP_UNIT" "$UPLINK_UNIT"; do
    if [ ! -f "$SCRIPT_DIR/$unit" ]; then
        echo "에러: $SCRIPT_DIR/$unit 없음 — 로봇 코드를 확인하세요" >&2
        exit 1
    fi
done

sudo cp "$SCRIPT_DIR/$BRINGUP_UNIT" "$SCRIPT_DIR/$UPLINK_UNIT" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now "$BRINGUP_UNIT" "$UPLINK_UNIT"

echo "설치 완료. 상태 확인:"
sudo systemctl status "$BRINGUP_UNIT" "$UPLINK_UNIT" --no-pager -l | head -40
