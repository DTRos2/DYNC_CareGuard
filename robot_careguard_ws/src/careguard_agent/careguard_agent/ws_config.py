import os

# 대시보드(pc_careguard)와 동일한 관례 — 서버 주소는 환경변수 하나로 통일한다.
SERVER_HTTP_URL = os.environ.get('CAREGUARD_SERVER_URL', 'http://127.0.0.1:8000')
SERVER_WS_URL = SERVER_HTTP_URL.replace('https://', 'wss://').replace('http://', 'ws://')

# 이 로봇의 robot.code (예: "R-01"). ws/robot_ws.py가 접속을 식별하는 키.
ROBOT_CODE = os.environ.get('CAREGUARD_ROBOT_CODE', 'R-01')
