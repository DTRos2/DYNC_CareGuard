import os

# LIMO용 robot_careguard_ws/src/careguard_agent/ws_config.py와 동일한 관례 —
# 서버 주소·로봇 코드를 환경변수 하나씩으로 통일한다.
SERVER_HTTP_URL = os.environ.get('CAREGUARD_SERVER_URL', 'http://127.0.0.1:8000')
SERVER_WS_URL = SERVER_HTTP_URL.replace('https://', 'wss://').replace('http://', 'ws://')

# 이 로봇의 robot.code (대시보드 "로봇 등록"으로 미리 만들어둔 값, 예: "R-05").
# LIMO 기본값(R-01)과 겹치지 않도록 기본값을 비워둔다 — 실행 시 반드시 넘겨야 한다.
ROBOT_CODE = os.environ.get('CAREGUARD_ROBOT_CODE', '')
