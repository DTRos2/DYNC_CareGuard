#!/bin/bash
# careguard_turtlebot_agent 패키지를 로봇 라즈베리파이로 올리고 빌드한다.
#
# 사용법:
#   ./deploy_agent.sh <ssh계정@호스트> <원격 워크스페이스 경로>
#
# 예시:
#   ./deploy_agent.sh yeyu@100.110.210.121 ~/careguard_turtlebot_agent_ws
#
# 실행 전 준비물:
#   - 이 컴퓨터에서 그 로봇으로 SSH가 먼저 되어야 함(같은 공유기이거나 Tailscale로 연결됨)
#   - 로봇에 ROS2 humble, colcon, python3-pip가 이미 설치되어 있어야 함
set -euo pipefail

REMOTE="${1:?사용법: ./deploy_agent.sh <계정@호스트> <원격 워크스페이스 경로>}"
REMOTE_WS="${2:?사용법: ./deploy_agent.sh <계정@호스트> <원격 워크스페이스 경로>}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[1/4] 로봇에 워크스페이스 디렉토리 생성"
ssh "$REMOTE" "mkdir -p $REMOTE_WS/src/careguard_turtlebot_agent"

echo "[2/4] 패키지 코드 rsync"
rsync -avz --delete \
  --exclude 'scripts' --exclude 'systemd' --exclude '*.md' \
  "$SCRIPT_DIR/" "$REMOTE:$REMOTE_WS/src/careguard_turtlebot_agent/"

echo "[3/4] python 의존성 설치 (websockets)"
ssh "$REMOTE" "pip3 install --user websockets"

echo "[4/4] colcon build"
ssh "$REMOTE" "source /opt/ros/humble/setup.bash && cd $REMOTE_WS && colcon build --packages-select careguard_turtlebot_agent"

echo "완료: $REMOTE:$REMOTE_WS 에 빌드됨"
