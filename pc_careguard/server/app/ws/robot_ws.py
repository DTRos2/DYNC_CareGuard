import asyncio
import base64
import json
import math
import time
import uuid
from datetime import datetime
from typing import Dict, Optional, Tuple
from zoneinfo import ZoneInfo

import psycopg2.extras
from fastapi import APIRouter, Query, WebSocket, WebSocketDisconnect

from . import dashboard_ws
from ..core import db
from ..api.v1 import media
from ..api.v1.event import EVENT_TYPE_LABELS

KST = ZoneInfo("Asia/Seoul")

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

# robot.code -> ESP32 긴급 호출로 잠시 밀려난(paused) patrol_run.id. dispatch_drive_to_point가
# severity 있는(SOS) 이동을 보내기 직전에 등록하고, 그 이동의 patrol_done을 받는 순간
# _resume_patrol_run으로 이어서 돈다(_handle_message의 patrol_done 분기 참고). run_id=0인
# "이동"은 patrol_run 행이 없어 patrol_done만으로는 "이 이동이 방금 그 SOS였는지"를 구분할
# 수 없으므로, DB가 아니라 이 in-memory 표식으로 정확히 그 한 번만 재개를 트리거한다.
_pending_resume: Dict[str, int] = {}

# patrol_run.id -> 재개 시점의 waypoints_done(=중단 전까지 완료한 개수). 재개 goal은
# 남은 웨이포인트만 담아 보내므로, patrol_task가 보고하는 feedback의 i는 그 남은
# 목록 안에서의 인덱스(0부터)다 — DB의 waypoints_total 기준 누적값으로 보이려면
# 이 오프셋을 더해야 한다(patrol_progress 처리 참고).
_resume_offsets: Dict[int, int] = {}

# robot.code -> 마지막으로 알린 Nav2 준비 상태(nav_status의 running). GET /robots가
# 같이 내려줘서, P6PatrolPage::refreshRobotState()가 nav_status WS 메시지 유실/지연
# 시에도 이 값으로 navRunning_을 다시 맞출 수 있게 한다(그 전엔 재동기화 경로가 아예
# 없어 메시지 하나만 놓쳐도 이동·순찰 버튼이 새로고침으로도 안 풀렸다, 2026-09-10).
_nav_running: Dict[str, bool] = {}

# robot.code -> 마지막으로 알린 배터리 상태("normal"/"attention"/"return_needed").
# 매 battery_state 메시지마다 시스템 로그를 남기면 스팸이 되므로, 상태가 바뀌는
# 순간(경계를 넘는 순간)에만 한 번 남긴다.
_battery_alert_state: Dict[str, str] = {}

# return_needed/attention에서 벗어나려면(더 나은 상태로 갈 때만) 해당 임계값을 이
# 여유분만큼 더 넘어야 한다. 나빠지는 방향(정상 -> 주의 -> 복귀)은 안전을 위해
# 여유분 없이 즉시 반영한다 — 아래는 return_needed 쪽 여유분만 있을 때 실제로
# 확인된 문제다.
#
# 2026-09-14 R-07(LIMO)에서 실제로 확인된 문제: 복귀 임계값(28%) 근처에서 모터
# 구동/정지에 따라 배터리 %가 27%↔31%로 반복해서 튀는데(전압 처짐·회복 — 이미
# 스무딩 대상인 것과 같은 현상, _smooth_battery_percent() 주석 참고), 여유분 없이
# 그때그때 경계만 비교하면 return_needed <-> normal이 수십 초 간격으로 계속
# 뒤집히면서 system_event가 반복 생성되고, 그때마다 "충전소로 복귀" 명령까지
# 매번 다시 나갔다(patrol_start가 반복 재전송됨).
#
# 처음엔 return_needed 쪽에만 이 여유분을 적용했는데, attention 임계값 근처에서도
# 같은 전압 처짐/회복으로 정확히 같은 증상(배터리 주의 <-> 정상 회복이 번갈아
# 반복 생성)이 재현됐다 — 그래서 attention 쪽 탈출에도 같은 여유분을 적용한다.
# 실측된 4%p 흔들림보다 넉넉하게 잡는다.
_BATTERY_RECOVERY_MARGIN_PCT = 5

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

