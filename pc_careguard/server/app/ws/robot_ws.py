from typing import Dict, Optional

import psycopg2.extras
from fastapi import APIRouter, Query, WebSocket, WebSocketDisconnect

from . import dashboard_ws
from .. import db

router = APIRouter()

# robot.code -> 연결된 소켓. 로봇 하나당 커넥션 하나만 유지한다(같은 코드로 다시 붙으면
# 이전 항목을 덮어쓴다 — agent가 재연결할 때 흔히 벌어지는 상황).
_connections: Dict[str, WebSocket] = {}


def is_connected(robot_code: str) -> bool:
    return robot_code in _connections


async def send_command(robot_code: str, message: dict) -> bool:
    """robot_code 로봇에 명령을 보낸다. 연결이 없거나 전송에 실패하면 False."""
    ws = _connections.get(robot_code)
    if ws is None:
        return False
    try:
        await ws.send_json(message)
        return True
    except Exception:
        _connections.pop(robot_code, None)
        return False


@router.websocket("/ws/robot")
async def robot_ws_endpoint(websocket: WebSocket, robot_code: str = Query(...)):
    await websocket.accept()
    _connections[robot_code] = websocket
    await dashboard_ws.broadcast({"type": "robot_status", "robot_code": robot_code, "online": True})

    try:
        while True:
            message = await websocket.receive_json()
            await _handle_message(robot_code, message)
    except WebSocketDisconnect:
        pass
    finally:
        if _connections.get(robot_code) is websocket:
            _connections.pop(robot_code, None)
        await dashboard_ws.broadcast({"type": "robot_status", "robot_code": robot_code, "online": False})


async def _handle_message(robot_code: str, message: dict) -> None:
    msg_type = message.get("type")

    if msg_type == "position":
        await dashboard_ws.broadcast(
            {
                "type": "robot_position",
                "robot_code": robot_code,
                "x": message.get("x"),
                "y": message.get("y"),
                "yaw": message.get("yaw"),
            }
        )

    elif msg_type == "slam_progress":
        await dashboard_ws.broadcast(
            {
                "type": "slam_progress",
                "robot_code": robot_code,
                "area_m2": message.get("area_m2"),
                "elapsed_sec": message.get("elapsed_sec"),
            }
        )

    elif msg_type == "slam_map_update":
        # SLAM 진행 중 실시간 미리보기 — DB에 저장하지 않고 그대로 중계만 한다.
        # 완성된 최종 맵은 slam_saved(파일 업로드 + map 행 생성)로 별도 처리된다.
        await dashboard_ws.broadcast(
            {
                "type": "slam_map_update",
                "robot_code": robot_code,
                "width": message.get("width"),
                "height": message.get("height"),
                "resolution": message.get("resolution"),
                "origin_x": message.get("origin_x"),
                "origin_y": message.get("origin_y"),
                "pgm_base64": message.get("pgm_base64"),
            }
        )

    elif msg_type == "slam_scan_update":
        # SLAM 중 실시간 라이다 스캔 오버레이 — slam_map_update와 같은 이유로 DB에는 안 남긴다.
        await dashboard_ws.broadcast(
            {
                "type": "slam_scan_update",
                "robot_code": robot_code,
                "points": message.get("points"),
            }
        )

    elif msg_type == "slam_saved":
        map_id = _save_slam_result(message)
        await dashboard_ws.broadcast(
            {"type": "slam_done", "robot_code": robot_code, "job_id": message.get("job_id"), "map_id": map_id}
        )

    elif msg_type == "patrol_progress":
        _update_patrol_run(message.get("run_id"), waypoints_done=message.get("waypoints_done"))
        await dashboard_ws.broadcast(
            {
                "type": "patrol_progress",
                "robot_code": robot_code,
                "run_id": message.get("run_id"),
                "waypoints_done": message.get("waypoints_done"),
            }
        )

    elif msg_type == "patrol_done":
        status = message.get("status", "completed")
        _update_patrol_run(message.get("run_id"), status=status, ended=True)
        await dashboard_ws.broadcast(
            {"type": "patrol_done", "robot_code": robot_code, "run_id": message.get("run_id"), "status": status}
        )


def _save_slam_result(message: dict) -> Optional[int]:
    """slam_job을 saved로 표시하고 map 행을 새로 만든다.

    맵 이미지 자체는 careguard_agent/media_uploader.py가 이미 POST /media/upload-map으로
    올려서 files.id(file_id)를 갖고 있다 — 여기서는 그 files 행의 entity_id를 새로
    만든 map.id로 다시 연결해준다(업로드 시점엔 아직 map.id가 없어서 임시로
    entity_id=robot_id로 올려뒀다고 가정).
    """
    job_id = message.get("job_id")
    if job_id is None:
        return None

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT robot_id, map_name FROM slam_job WHERE id = %s", (job_id,))
        job = cur.fetchone()
        if job is None:
            return None

        cur.execute("UPDATE map SET is_active = FALSE WHERE robot_id = %s", (job["robot_id"],))
        cur.execute(
            """
            INSERT INTO map (robot_id, name, raw_yaml, resolution, origin_x, origin_y, origin_yaw,
                              width_px, height_px, is_active)
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, TRUE)
            RETURNING id
            """,
            (
                job["robot_id"],
                job["map_name"],
                psycopg2.extras.Json(message.get("raw_yaml", {})),
                message.get("resolution"),
                message.get("origin_x", 0),
                message.get("origin_y", 0),
                message.get("origin_yaw", 0),
                message.get("width_px"),
                message.get("height_px"),
            ),
        )
        map_id = cur.fetchone()["id"]

        file_id = message.get("file_id")
        if file_id is not None:
            cur.execute(
                "UPDATE files SET entity_id = %s WHERE id = %s AND entity_type = 'map_image'",
                (str(map_id), file_id),
            )

        cur.execute(
            "UPDATE slam_job SET status = 'saved', map_id = %s, ended_at = NOW() WHERE id = %s",
            (map_id, job_id),
        )
        conn.commit()

    return map_id


def _update_patrol_run(run_id, **fields) -> None:
    if run_id is None:
        return
    sets = []
    params = []
    if fields.get("waypoints_done") is not None:
        sets.append("waypoints_done = %s")
        params.append(fields["waypoints_done"])
    if "status" in fields:
        sets.append("status = %s")
        params.append(fields["status"])
    if fields.get("ended"):
        sets.append("ended_at = NOW()")
    if not sets:
        return

    params.append(run_id)
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(f"UPDATE patrol_run SET {', '.join(sets)} WHERE id = %s", params)
        conn.commit()
