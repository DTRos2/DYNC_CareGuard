import json

from fastapi import WebSocket

# 접속 중인 로봇의 code 집합. DB 컬럼이 아니라 서버 프로세스 메모리로만 관리한다 —
# 재시작하면 리셋되지만 이번 범위(최소 연결)엔 충분하다.
connected_robots: set[str] = set()

# 실시간 상태를 구독 중인 대시보드 WebSocket들.
dashboard_sockets: list[WebSocket] = []


async def broadcast_status(code: str, online: bool) -> None:
    message = json.dumps({"type": "robot_status", "code": code, "online": online})
    dead: list[WebSocket] = []
    for socket in dashboard_sockets:
        try:
            await socket.send_text(message)
        except Exception:
            dead.append(socket)
    for socket in dead:
        dashboard_sockets.remove(socket)
