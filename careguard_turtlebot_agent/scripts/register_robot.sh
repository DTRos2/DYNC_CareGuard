#!/bin/bash
# CareGuard 서버 DB에 새 로봇을 등록한다 (대시보드 "로봇 등록" 버튼과 동일한 API 호출).
#
# 사용법:
#   ./register_robot.sh <서버URL> <code> <name> <model> <ros_domain_id> [ip]
#
# 예시:
#   ./register_robot.sh http://100.107.185.97:8000 R-06 "R-06(yeyu)" turtlebot 40 100.110.210.121
#
# model은 "turtlebot" 또는 "limo" 등 서버가 아는 값이어야 한다.
# 이미 등록된 code면 서버가 409를 반환한다(에러 아님 — 이미 있다는 뜻).
set -euo pipefail

SERVER_URL="${1:?사용법: ./register_robot.sh <서버URL> <code> <name> <model> <ros_domain_id> [ip]}"
CODE="${2:?}"
NAME="${3:?}"
MODEL="${4:?}"
DOMAIN="${5:?}"
IP="${6:-}"

if [ -n "$IP" ]; then
  IP_JSON="\"$IP\""
else
  IP_JSON="null"
fi

curl -sf -X POST "$SERVER_URL/robots" \
  -H "Content-Type: application/json" \
  -d "{\"code\":\"$CODE\",\"name\":\"$NAME\",\"model\":\"$MODEL\",\"ros_domain_id\":$DOMAIN,\"ip\":$IP_JSON}" \
  | python3 -m json.tool