# robot.code -> 대시보드에 실제로 내려준(화면에 안정적으로 표시되는) 배터리 정수 %.
# 2026-09-12까지는 battery_state를 저배터리 자동 복귀 판단(_handle_battery_state)에만
# 쓰고 대시보드로는 아예 안 보냈다 — P1 통합관제 카드에 배터리 %를 실제 값으로
# 보여주려면 필요해서 추가했다. _latest_positions와 같은 이유로 신규 접속 hydration에도
# 쓴다. 원본이 아니라 _smooth_battery_percent()를 거친 값이라는 점에 주의 — 아래 참고.
_latest_battery: Dict[str, int] = {}

# robot.code -> (지수이동평균으로 다듬은 원시값, 그 값을 계산한 시각(monotonic)).
# _smooth_battery_percent()가 관리하는 내부 상태로, _latest_battery(표시용 정수)와는
# 별개다.
_battery_filter_state: Dict[str, Tuple[float, float]] = {}

# aurix_medirun(RosBatteryTelemetryProvider, 이 로봇들의 원래 프로젝트)에서 이미 검증된
# 값을 그대로 가져왔다.
_BATTERY_PERCENT_TAU_SEC = 5.0
_BATTERY_PERCENT_HYSTERESIS = 0.75


def _smooth_battery_percent(robot_code: str, raw_percent: float) -> int:
    """배터리 % 떨림 방지 — 원본 percentage는 모터 부하에 따른 순간 전압 처짐 때문에
    몇 초 사이에도 82→84→82%처럼 튄다(2026-09-12, 대시보드에서 실제로 확인). 이건
    배터리의 물리적 특성이라 로봇 쪽 원본 신호 자체를 안 튀게 만들 수는 없다.

    aurix_medirun의 RosBatteryTelemetryProvider와 동일한 2단계 방식을 그대로 옮겼다:
    1. 경과 시간(dt) 기반 지수이동평균(EMA)으로 순간 튐을 누그러뜨린다. 고정 alpha가
       아니라 dt로 계산하는 이유는, battery_state가 로봇마다·상황마다 다른 주기로
       들어와도(예: 0.2초 간격 vs 1초 간격) 필터가 "시간당 같은 정도로" 반응하게
       하기 위해서다 — 고정 alpha면 메시지가 자주 올수록 필터가 실제보다 더 굼떠진다.
    2. 화면에 보여줄 정수값은 필터링된 값이 일정 폭(반올림 0.5 + 히스테리시스 0.75
       = 1.25%p) 이상 벗어날 때만 바꾼다 — 이게 없으면 필터링을 거친 값도 반올림
       경계(예: 83.4 ↔ 83.6) 근처에서 표시 숫자가 83↔84로 깜빡일 수 있다.
    """
    now = time.monotonic()
    filtered, last_at = _battery_filter_state.get(robot_code, (raw_percent, now))
    dt = max(0.0, now - last_at)
    alpha = 1.0 - math.exp(-dt / _BATTERY_PERCENT_TAU_SEC)
    filtered = filtered + alpha * (raw_percent - filtered)
    _battery_filter_state[robot_code] = (filtered, now)

    displayed = _latest_battery.get(robot_code)
    if displayed is None or abs(filtered - displayed) >= (0.5 + _BATTERY_PERCENT_HYSTERESIS):
        displayed = round(filtered)
    return displayed


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


def latest_battery() -> Dict[str, int]:
    return dict(_latest_battery)


def nav_running(robot_code: str) -> bool:
    return _nav_running.get(robot_code, False)


def register_pending_resume(robot_code: str, run_id: int) -> None:
    """patrol.py::dispatch_drive_to_point가 SOS 이동을 보내기 직전에 부른다 —
    이 이동이 끝나는 순간(patrol_done) run_id의 순찰을 이어서 재개하라는 표식."""
    _pending_resume[robot_code] = run_id


