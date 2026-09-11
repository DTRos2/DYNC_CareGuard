import asyncio
import time
from datetime import datetime, timezone
from typing import Dict, Optional

import psycopg2.extras
from fastapi import APIRouter, Query, WebSocket, WebSocketDisconnect

from . import dashboard_ws
from ..core import db
from ..api.v1 import media

router = APIRouter()

# robot.code -> 명령을 받을 소켓(downlink_node 전용). 로봇 하나당 커넥션 하나만
# 유지한다(같은 코드로 다시 붙으면 이전 항목을 덮어쓴다 — agent가 재연결할 때 흔히
# 벌어지는 상황).
#
# uplink_node(카메라·위치 등 상태를 올리기만 하는 별도 연결)를 이 딕셔너리에 같이
# 담던 시절엔, 와이파이가 끊겼다 붙을 때 uplink가 downlink보다 "나중에" 재연결되면
# 이 자리를 uplink 소켓이 차지해버렸다 — uplink_node는 들어오는 메시지를 안 읽으므로
# (연결 유지 목적으로만 wait_closed()) send_command로 보낸 teleop/save 명령이 그
# 소켓으로 가서 조용히 사라졌다(소켓 자체는 살아있어 서버 입장에선 "전송 성공"으로
# 보여 에러도 안 남았다). 그래서 role(uplink/downlink)로 딕셔너리를 분리했다 —
# _connections는 downlink 전용, 아래 _telemetry_connections는 uplink 전용.
_connections: Dict[str, WebSocket] = {}

# robot.code -> 마지막으로 알린 Nav2 준비 상태(nav_status의 running). GET /robots가
# 같이 내려줘서, P6PatrolPage::refreshRobotState()가 nav_status WS 메시지 유실/지연
# 시에도 이 값으로 navRunning_을 다시 맞출 수 있게 한다(그 전엔 재동기화 경로가 아예
# 없어 메시지 하나만 놓쳐도 이동·순찰 버튼이 새로고침으로도 안 풀렸다, 2026-09-10).
_nav_running: Dict[str, bool] = {}

# robot.code -> 마지막으로 알린 배터리 상태("normal"/"attention"/"return_needed").
# 매 battery_state 메시지마다 시스템 로그를 남기면 스팸이 되므로, 상태가 바뀌는
# 순간(경계를 넘는 순간)에만 한 번 남긴다.
_battery_alert_state: Dict[str, str] = {}

# robot.code -> 마지막으로 임계값을 확인한 시각(time.monotonic()). db.get_connection()이
# 커넥션 풀 없이 매번 원격 DB(192.168.230.13)에 새 TCP 연결을 여는 방식이라, /battery_state가
# 빠르게 계속 들어오면 이 확인 하나가 이벤트 루프를 막아 서버 전체(새 WS 접속 등)가
# 멈추는 걸 실제로 확인했다 — 그래서 이 주기보다 자주는 절대 확인하지 않는다.
_battery_check_interval_sec = 5.0
_last_battery_check: Dict[str, float] = {}

# robot.code -> uplink 소켓. send_command/is_connected는 이 딕셔너리를 절대 보지
# 않는다 — 명령 라우팅과 완전히 분리하기 위한 용도라, 지금은 연결 수명 추적 외의
# 다른 목적으로는 쓰지 않는다.
_telemetry_connections: Dict[str, WebSocket] = {}

# robot.code -> 가장 최근 position 메시지({"x", "y", "yaw"}). 예전엔 position이 오는
# 즉시 대시보드로 relay만 하고 서버 어디에도 안 남겼다 — 그 사이 새로 접속한 대시보드는
# 로봇이 마침 안 움직이고 있으면(다음 position이 언제 올지 알 수 없음) 화면에 위치가
# 하나도 안 떴다. 여기 보관해두고 dashboard_ws.py가 신규 접속 시 한 번 뿌려준다.
_latest_positions: Dict[str, dict] = {}


def is_connected(robot_code: str) -> bool:
    return robot_code in _connections


def connected_robot_codes() -> list:
    # dashboard_ws.py가 신규 접속 시 "지금 온라인인 로봇" 배지를 채우는 데 쓴다
    # (_latest_positions와 같은 이유의 hydration — robot_status는 연결/해제
    # "전환" 시점에만 broadcast되므로, 그 사이에 새로 붙은 대시보드는 이걸 안 부르면
    # 로봇이 이미 오프라인이어도 화면엔 계속 "연결됨"으로 남거나 반대로 이미 온라인인데
    # "모름" 상태로 남는다).
    return list(_connections.keys())


