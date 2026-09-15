#!/bin/bash
# deploy/sync_to_limo.sh — robot_careguard_ws/를 LIMO 로봇으로 rsync 배포한다.
# CONVENTIONS.md 5절: LIMO는 git 클라이언트 없이 rsync 수신 대상 역할만 한다.
#
# 사용법:
#   deploy/sync_to_limo.sh [--build]
#
# 환경변수로 접속 정보를 재정의할 수 있다 (경로 하드코딩 금지, CONVENTIONS.md 3.2):
#   LIMO_HOST   robot@192.168.230.42 처럼 user@host 형식. 기본값 없음 — 필수.
#   LIMO_WS_DIR 로봇 쪽 워크스페이스 경로. 기본값: ~/robot_careguard_ws
#
# --build: 배포 후 SSH로 접속해 colcon build까지 실행한다.

set -euo pipefail

if [ -z "${LIMO_HOST:-}" ]; then
    echo "LIMO_HOST 환경변수가 필요합니다 (예: LIMO_HOST=wego@192.168.230.42 $0)" >&2
    exit 1
fi

LIMO_WS_DIR="${LIMO_WS_DIR:-robot_careguard_ws}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/robot_careguard_ws/src/"

echo "==> $SRC_DIR -> $LIMO_HOST:$LIMO_WS_DIR/src/"
ssh "$LIMO_HOST" "mkdir -p $LIMO_WS_DIR/src"
rsync -avz --delete \
    --exclude '__pycache__' \
    --exclude '*.pyc' \
    --exclude 'build' \
    --exclude 'install' \
    --exclude 'log' \
    --exclude '.venv' \
    "$SRC_DIR" "$LIMO_HOST:$LIMO_WS_DIR/src/"

if [ "${1:-}" = "--build" ]; then
    echo "==> colcon build ($LIMO_HOST:$LIMO_WS_DIR)"
    ssh "$LIMO_HOST" "source /opt/ros/humble/setup.bash && cd $LIMO_WS_DIR && colcon build --symlink-install"
fi

echo "==> 완료"
