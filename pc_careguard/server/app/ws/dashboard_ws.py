from typing import Set

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

router = APIRouter()

# 연결된 모든 대시보드 소켓. 로봇 위치·SLAM 진행률 등은 전부 여기로 그대로 뿌리고
# 필터링(어느 로봇 화면을 보고 있는지)은 대시보드 쪽에서 robot_code로 처리한다.
_connections: Set[WebSocket] = set()


@router.websocket("/ws/dashboard")
async def dashboard_ws_endpoint(websocket: WebSocket):
    await websocket.accept()
    _connections.add(websocket)
    try:
        while True:
            # 대시보드가 서버로 보낼 메시지는 없다 — 연결 유지 목적으로만 수신 대기한다.
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        _connections.discard(websocket)


async def broadcast(message: dict) -> None:
    """연결된 모든 대시보드에 메시지를 보낸다. 끊긴 소켓은 조용히 제거한다."""
    dead = []
    for ws in list(_connections):
        try:
            await ws.send_json(message)
        except Exception:
            dead.append(ws)
    for ws in dead:
        _connections.discard(ws)