def clear_pending_resume(robot_code: str) -> None:
    """patrol.py::cancel_patrol_run(취소)이 부른다 — paused 상태로 남겨둔 순찰을
    사용자가 아예 취소해버리면, SOS 이동이 끝나도 더 이상 이어서 돌면 안 된다."""
    _pending_resume.pop(robot_code, None)


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
        # pos_std_m(위치 표준편차, 미터) — LIMO uplink_node._on_pose가 /amcl_pose
        # 공분산에서 뽑아 보낸다. P6 "위치 보정" 직후 AMCL이 실제로 수렴했는지
        # 판단하는 데 쓴다(P6PatrolPage::poseCaptureSettling_). TF 기반 경로
        # (_on_slam_overlay_tick, P5 SLAM 전용)는 공분산이 없어 이 필드를 안 보내며,
        # 그때는 None으로 그대로 넘어간다.
        position = {
            "x": message.get("x"),
            "y": message.get("y"),
            "yaw": message.get("yaw"),
            "pos_std_m": message.get("pos_std_m"),
        }
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
        run_id = message.get("run_id")
        raw_done = message.get("waypoints_done")
        # 재개된 순찰(_resume_patrol_run)은 남은 웨이포인트만 담아 보내므로, patrol_task가
        # 보고하는 i는 그 목록 안에서의 인덱스(0부터)다 — 중단 전까지 완료한 개수를
        # 더해야 DB/화면에 누적 진행률이 정확히 보인다(모듈 상단 _resume_offsets 참고).
        offset = _resume_offsets.get(run_id, 0)
        waypoints_done = raw_done + offset if raw_done is not None else raw_done
        _update_patrol_run(run_id, waypoints_done=waypoints_done)
        await dashboard_ws.broadcast(
            {
                "type": "patrol_progress",
                "robot_code": robot_code,
                "run_id": run_id,
                "waypoints_done": waypoints_done,
            }
        )

    elif msg_type == "battery_state":
        percentage = message.get("percentage")
        if percentage is not None:
            # 예전엔 대시보드(P1OverviewPage) 쪽에서 각자 스무딩했는데, 그러면 배터리를
            # 보여주는 화면이 하나 늘 때마다 스무딩도 따로 구현해야 해서 여기(단일 소스)로
            # 옮겼다 — 이후로 모든 대시보드 클라이언트는 이미 다듬어진 값만 받는다.
            smoothed = _smooth_battery_percent(robot_code, percentage)
            _latest_battery[robot_code] = smoothed
            # 저배터리 임계값 확인(DB 조회)과 달리 대시보드 중계는 가벼운 broadcast
            # 하나뿐이라, 5초 스로틀 없이 메시지가 오는 대로 그대로 보낸다.
            await dashboard_ws.broadcast(
                {"type": "battery_state", "robot_code": robot_code, "percentage": smoothed}
            )
        now = time.monotonic()
        last_checked = _last_battery_check.get(robot_code, 0.0)
        if percentage is not None and now - last_checked >= _battery_check_interval_sec:
            _last_battery_check[robot_code] = now
            # db.get_connection()이 매번 원격 DB에 새로 접속하는 blocking 호출이라
            # (§_battery_check_interval_sec 주석), 이벤트 루프를 막지 않게 스레드로 뺀다.
            # 원본 percentage가 아니라 smoothed(위에서 이미 계산한 EMA 값)를 넘긴다 —
            # 표시값과 같은 소스를 봐야 "화면엔 안 튀는데 복귀 알림만 계속 뒤집힌다"는
            # 불일치가 안 생기고, 임계값 판정 자체도 원본의 순간 튐에 덜 흔들린다.
            return_map_id = await asyncio.to_thread(_handle_battery_state, robot_code, smoothed)
            if return_map_id is not None:
                # 빈 웨이포인트 + return_to_charging=True — _start_patrol이 이미
                # "웨이포인트 다 돌고 충전소로 복귀"로 처리하는 조합을 그대로 재사용한다
                # (0개를 돌면 바로 복귀 단계로 넘어간다). 로봇 쪽에 새 명령을 안 추가해도
                # 되는 이유는 _handle_battery_state 주석 참고.
                # override=True — 저배터리 복귀도 SOS와 같은 이유로 지금 순찰/이동
                # 중이어도 밀어내고 우선 실행돼야 한다(_ensure_patrol_task_running 참고).
                # 다만 복귀는 끝난 뒤 원래 순찰을 이어서 재개하지 않는다(요청 사양 —
                # SOS만 재개 대상).
                await send_command(robot_code, {
                    "type": "patrol_start",
                    "run_id": 0,
                    "waypoints": [],
                    "return_to_charging": True,
                    "map_id": return_map_id,
                    "override": True,
                })

    elif msg_type == "dock_result":
        # 충전소 ArUco 도킹 정렬(_dock_with_aruco) 종료 보고 — nav_status와 같은 이유로
        # DB에는 안 남기고 순수 중계만 한다. 실패해도 patrol_done/nav 성공 여부와는
        # 별개로만 알리는 용도(로봇은 충전 웨이포인트 자체엔 이미 도착해 있다).
        await dashboard_ws.broadcast(
            {"type": "dock_result", "robot_code": robot_code, "docked": bool(message.get("docked"))}
        )

    elif msg_type == "patrol_done":
        run_id = message.get("run_id")
        status = message.get("status", "completed")
        _update_patrol_run(run_id, status=status, ended=True)
        _resume_offsets.pop(run_id, None)
        await dashboard_ws.broadcast(
            {"type": "patrol_done", "robot_code": robot_code, "run_id": run_id, "status": status}
        )
        # SOS 이동(run_id=0)이 막 끝났고, 그게 밀어낸 순찰(paused)이 있으면 이어서
        # 재개한다 — register_pending_resume 문서 참고. 이 표식은 정확히 그 SOS
        # 이동 하나만을 가리키므로 pop으로 한 번 쓰고 바로 지운다(중복 재개 방지).
        resume_run_id = _pending_resume.pop(robot_code, None)
        if resume_run_id is not None:
            await _resume_patrol_run(resume_run_id)

    elif msg_type == "intruder_detected":
        # careguard_perception/patrol_intruder_watch_node.py가 순찰 중 이 로봇에
        # 배정된 대상자 누구와도 얼굴이 일치하지 않는 트랙을 쿨다운 간격으로 신고하면
        # 온다(그 노드 모듈 docstring 참고). ESP32 호출 버튼(mqtt_listener.py)과 달리
        # 이 이벤트는 로봇 스스로의 판단이라 recipient_id를 못 채운다(아무도 매치 안
        # 됐다는 뜻 그 자체이므로 NULL이 맞음).
        event_payload = _handle_intruder_detected(robot_code, message)
        if event_payload is not None:
            await dashboard_ws.broadcast(event_payload)


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


