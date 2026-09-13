#!/bin/bash
# 이 로봇(라즈베리파이)에 CareGuard 자동 기동 systemd 서비스를 설치한다.
#
# 사용법 (로봇 코드를 소문자로, 이 스크립트가 있는 디렉토리에서 실행):
#   ./install.sh r05
#   ./install.sh r06
#
# 새 로봇을 추가할 때는 careguard-bringup-<code>.service /
# careguard-uplink-<code>.service / careguard-downlink-<code>.service 세 개를
# 복사해서 만들고, 이 스크립트로 설치하면 된다. careguard-camera-<code>.service는
# 그 로봇에 카메라가 있을 때만 선택적으로 추가한다(없으면 건너뛴다 — 2026-09-11,
# R-06부터 도입). downlink는 2026-09-13부터 추가 — 배터리 임계값 자동 복귀
# (충전소로 실제 주행)를 처리한다.
set -euo pipefail

CODE="${1:?사용법: ./install.sh r05 (또는 r06 등 소문자 로봇 코드)}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRINGUP_UNIT="careguard-bringup-${CODE}.service"
UPLINK_UNIT="careguard-uplink-${CODE}.service"
DOWNLINK_UNIT="careguard-downlink-${CODE}.service"
CAMERA_UNIT="careguard-camera-${CODE}.service"

for unit in "$BRINGUP_UNIT" "$UPLINK_UNIT" "$DOWNLINK_UNIT"; do
    if [ ! -f "$SCRIPT_DIR/$unit" ]; then
        echo "에러: $SCRIPT_DIR/$unit 없음 — 로봇 코드를 확인하세요" >&2
        exit 1
    fi
done

UNITS=("$BRINGUP_UNIT" "$UPLINK_UNIT" "$DOWNLINK_UNIT")
if [ -f "$SCRIPT_DIR/$CAMERA_UNIT" ]; then
    UNITS+=("$CAMERA_UNIT")
else
    echo "참고: $CAMERA_UNIT 없음 — 이 로봇엔 카메라 서비스를 설치하지 않는다"
fi

sudo cp "${UNITS[@]/#/$SCRIPT_DIR/}" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now "${UNITS[@]}"

echo "설치 완료. 상태 확인:"
sudo systemctl status "${UNITS[@]}" --no-pager -l | head -60