def latest_positions() -> Dict[str, dict]:
    return dict(_latest_positions)


def nav_running(robot_code: str) -> bool:
    return _nav_running.get(robot_code, False)


def _robot_exists(robot_code: str) -> bool:
    # robot_code는 인증 없이 클라이언트가 자유롭게 지정하는 쿼리 파라미터라, DB에
    # 등록된 로봇인지만이라도 확인한다 — media.py/map.py의 같은 조회 패턴을 그대로 쓴다.
    # 토큰 인증까지는 로봇이 여러 대로 늘거나 신뢰 안 되는 네트워크에 나갈 때로 미룬다.
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        return cur.fetchone() is not None


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
async def robot_ws_endpoint(websocket: WebSocket, robot_code: str = Query(...), role: str = Query("downlink")):
    if not _robot_exists(robot_code):
        await websocket.close(code=4404)
        return

    # downlink(명령 수신)와 uplink(상태 송신 전용)를 서로 다른 딕셔너리에 담는다 — 위
    # _connections 주석 참고. role을 안 보내는 클라이언트는 downlink로 취급한다(과거
    # 동작과 호환 + 더 안전한 기본값: 모르면 "명령을 받을 수 있는 연결"로 다뤄서 최소한
    # send_command가 조용히 실패하지 않게 한다).
    connections = _telemetry_connections if role == "uplink" else _connections

    await websocket.accept()
    connections[robot_code] = websocket
    if connections is _connections:
        # "온라인" 배지는 명령이 실제로 전달될 수 있는지(=downlink 연결 여부)를
        # 뜻해야 의미가 있다 — uplink만 살아있어도(카메라는 보이는데 명령은 안 먹히는
        # 지금 같은 상태) "온라인"으로 보이면 사용자가 위험을 못 알아챈다.
        await dashboard_ws.broadcast({"type": "robot_status", "robot_code": robot_code, "online": True})

    try:
        while True:
            message = await websocket.receive_json()
            await _handle_message(robot_code, message)
    except WebSocketDisconnect:
        pass
    finally:
        if connections.get(robot_code) is websocket:
            connections.pop(robot_code, None)
        if connections is _connections:
            await dashboard_ws.broadcast({"type": "robot_status", "robot_code": robot_code, "online": False})