def _handle_intruder_detected(robot_code: str, message: dict) -> Optional[dict]:
    """care_event(INTRUDER) 생성 — scheduler.py의 MED_MISSED 자동 생성과 같은 패턴
    (event_uid 생성 -> INSERT -> P2 이벤트 큐가 기대하는 모양으로 dashboard_ws에
    브로드캐스트). ESP32 호출 버튼(mqtt_listener.py)과 달리 MQTT가 아니라 이 로봇이
    이미 맺어둔 uplink WS 연결로 오므로 새 브로커 구독을 안 만들고 여기서 바로
    처리한다.

    로봇이 등록 해제됐거나(이론상 메시지가 오는 사이 등록 해제된 경우) 아직 DB에
    없는 코드라면 None을 반환해 호출부가 브로드캐스트를 건너뛰게 한다.
    """
    now = datetime.now(KST)
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        robot = cur.fetchone()
        if robot is None:
            return None
        robot_id = robot["id"]

        # 이 로봇이 지금 돌고 있는 순찰 회차에 이벤트를 묶어둔다(있으면) — perception
        # 노드는 run_id를 문자 그대로는 몰라도(patrol_intruder_watch_node.py가 받은
        # run_id를 evidence에만 참고용으로 싣는다) DB 쪽 patrol_run_id 연결은 여기서
        # 정확하게 다시 조회해서 채운다.
        cur.execute(
            "SELECT id FROM patrol_run WHERE robot_id = %s AND status = 'running' "
            "ORDER BY started_at DESC LIMIT 1",
            (robot_id,),
        )
        run = cur.fetchone()
        patrol_run_id = run["id"] if run else None

        position = _latest_positions.get(robot_code)
        pos_x = position["x"] if position else None
        pos_y = position["y"] if position else None

        event_uid = f"EV-{robot_code}-{now:%y%m%d}-{uuid.uuid4().hex[:8]}"
        summary = "순찰 중 미등록 인물이 감지되었습니다"
        evidence = {
            "track_id": message.get("track_id"),
            # 정지→접근→재판별(patrol_intruder_watch_node.py)로 가까이서 다시 찍어
            # 확인한 결과인지, 원거리 1회 판정을 그대로 쓴 것인지 — 담당자가 신뢰도를
            # 가늠하는 데 참고할 수 있게 남긴다.
            "verified_at_close_range": message.get("verified_at_close_range"),
            "nearest_dist": message.get("nearest_dist"),
            "perception_run_id": message.get("run_id"),
        }
        cur.execute(
            """
            INSERT INTO care_event (event_uid, recipient_id, robot_id, patrol_run_id, trigger_path,
                event_type, severity, summary, evidence, pos_x, pos_y, occurred_at)
            VALUES (%s, NULL, %s, %s, 'B', 'INTRUDER', 'emergency', %s, %s, %s, %s, NOW())
            RETURNING id, occurred_at
            """,
            (event_uid, robot_id, patrol_run_id, summary, json.dumps(evidence), pos_x, pos_y),
        )
        row = cur.fetchone()
        event_id, occurred_at = row["id"], row["occurred_at"]

        # patrol_intruder_watch_node.py가 확정 순간의 얼굴 캡처를 base64로 같이
        # 실어 보낸다(files 테이블 코멘트에 명시된 "침입자 캡처" 용도). 캡처 저장이
        # 실패해도(인코딩 오류, 디스크 문제 등) 이벤트 자체는 이미 커밋 대상이라
        # 그대로 살려둔다 — 사진은 있으면 좋은 것이지 신고를 막을 이유는 아니다.
        image_b64 = message.get("image_base64")
        if image_b64:
            try:
                image_bytes = base64.b64decode(image_b64)
                file_id = media.save_intruder_capture(robot_code, event_id, image_bytes)
                evidence["capture_file_id"] = file_id
                cur.execute(
                    "UPDATE care_event SET evidence = %s WHERE id = %s",
                    (json.dumps(evidence), event_id),
                )
            except Exception:
                pass

        conn.commit()

    return {
        "type": "event_created",
        "event_uid": event_uid,
        "severity": "emergency",
        "event_type": "INTRUDER",
        "event_type_label": EVENT_TYPE_LABELS["INTRUDER"],
        "summary": summary,
        "occurred_at": occurred_at.isoformat(),
        "resolved": False,
        "robot_code": robot_code,
        "recipient_name": None,
    }


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

        previous_state = _battery_alert_state.get(robot_code, "normal")

        if previous_state == "return_needed" and percentage <= return_pct + _BATTERY_RECOVERY_MARGIN_PCT:
            # 여유분(_BATTERY_RECOVERY_MARGIN_PCT)을 확실히 넘기 전까지는
            # return_needed를 유지한다 — return_pct를 살짝 넘나드는 값(모듈 상단
            # 주석 참고)만으로 정상 복귀로 인정하지 않는다.
            new_state = "return_needed"
        elif percentage <= return_pct:
            new_state = "return_needed"
        elif previous_state == "attention" and percentage <= attention_pct + _BATTERY_RECOVERY_MARGIN_PCT:
            # return_needed와 같은 이유 — attention_pct 근처에서도 같은 폭으로
            # 흔들려 "배터리 주의" <-> "정상 회복"이 번갈아 반복 생성되는 게
            # 실제로 확인됐다(모듈 상단 주석 참고).
            new_state = "attention"
        elif percentage <= attention_pct:
            new_state = "attention"
        else:
            new_state = "normal"

        _battery_alert_state[robot_code] = new_state
        if new_state == previous_state:
            return None

        # "정상 회복" 로그는 남기지 않는다 — 사용자 요청(2026-09-14): 주의/복귀 알림만
        # 있으면 되고, 그게 풀렸다는 별도 로그는 필요 없다고 판단됨. 상태 추적
        # (_battery_alert_state)은 히스테리시스 판정에 계속 필요해서 위에서 그대로
        # 갱신하고, system_event만 안 남긴다.
        if new_state == "normal":
            return None

        if new_state == "attention":
            event_type, severity = "battery_attention", "warn"
            event_message = f"배터리 {percentage:.0f}% — 주의 임계값({attention_pct}%) 이하"
        else:
            event_type, severity = "battery_return_needed", "error"
            event_message = f"배터리 {percentage:.0f}% — 복귀 임계값({return_pct}%) 이하, 충전 복귀 시작"

        cur.execute(
            "INSERT INTO system_event (robot_id, event_type, severity, message) VALUES (%s, %s, %s, %s)",
            (robot_id, event_type, severity, event_message),
        )
        conn.commit()
        # 이 함수는 asyncio.to_thread로 스레드풀에서 도니(위 호출부 참고) 그 스레드엔
        # 실행 중인 이벤트 루프가 없다 — accounts.py와 같은 이유로 broadcast_threadsafe를 쓴다.
        dashboard_ws.broadcast_threadsafe({"type": "system_event_created"})

        if new_state != "return_needed":
            return None

        cur.execute(
            "SELECT id FROM map WHERE robot_id = %s AND is_active = TRUE ORDER BY created_at DESC LIMIT 1",
            (robot_id,),
        )
        active_map = cur.fetchone()
        return active_map["id"] if active_map else None


