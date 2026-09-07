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

    # 방금 접속한 대시보드는 로봇이 마침 안 움직이고 있으면 다음 position이 올 때까지
    # 화면에 위치가 하나도 안 뜬다 — robot_ws.py가 보관해둔 로봇별 마지막 위치로 한 번
    # 채워준다. robot_ws.py가 이 모듈을 import해 broadcast()를 부르므로(순환 import),
    # 모듈 최상단이 아니라 여기서만 지역 import한다.
    from . import robot_ws
    for robot_code, position in robot_ws.latest_positions().items():
        await websocket.send_json({"type": "robot_position", "robot_code": robot_code, **position})

    # 로봇 온라인 배지도 같은 이유로 hydrate한다 — robot_status는 연결/해제 "전환"
    # 시점에만 broadcast되므로, 안 하면 방금 켠 대시보드는 로봇이 이미 오프라인이어도
    # (또는 이미 온라인이어도) 다음 전환이 일어나기 전까지 정확한 상태를 모른다. 이걸
    # 놓치면 "로봇이 실제로 명령을 못 받는 상태인데 화면엔 아무 경고도 안 뜨는" 위험한
    # 상황(teleop 실패가 조용히 무시되는 것과 겹치면 특히)이 생긴다.
    for robot_code in robot_ws.connected_robot_codes():
        await websocket.send_json({"type": "robot_status", "robot_code": robot_code, "online": True})

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