async def _handle_message(robot_code: str, message: dict) -> None:
    msg_type = message.get("type")

    if msg_type == "position":
        position = {"x": message.get("x"), "y": message.get("y"), "yaw": message.get("yaw")}
        _latest_positions[robot_code] = position
        await dashboard_ws.broadcast({"type": "robot_position", "robot_code": robot_code, **position})

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

    elif msg_type == "nav_status":
        # downlink_node._ensure_navigation_running의 성공/실패, 그리고 SLAM 시작이
        # Nav2를 강제로 내리는 지점에서 보낸다 — DB에 남기지 않는 순수 중계(다른 실시간
        # 상태 메시지들과 같은 이유). P6PatrolPage가 "SLAM 중도 아니고 Nav2도 안 뜬
        # 상태"를 구분해서 배너/버튼에 반영하는 데 쓴다.
        _nav_running[robot_code] = bool(message.get("running"))
        await dashboard_ws.broadcast(
            {"type": "nav_status", "robot_code": robot_code, "running": message.get("running")}
        )

    elif msg_type == "patrol_path_update":
        # 순찰(Nav2) 중 planner_server가 발행하는 전역 경로 — rviz2가 보여주는 "로봇이
        # 가려는 길" 선과 같은 것. slam_map_update/slam_scan_update와 같은 이유로
        # 순수 중계만 하고 DB에는 안 남긴다(경로가 매 goal마다 계속 바뀌는 실시간 값).
        await dashboard_ws.broadcast(
            {
                "type": "patrol_path_update",
                "robot_code": robot_code,
                "points": message.get("points"),
            }
        )

    elif msg_type == "camera_frame":
        # 카메라 실시간 미리보기 — slam_map_update/slam_scan_update와 같은 이유로
        # DB에는 안 남기고 순수 중계만 한다.
        await dashboard_ws.broadcast(
            {
                "type": "camera_frame",
                "robot_code": robot_code,
                "jpeg_base64": message.get("jpeg_base64"),
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

    elif msg_type == "battery_state":
        percentage = message.get("percentage")
        now = time.monotonic()
        last_checked = _last_battery_check.get(robot_code, 0.0)
        if percentage is not None and now - last_checked >= _battery_check_interval_sec:
            _last_battery_check[robot_code] = now
            # db.get_connection()이 매번 원격 DB에 새로 접속하는 blocking 호출이라
            # (§_battery_check_interval_sec 주석), 이벤트 루프를 막지 않게 스레드로 뺀다.
            return_map_id = await asyncio.to_thread(_handle_battery_state, robot_code, percentage)
            if return_map_id is not None:
                # 빈 웨이포인트 + return_to_charging=True — _start_patrol이 이미
                # "웨이포인트 다 돌고 충전소로 복귀"로 처리하는 조합을 그대로 재사용한다
                # (0개를 돌면 바로 복귀 단계로 넘어간다). 로봇 쪽에 새 명령을 안 추가해도
                # 되는 이유는 _handle_battery_state 주석 참고.
                await send_command(robot_code, {
                    "type": "patrol_start",
                    "run_id": 0,
                    "waypoints": [],
                    "return_to_charging": True,
                    "map_id": return_map_id,
                })

    elif msg_type == "patrol_done":
        status = message.get("status", "completed")
        _update_patrol_run(message.get("run_id"), status=status, ended=True)
        await dashboard_ws.broadcast(
            {"type": "patrol_done", "robot_code": robot_code, "run_id": message.get("run_id"), "status": status}
        )

    elif msg_type == "medication_done":
        # careguard_mission/medication_task.py가 DeliverMedication 액션이 끝날
        # 때마다(patrol_task.py를 거친 자동 경로든, P3 수동 재안내든) 보낸다 —
        # patrol_done과 같은 토픽(/careguard/patrol_status)을 relay한 것뿐이라
        # 여기 patrol_done 바로 아래 둔다. scheduler.py::_medication_tick()이
        # 'delivered' medication_log를 찾아 "미수령"을 걸러내는데, 지금까지는
        # 이걸 남기는 프로덕션 코드가 하나도 없었다 — 이 핸들러가 그 첫 writer다.
        row = _handle_medication_done(
            robot_code, message.get("schedule_id", 0), message.get("slot_no", 0), message.get("status", "hw_error")
        )
        if row is not None:
            await dashboard_ws.broadcast({
                "type": "medication_done", "robot_code": robot_code,
                "schedule_id": message.get("schedule_id"), "slot_no": message.get("slot_no"),
                "status": message.get("status"),
            })


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
            ON CONFLICT (robot_id, name) DO UPDATE SET
                raw_yaml = EXCLUDED.raw_yaml,
                resolution = EXCLUDED.resolution,
                origin_x = EXCLUDED.origin_x,
                origin_y = EXCLUDED.origin_y,
                origin_yaw = EXCLUDED.origin_yaw,
                width_px = EXCLUDED.width_px,
                height_px = EXCLUDED.height_px,
                is_active = TRUE,
                created_at = NOW()
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
        # map(robot_id, name)엔 UNIQUE 제약이 있다 — 같은 장소를 재매핑해서 같은
        # 이름으로 다시 저장하면(실제로 흔한 시나리오) 예전엔 그냥 INSERT라 여기서
        # UniqueViolation으로 예외가 나 통째로 실패했다(위 ON CONFLICT로 고침). map.id가
        # 안 바뀌어야 그 맵에 이미 등록된 waypoint들이 재저장 후에도 계속 유효하므로,
        # 새 행을 또 만드는 대신 기존 행을 덮어쓰는 쪽을 택했다.
        map_id = cur.fetchone()["id"]

        file_id = message.get("file_id")
        if file_id is not None:
            # 같은 map_id로 다시 저장되는 경우, 그 map_id에 이미 연결돼 있던 예전 이미지
            # 파일이 있으면 "현재 이미지" 후보에서 빼야 한다 — 안 그러면 GET /maps/active의
            # LEFT JOIN files가 같은 맵에 파일 2개를 붙여 행이 중복된다. 실제 파일은
            # 지우지 않는다(soft delete만) — server/storage/maps에 그대로 남겨서 나중에
            # "파일로 등록"으로 여전히 불러올 수 있게 하는 게 이 .yaml 사이드카 기능의
            # 목적 그 자체다.
            cur.execute(
                "UPDATE files SET deleted_at = NOW() "
                "WHERE entity_type = 'map_image' AND entity_id = %s AND id != %s AND deleted_at IS NULL",
                (str(map_id), file_id),
            )
            cur.execute(
                "UPDATE files SET entity_id = %s WHERE id = %s AND entity_type = 'map_image'",
                (str(map_id), file_id),
            )

        cur.execute(
            "UPDATE slam_job SET status = 'saved', map_id = %s, ended_at = NOW() WHERE id = %s",
            (map_id, job_id),
        )
        conn.commit()

    if file_id is not None:
        # .pgm 옆에 실제 .yaml도 남겨야 나중에 "파일로 등록"으로 이 폴더에서
        # 다시 불러올 수 있다 — DB 컬럼에만 있으면 등록 화면이 요구하는 .yaml이
        # 실제로 없어서 불가능하다.
        media.write_map_yaml_sidecar(file_id, message.get("raw_yaml", {}))

    return map_id


def _handle_battery_state(robot_code: str, percentage: float) -> Optional[int]:
    """시스템 설정 > 임계값·알림의 "배터리 주의/복귀" 값과 비교해 임계값을 넘는
    순간에만 system_event를 한 번 남긴다(§_battery_alert_state 주석 참고).

    "복귀" 임계값을 막 넘은 순간(상태가 새로 return_needed가 된 순간)에는, 그
    로봇의 활성 맵 id를 반환한다 — 호출부(비동기 쪽)가 이 값으로 downlink에
    "patrol_start"(웨이포인트 0개 + return_to_charging=True) 명령을 보내
    실제로 충전소 복귀 주행을 시작시킨다. LIMO downlink_node._start_patrol이
    이미 이 조합(빈 웨이포인트 목록 + return_to_charging)을 "그냥 충전소로
    복귀"로 처리하므로(patrol_task.py의 순찰 종료 로직 재사용), 로봇 쪽에는
    새 명령 타입을 추가할 필요가 없다. 활성 맵이 없으면(SLAM 전 로봇 등)
    None을 반환해 호출부가 명령을 보내지 않게 한다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        robot = cur.fetchone()
        if robot is None:
            return None
        robot_id = robot["id"]

        cur.execute(
            "SELECT battery_attention_pct, battery_return_pct FROM robot_threshold WHERE robot_id = %s",
            (robot_id,),
        )
        thresholds = cur.fetchone()
        attention_pct = thresholds["battery_attention_pct"] if thresholds else 30
        return_pct = thresholds["battery_return_pct"] if thresholds else 28

        if percentage <= return_pct:
            new_state = "return_needed"
        elif percentage <= attention_pct:
            new_state = "attention"
        else:
            new_state = "normal"

        previous_state = _battery_alert_state.get(robot_code, "normal")
        _battery_alert_state[robot_code] = new_state
        if new_state == previous_state:
            return None

        if new_state == "attention":
            event_type, severity = "battery_attention", "warn"
            event_message = f"배터리 {percentage:.0f}% — 주의 임계값({attention_pct}%) 이하"
        elif new_state == "return_needed":
            event_type, severity = "battery_return_needed", "error"
            event_message = f"배터리 {percentage:.0f}% — 복귀 임계값({return_pct}%) 이하, 충전 복귀 시작"
        else:
            event_type, severity = "battery_recovered", "info"
            event_message = f"배터리 {percentage:.0f}% — 정상 범위로 회복"

        cur.execute(
            "INSERT INTO system_event (robot_id, event_type, severity, message) VALUES (%s, %s, %s, %s)",
            (robot_id, event_type, severity, event_message),
        )
        conn.commit()

        if new_state != "return_needed":
            return None

        cur.execute(
            "SELECT id FROM map WHERE robot_id = %s AND is_active = TRUE ORDER BY created_at DESC LIMIT 1",
            (robot_id,),
        )
        active_map = cur.fetchone()
        return active_map["id"] if active_map else None


# 로봇이 보내는 DeliverMedication 결과("delivered"/"timeout"/"jammed"/"hw_error"/
# "cancelled" — careguard_msgs/action/DeliverMedication.action 참고)를
# medication_log.result CHECK 제약(schema v1: 'delivered'/'missed'/'not_found'/
# 'error'/'skipped')으로 옮긴다. "timeout"(사람을 못 찾음)은 의미상 not_found가
# 가장 가깝고, "jammed"/"hw_error"는 둘 다 하드웨어 이상이라 error로 뭉뚱그린다
# (둘을 구분해야 할 만큼 운영 절차가 달라지면 그때 CHECK 제약을 늘린다).
# "cancelled"는 사람이 껐거나 순찰이 취소된 경우라 미수령과는 다르므로 skipped.
_MEDICATION_STATUS_TO_RESULT = {
    "delivered": "delivered",
    "timeout": "not_found",
    "jammed": "error",
    "hw_error": "error",
    "cancelled": "skipped",
}


def _handle_medication_done(robot_code: str, schedule_id: int, slot_no: int, status: str) -> Optional[dict]:
    """medication_log에 결과를 남긴다. scheduler.py::_medication_tick()이 오늘자
    'delivered' 기록 유무로 "미수령"을 판단하는데, 지금까지 이걸 실제로 남기는
    프로덕션 코드가 없었다 — 여기가 그 자리다.

    recipient_id는 로봇이 모르므로(액션 goal엔 schedule_id/slot_no만 있음) 여기서
    schedule_id로 medication_schedule을 거꾸로 조회해 채운다. schedule_id가
    0(P3 수동 재안내처럼 특정 회차와 안 묶인 즉시 호출)이면 patrol.py::
    start_patrol_run의 recipient 조회와 같은 방식(로봇에 배정된 대상자 1명)으로
    대체한다 — 그마저 없으면(대상자 미배정 로봇) 기록을 포기한다(로그만 남김).
    """
    result = _MEDICATION_STATUS_TO_RESULT.get(status, "error")
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        robot = cur.fetchone()
        if robot is None:
            return None
        robot_id = robot["id"]

        recipient_id = None
        if schedule_id:
            cur.execute("SELECT recipient_id FROM medication_schedule WHERE id = %s", (schedule_id,))
            medication = cur.fetchone()
            if medication is not None:
                recipient_id = medication["recipient_id"]
        if recipient_id is None:
            cur.execute(
                "SELECT id FROM recipient WHERE robot_id = %s AND deregistered_at IS NULL LIMIT 1",
                (robot_id,),
            )
            recipient = cur.fetchone()
            recipient_id = recipient["id"] if recipient else None
        if recipient_id is None:
            return None

        # P3 "복약 재안내" 버튼 경로는 schedule_id를 아예 모른 채 온다(mission.py 주석
        # 참고 — 특정 회차와 안 묶인 즉시 호출이라 downlink_node.py가 0을 그대로 흘려보냄).
        # 그대로 두면 medication_log.schedule_id가 NULL로 남아, scheduler.py::
        # _medication_tick()의 미수령 판정 쿼리(WHERE schedule_id = %s)가 절대 이 행을
        # 못 찾는다 — 재안내로 실제 배출에 성공해도 원래 놓친 스케줄의 MED_MISSED 경보가
        # 계속 안 풀리는 버그였다. slot_no는 이미 있으니(P3 카드가 화면에 표시 중이던
        # 그 슬롯) recipient_id+slot_no로 활성 medication_schedule을 역으로 찾아 채운다 —
        # 대시보드/로봇 쪽을 안 건드리고 서버 안에서만 고칠 수 있는 지점이라 여기서 처리.
        if not schedule_id and slot_no:
            cur.execute(
                """
                SELECT id FROM medication_schedule
                WHERE recipient_id = %s AND slot_no = %s AND deregistered_at IS NULL
                """,
                (recipient_id, slot_no),
            )
            medication = cur.fetchone()
            if medication is not None:
                schedule_id = medication["id"]

        cur.execute(
            """
            INSERT INTO medication_log (schedule_id, recipient_id, robot_id, slot_no, result, delivered_at, confirmed_by)
            VALUES (%s, %s, %s, %s, %s, %s, %s)
            RETURNING id
            """,
            (
                schedule_id or None, recipient_id, robot_id, slot_no or None, result,
                # IR 센서로 "회수"를 직접 확인해야만 delivered로 오므로(medication_task.py
                # _execute 참고 — waiting_retrieval을 통과해야 closing까지 감), 이 결과는
                # 항상 센서 근거다. 사람이 대시보드에서 수기로 남기는 경로가 생기면 그때
                # confirmed_by='operator'를 쓰는 자리가 따로 필요하다.
                datetime.now(timezone.utc) if result == "delivered" else None,
                "sensor" if result == "delivered" else None,
            ),
        )
        row = cur.fetchone()
        conn.commit()
    return row


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