async def _resume_patrol_run(run_id: int) -> None:
    """SOS 이동이 끝난 뒤 그게 밀어낸 순찰(status='paused')을 이어서 돌린다 —
    _handle_message의 patrol_done 분기가 부른다. 이미 완료한 웨이포인트는 다시
    보내지 않고 patrol_route.steps에서 waypoints_done개를 건너뛴 나머지만 새
    goal로 보낸다(patrol_task는 매번 받은 goal 전체를 처음부터 도는 구조라 —
    patrol.py::start_patrol_run과 달리 여기서 "남은 것"을 직접 계산해야 한다).
    override는 안 준다 — 이 시점엔 patrol_task가 SOS 이동을 막 끝내고 비어있는
    상태라(_on_patrol_result), 강제로 밀어낼 대상이 없다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT pr.robot_id, pr.route_id, pr.waypoints_done, pr.waypoints_total,
                   r.code AS robot_code, rt.map_id, rt.steps
            FROM patrol_run pr
            JOIN robot r ON r.id = pr.robot_id
            JOIN patrol_route rt ON rt.id = pr.route_id
            WHERE pr.id = %s AND pr.status = 'paused'
            """,
            (run_id,),
        )
        row = cur.fetchone()
        if row is None:
            return  # 이미 취소됐거나(cancel_patrol_run) 다른 경로로 정리된 경우 — 조용히 무시.

        remaining_ids = (row["steps"] or [])[row["waypoints_done"]:]
        if not remaining_ids:
            # 하필 SOS가 끼어든 시점이 마지막 웨이포인트 직후였다면 남은 게 없다 —
            # 그대로 완료 처리.
            cur.execute(
                "UPDATE patrol_run SET status = 'completed', ended_at = NOW() WHERE id = %s", (run_id,)
            )
            conn.commit()
            await dashboard_ws.broadcast({"type": "patrol_run_updated", "robot_code": row["robot_code"]})
            return

        cur.execute("SELECT id, code, pos_x, pos_y, yaw FROM waypoint WHERE id = ANY(%s)", (remaining_ids,))
        by_id = {w["id"]: w for w in cur.fetchall()}
        waypoints = [by_id[wid] for wid in remaining_ids if wid in by_id]

        cur.execute("UPDATE patrol_run SET status = 'running' WHERE id = %s", (run_id,))
        conn.commit()
        robot_code = row["robot_code"]
        map_id = row["map_id"]

    _resume_offsets[run_id] = row["waypoints_done"]
    await send_command(robot_code, {
        "type": "patrol_start",
        "run_id": run_id,
        "waypoints": [{"code": w["code"], "x": w["pos_x"], "y": w["pos_y"], "yaw": w["yaw"]} for w in waypoints],
        "map_id": map_id,
    })
    await dashboard_ws.broadcast({"type": "patrol_run_updated", "robot_code": robot_code})


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
