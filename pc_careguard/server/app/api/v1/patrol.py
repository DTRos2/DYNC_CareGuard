import asyncio
from datetime import date, timedelta
from typing import List, Optional

import anyio
import psycopg2.extras
from fastapi import APIRouter, HTTPException, Query

from ...core import db
from ...schemas import (
    CancelPatrolRunRequest,
    DriveToPointRequest,
    DriveToPointResponse,
    PatrolRouteCreate,
    PatrolRouteItem,
    PatrolRouteUpdate,
    PatrolRunCreate,
    PatrolRunItem,
    PatrolRunLogItem,
    PatrolScheduleCreate,
    PatrolScheduleItem,
    PatrolScheduleUpdate,
    PrepareNavigationRequest,
    SetInitialPoseRequest,
    ShutdownRobotRequest,
)
from ...ws import dashboard_ws, robot_ws


def _reject_charge_in_steps(cur, steps: List[int]) -> None:
    """patrol_route.steps에는 charge(충전 스테이션, 자동 출발·복귀 전용) 웨이포인트를
    넣을 수 없다 — DB CHECK로는 막을 수 없어(steps는 그냥 정수 배열) 여기서 저장
    시점에 막는다. 프런트가 이미 charge를 클릭 못 하게 막고 있지만(WaypointCanvas),
    API를 직접 호출하면 우회할 수 있었다. patrol/emergency는 둘 다 경로에 넣을 수
    있다(emergency도 순서대로 순찰 도는 지점 중 하나로 취급 — 요청 사양).
    """
    if not steps:
        return
    cur.execute("SELECT id, kind FROM waypoint WHERE id = ANY(%s)", (list(steps),))
    kind_by_id = {row["id"]: row["kind"] for row in cur.fetchall()}
    for step_id in steps:
        kind = kind_by_id.get(step_id)
        if kind is None:
            raise HTTPException(status_code=400, detail=f"존재하지 않는 웨이포인트입니다: {step_id}")
        if kind == "charge":
            raise HTTPException(
                status_code=400,
                detail=f"충전 스테이션은 경로에 넣을 수 없습니다: waypoint {step_id}",
            )


routes_router = APIRouter(prefix="/patrol-routes", tags=["patrol"])
schedules_router = APIRouter(prefix="/patrol-schedules", tags=["patrol"])

# 순찰 실행(실제 "순찰 시작" 트리거). schedule(자동)·manual(대시보드 버튼) 둘 다 여기로 모인다.
runs_router = APIRouter(prefix="/patrol-runs", tags=["patrol"])


@routes_router.get("", response_model=List[PatrolRouteItem])
def list_patrol_routes(map_id: Optional[int] = Query(None, description="맵으로 필터링")):
    """순찰 경로 목록. steps는 방문할 waypoint id를 순서대로 담은 배열이다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT id, map_id, name, description, steps, created_by, created_at
            FROM patrol_route
            WHERE deregistered_at IS NULL
                AND (%(map_id)s IS NULL OR map_id = %(map_id)s)
            ORDER BY id
            """,
            {"map_id": map_id},
        )
        rows = cur.fetchall()

    return [PatrolRouteItem(**row) for row in rows]


@routes_router.post("", response_model=PatrolRouteItem, status_code=201)
def create_patrol_route(payload: PatrolRouteCreate):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT id FROM patrol_route WHERE map_id = %s AND name = %s AND deregistered_at IS NULL",
            (payload.map_id, payload.name),
        )
        if cur.fetchone():
            raise HTTPException(status_code=409, detail="이미 같은 이름의 경로가 있습니다")

        _reject_charge_in_steps(cur, payload.steps)

        cur.execute(
            """
            INSERT INTO patrol_route (map_id, name, description, steps, created_by)
            VALUES (%s, %s, %s, %s, %s)
            RETURNING id, map_id, name, description, steps, created_by, created_at
            """,
            (
                payload.map_id,
                payload.name,
                payload.description,
                psycopg2.extras.Json(payload.steps),
                payload.created_by,
            ),
        )
        row = cur.fetchone()
        conn.commit()

    anyio.from_thread.run(dashboard_ws.broadcast, {"type": "patrol_routes_changed", "map_id": row["map_id"]})

    return PatrolRouteItem(**row)


@routes_router.put("/{route_id}", response_model=PatrolRouteItem)
def update_patrol_route(route_id: int, payload: PatrolRouteUpdate):
    fields = payload.dict(exclude_unset=True)
    if not fields:
        raise HTTPException(status_code=400, detail="변경할 내용이 없습니다")

    with db.get_connection() as conn, conn.cursor() as cur:
        if "steps" in fields:
            _reject_charge_in_steps(cur, fields["steps"])
            fields["steps"] = psycopg2.extras.Json(fields["steps"])

        sets = ", ".join(f"{key} = %s" for key in fields)
        params = list(fields.values()) + [route_id]

        cur.execute(
            f"""
            UPDATE patrol_route SET {sets}
            WHERE id = %s AND deregistered_at IS NULL
            RETURNING id, map_id, name, description, steps, created_by, created_at
            """,
            params,
        )
        row = cur.fetchone()
        conn.commit()

    if row is None:
        raise HTTPException(status_code=404, detail="경로를 찾을 수 없습니다")

    anyio.from_thread.run(dashboard_ws.broadcast, {"type": "patrol_routes_changed", "map_id": row["map_id"]})

    return PatrolRouteItem(**row)


@routes_router.delete("/{route_id}", status_code=204)
def delete_patrol_route(route_id: int):
    """경로를 삭제한다. 이 경로를 쓰는 순찰 스케줄도 함께 비활성화한다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "UPDATE patrol_route SET deregistered_at = NOW() WHERE id = %s AND deregistered_at IS NULL RETURNING map_id",
            (route_id,),
        )
        row = cur.fetchone()
        if row is None:
            conn.rollback()
            raise HTTPException(status_code=404, detail="경로를 찾을 수 없습니다")

        cur.execute(
            "UPDATE patrol_schedule SET deregistered_at = NOW() WHERE route_id = %s AND deregistered_at IS NULL",
            (route_id,),
        )
        conn.commit()

    anyio.from_thread.run(dashboard_ws.broadcast, {"type": "patrol_routes_changed", "map_id": row["map_id"]})


@schedules_router.get("", response_model=List[PatrolScheduleItem])
def list_patrol_schedules(route_id: int = Query(..., description="대상 경로 id")):
    """경로 실행 예약 목록. 한 경로에 여러 시각이 등록될 수 있다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT id, route_id, run_time, days_of_week, medication_schedule_id, created_by, created_at,
                   (deregistered_at IS NULL) AS enabled
            FROM patrol_schedule
            WHERE route_id = %s AND deregistered_at IS NULL
            ORDER BY run_time
            """,
            (route_id,),
        )
        rows = cur.fetchall()

    return [PatrolScheduleItem(**row) for row in rows]


@schedules_router.post("", response_model=PatrolScheduleItem, status_code=201)
def create_patrol_schedule(payload: PatrolScheduleCreate):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT id FROM patrol_schedule
            WHERE route_id = %s AND run_time = %s AND deregistered_at IS NULL
            """,
            (payload.route_id, payload.run_time),
        )
        if cur.fetchone():
            raise HTTPException(status_code=409, detail="같은 시각의 스케줄이 이미 있습니다")

        cur.execute(
            """
            INSERT INTO patrol_schedule (route_id, run_time, days_of_week, medication_schedule_id, created_by)
            VALUES (%s, %s, %s, %s, %s)
            RETURNING id, route_id, run_time, days_of_week, medication_schedule_id, created_by, created_at,
                      (deregistered_at IS NULL) AS enabled
            """,
            (payload.route_id, payload.run_time, payload.days_of_week, payload.medication_schedule_id,
             payload.created_by),
        )
        row = cur.fetchone()
        conn.commit()

    return PatrolScheduleItem(**row)


@schedules_router.put("/{schedule_id}", response_model=PatrolScheduleItem)
def update_patrol_schedule(schedule_id: int, payload: PatrolScheduleUpdate):
    """run_time/days_of_week 수정 + enabled 토글(대시보드 ToggleSwitch).

    DB엔 별도 enabled 컬럼이 없어 deregistered_at으로 대신한다: False -> NOW(), True -> NULL.
    """
    provided = payload.dict(exclude_unset=True)
    enabled = provided.pop("enabled", None)

    sets = [f"{key} = %s" for key in provided]
    params = list(provided.values())
    if enabled is not None:
        sets.append("deregistered_at = NULL" if enabled else "deregistered_at = NOW()")
    if not sets:
        raise HTTPException(status_code=400, detail="변경할 내용이 없습니다")

    params.append(schedule_id)
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            f"""
            UPDATE patrol_schedule SET {', '.join(sets)}
            WHERE id = %s
            RETURNING id, route_id, run_time, days_of_week, medication_schedule_id, created_by, created_at,
                      (deregistered_at IS NULL) AS enabled
            """,
            params,
        )
        row = cur.fetchone()
        conn.commit()

    if row is None:
        raise HTTPException(status_code=404, detail="스케줄을 찾을 수 없습니다")
    return PatrolScheduleItem(**row)


@schedules_router.delete("/{schedule_id}", status_code=204)
def delete_patrol_schedule(schedule_id: int):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "UPDATE patrol_schedule SET deregistered_at = NOW() WHERE id = %s AND deregistered_at IS NULL",
            (schedule_id,),
        )
        if cur.rowcount == 0:
            conn.rollback()
            raise HTTPException(status_code=404, detail="스케줄을 찾을 수 없습니다")
        conn.commit()


def _reject_if_slam_active(cur, robot_id: int) -> None:
    """이 로봇에 진행 중인(status='queued') slam_job이 있으면 409를 던진다.

    downlink_node._ensure_navigation_running이 로봇 쪽에서도 SLAM 중엔 Nav2를 안
    띄우고 조용히 로그만 남기고 거부하지만(로봇→서버→PC로 그 실패가 전혀 전달되지
    않는다), 그 상태로 서버가 200을 돌려주면 patrol_run이 status='running'으로 DB에
    남은 채 patrol_done을 영원히 못 받는 고아 기록이 된다(2026-09-10 발견한
    "robot_id=4에 8건 stuck" 레코드와 같은 패턴). 여기서 먼저 걸러 로봇에 명령 자체를
    안 보내면, 대시보드 수동 클릭과 services/scheduler.py 자동 트리거(둘 다
    start_patrol_run을 그대로 호출) 양쪽 다 한 번에 막힌다.
    """
    cur.execute("SELECT 1 FROM slam_job WHERE robot_id = %s AND status = 'queued'", (robot_id,))
    if cur.fetchone() is not None:
        raise HTTPException(status_code=409, detail="이 로봇은 SLAM 매핑 중이라 순찰/이동을 시작할 수 없습니다")


def _reject_if_already_patrolling(cur, robot_id: int) -> None:
    """이 로봇에 이미 진행 중(running)이거나 ESP32 긴급 호출로 잠시 밀려난(paused)
    patrol_run이 있으면 409를 던진다 — 새 수동 순찰/이동이 그걸 밀어내고 로봇을
    가로채지 못하게 막는다.

    이게 없던 시절엔 downlink_node._ensure_patrol_task_running이 "map_id가 같아도
    매번 무조건 재시작"해서, P6 페이지를 새로고침만 해도(nav_prepare) 지금 돌고
    있는 순찰이 SIGINT로 조용히 죽었다(2026-09-15 R-07 스케줄 순찰 stuck 사고 원인).
    여기서 먼저 막으면 로봇에 명령 자체가 안 나가므로 그 사고가 재발하지 않는다.

    ESP32 SOS(dispatch_drive_to_point, severity 있음)는 이 검사를 타지 않는다 —
    대신 기존 순찰을 paused로 남기고 끼어든다(같은 함수의 severity 분기 참고).
    """
    cur.execute(
        "SELECT 1 FROM patrol_run WHERE robot_id = %s AND status IN ('running', 'paused')", (robot_id,)
    )
    if cur.fetchone() is not None:
        raise HTTPException(status_code=409, detail="이미 순찰/이동 중입니다 — 완료 후 다시 시도해주세요")


async def start_patrol_run(
    robot_id: int, route_id: int, trigger: str, schedule_id: Optional[int] = None
) -> PatrolRunItem:
    """순찰 1회차를 시작한다. patrol_run을 기록하고 로봇에 웨이포인트 순서를 전송한다.

    대시보드 "순찰 시작" 버튼(POST /patrol-runs)과 services/scheduler.py의 자동 트리거가
    공유하는 핵심 로직 — trigger만 "manual"/"schedule"로 다르다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT code FROM robot WHERE id = %s AND deregistered_at IS NULL", (robot_id,))
        robot = cur.fetchone()
        if robot is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
        _reject_if_slam_active(cur, robot_id)
        _reject_if_already_patrolling(cur, robot_id)

        cur.execute(
            "SELECT map_id, steps FROM patrol_route WHERE id = %s AND deregistered_at IS NULL",
            (route_id,),
        )
        route = cur.fetchone()
        if route is None:
            raise HTTPException(status_code=404, detail="경로를 찾을 수 없습니다")

        steps = route["steps"] or []
        waypoints = []
        if steps:
            cur.execute("SELECT id, code, pos_x, pos_y, yaw FROM waypoint WHERE id = ANY(%s)", (steps,))
            by_id = {w["id"]: w for w in cur.fetchall()}
            waypoints = [by_id[wid] for wid in steps if wid in by_id]

        cur.execute(
            "SELECT id FROM recipient WHERE robot_id = %s AND deregistered_at IS NULL LIMIT 1",
            (robot_id,),
        )
        recipient = cur.fetchone()

        cur.execute(
            """
            INSERT INTO patrol_run (robot_id, recipient_id, route_id, schedule_id, trigger, status, waypoints_total)
            VALUES (%s, %s, %s, %s, %s, 'running', %s)
            RETURNING id, robot_id, recipient_id, route_id, schedule_id, trigger, status,
                      waypoints_total, waypoints_done, started_at, ended_at
            """,
            (robot_id, recipient["id"] if recipient else None, route_id, schedule_id, trigger, len(waypoints)),
        )
        row = cur.fetchone()
        conn.commit()

    # P8 통합 로그의 "순찰 기록" 탭이 새로고침 없이도 방금 시작된 실행을 바로 보게 한다
    # (2026-09-12 추가) — patrol_progress/patrol_done은 이미 broadcast되고 있었지만
    # "시작" 시점 자체를 알리는 메시지는 없었다.
    await dashboard_ws.broadcast({"type": "patrol_run_created"})
    # P6PatrolPage(팀원 전원의 대시보드)가 이 로봇의 "주행 중" 배너·버튼 잠금을 이
    # 페이지 자신이 시작한 게 아니어도(스케줄 자동 트리거·다른 팀원 PC) 즉시
    # 반영하게 한다 — cancel_patrol_run이 이미 쓰던 것과 같은 타입(refreshRobotState로
    # 재조회, 2026-09-15 추가: 그 전엔 patrol_progress로 첫 웨이포인트 완료 메시지가
    # 올 때까지 배너가 안 뜨는 공백이 있었다).
    await dashboard_ws.broadcast({"type": "patrol_run_updated", "robot_code": robot["code"]})

    connected = await robot_ws.send_command(
        robot["code"],
        {
            "type": "patrol_start",
            "run_id": row["id"],
            "waypoints": [{"code": w["code"], "x": w["pos_x"], "y": w["pos_y"], "yaw": w["yaw"]} for w in waypoints],
            # downlink_node가 이걸로 Nav2를 자동으로 준비한다(맵 다운로드+런치+초기
            # 위치) — 없으면 사용자가 SSH로 navigation_diff_launch.py를 직접 켜야
            # 순찰 중 로봇 위치가 대시보드에 뜬다.
            "map_id": route["map_id"],
        },
    )
    return PatrolRunItem(**row, robot_connected=connected)


@runs_router.get("", response_model=List[PatrolRunItem])
def list_patrol_runs(
    robot_id: Optional[int] = Query(None, description="로봇으로 필터링"),
    status: Optional[str] = Query(None, description="patrol_run.status로 필터링(예: running)"),
):
    """순찰/이동 실행 기록 목록. P6PatrolPage가 페이지를 열거나(맵 전환 포함) 새로고침
    버튼을 눌렀을 때, robot_id + status=running으로 걸어 "지금 이 로봇이 실제로
    주행 중인지"를 DB 기준으로 다시 맞추는 용도로 쓴다(patrol_task.py가 goal 하나만
    허용하므로 정상적으로는 0건 또는 1건만 나온다).

    status=running으로 걸면 paused(ESP32 SOS가 잠시 밀어낸 순찰)도 같이 잡힌다 —
    이 로봇을 "점유 중"인지 판단하는 모든 호출부(P1 통합관제, P6 버튼 잠금)에게
    paused도 running과 마찬가지로 "아직 새 순찰/이동을 못 받는 상태"이기 때문이다.
    다른 status 값(completed/aborted/failed)은 그대로 정확히 일치하는 것만 본다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT pr.id, pr.robot_id, pr.recipient_id, pr.route_id, pr.schedule_id, pr.trigger,
                   pr.status, pr.waypoints_total, pr.waypoints_done, pr.started_at, pr.ended_at,
                   r.code AS robot_code
            FROM patrol_run pr JOIN robot r ON r.id = pr.robot_id
            WHERE (%(robot_id)s IS NULL OR pr.robot_id = %(robot_id)s)
                AND (
                    %(status)s IS NULL
                    OR pr.status = %(status)s
                    OR (%(status)s = 'running' AND pr.status = 'paused')
                )
            ORDER BY pr.started_at DESC
            """,
            {"robot_id": robot_id, "status": status},
        )
        rows = cur.fetchall()

    return [
        PatrolRunItem(**{k: v for k, v in row.items() if k != "robot_code"},
                      robot_connected=robot_ws.is_connected(row["robot_code"]))
        for row in rows
    ]


@runs_router.post("", response_model=PatrolRunItem, status_code=201)
async def create_patrol_run(payload: PatrolRunCreate):
    return await start_patrol_run(payload.robot_id, payload.route_id, payload.trigger)


async def dispatch_drive_to_point(
    robot_code: str, code: str, x: float, y: float, yaw: float, map_id: int,
    care_event_id: int | None = None, severity: str | None = None, reason: str | None = None,
) -> bool:
    """좌표 하나로만 로봇을 보낸다(순찰이 아니라 일회성 이동 — 도착해도 충전소로
    복귀하지 않고 그 자리에 머문다). patrol_run을 새로 기록하지 않으므로 run_id=0으로
    보낸다 — ws/robot_ws.py::_update_patrol_run은 존재하지 않는 run_id의 UPDATE를
    0행 영향으로 조용히 무시하므로 안전하다.

    P6 "이동"(아래 drive_to_point HTTP 엔드포인트)과 SOS 호출 버튼 자동 출동
    (services/mqtt_listener.py — call_button 코멘트의 "누르면... 로봇이 해당
    위치로 출동한다" 계약)이 이 로직을 공유한다. 로봇 미등록/SLAM 진행 중이면
    start_patrol_run과 동일하게 HTTPException을 던진다 — HTTP 엔드포인트는 그대로
    응답 코드가 되고, MQTT 트리거 쪽은 호출부(_handle_press)가 잡아서 경고 로그만
    남기고 넘어간다(버튼 눌림 자체를 실패시킬 이유는 없음).

    care_event_id/severity/reason은 SOS 버튼 쪽만 채워서 보낸다 — downlink_node.py가
    이 필드들이 있을 때만 record_trigger(careguard_perception/video_recorder.py)를
    발행해 이벤트 클립 녹화를 켠다. P6 "이동"은 안 넘기므로(기본값 None) 녹화가
    안 켜진다 — 사람이 수동으로 옮기는 것까지 다 녹화할 이유는 없어서다.

    severity 유무로 "이미 순찰/이동 중이면" 처리가 완전히 갈린다:
    - severity 없음(P6 "이동"): _reject_if_already_patrolling과 동일하게 409로 막는다
      — 사람이 수동으로 로봇을 가로채면 안 된다.
    - severity 있음(ESP32 SOS): 긴급이므로 막지 않는다. 대신 지금 running인 순찰을
      paused로 남기고(완전히 abort하지 않음) robot_ws에 재개 표식을 등록한 뒤,
      override=True를 실어 보내 downlink_node가 지금 실행 중인 patrol_task를 밀어내고
      이 SOS 이동을 바로 시작하게 한다. 이 SOS 이동이 끝나면(patrol_done) robot_ws가
      자동으로 그 순찰을 이어서 재개한다(robot_ws.py::_resume_patrol_run).
    """
    paused_run_id: Optional[int] = None
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        robot = cur.fetchone()
        if robot is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
        # start_patrol_run과 같은 이유(_reject_if_slam_active 문서 참고) — "이동"도
        # patrol_start를 그대로 쓰므로 SLAM 중엔 로봇이 조용히 거부한다.
        _reject_if_slam_active(cur, robot["id"])

        if not severity:
            _reject_if_already_patrolling(cur, robot["id"])
        else:
            cur.execute(
                "SELECT id FROM patrol_run WHERE robot_id = %s AND status = 'running'", (robot["id"],)
            )
            running = cur.fetchone()
            if running is not None:
                paused_run_id = running["id"]
                cur.execute("UPDATE patrol_run SET status = 'paused' WHERE id = %s", (paused_run_id,))
                conn.commit()

    if paused_run_id is not None:
        robot_ws.register_pending_resume(robot_code, paused_run_id)
        await dashboard_ws.broadcast({"type": "patrol_run_updated", "robot_code": robot_code})

    payload = {
        "type": "patrol_start",
        "run_id": 0,
        "waypoints": [{"code": code, "x": x, "y": y, "yaw": yaw}],
        "return_to_charging": False,
        "map_id": map_id,
    }
    if severity:
        payload["care_event_id"] = care_event_id
        payload["severity"] = severity
        payload["reason"] = reason
        payload["override"] = True
    return await robot_ws.send_command(robot_code, payload)


@runs_router.post("/drive-to-point", response_model=DriveToPointResponse)
async def drive_to_point(payload: DriveToPointRequest):
    """P6 "이동" — dispatch_drive_to_point() 참고."""
    connected = await dispatch_drive_to_point(
        payload.robot_code, payload.code, payload.x, payload.y, payload.yaw, payload.map_id
    )
    if not connected:
        raise HTTPException(status_code=409, detail="로봇이 연결되어 있지 않습니다")
    return DriveToPointResponse(robot_connected=connected)


async def _clear_running_patrol_run(robot_code: str) -> None:
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        robot = cur.fetchone()
        if robot is not None:
            # patrol_run.status CHECK 제약은 'running'/'paused'/'completed'/'aborted'/
            # 'failed'만 허용한다 — 'cancelled'는 없는 값이라, 이 UPDATE가 실제로
            # running 행을 하나라도 만나면 CheckViolation으로 매번 500이 났다
            # (2026-09-13 발견: R-02에 쌓여있던 고아 running 기록 4건이 이 버그
            # 때문에 "취소"를 눌러도 전혀 안 지워지고 있었다). 사용자가 직접 멈춘
            # 경우의 라벨로 이미 있는 'aborted'("중단", STATUS_LABELS 참고)가
            # 의미상으로도 맞는다. paused도 같이 정리한다 — SOS 이동 도중 사용자가
            # "취소"를 누르면 밀려나 있던 순찰까지 완전히 끝내려는 의도로 본다.
            cur.execute(
                "UPDATE patrol_run SET status = 'aborted', ended_at = NOW() "
                "WHERE robot_id = %s AND status IN ('running', 'paused')",
                (robot["id"],),
            )
            conn.commit()
            # paused였던 순찰을 취소했으니, SOS 이동이 끝나도 더 이상 자동으로
            # 재개하면 안 된다.
            robot_ws.clear_pending_resume(robot_code)


@runs_router.post("/cancel", response_model=DriveToPointResponse)
async def cancel_patrol_run(payload: CancelPatrolRunRequest):
    """P6 웨이포인트 팝오버 "주행 취소" — patrol_run.id를 몰라도(P6 "이동"은 run_id=0이라
    애초에 행이 없다) robot_code만으로 "지금 하고 있는 걸 멈춰라"를 표현한다. 로봇에
    실제로 취소 명령을 시도하는 것과, DB의 running 기록을 정리하는 것을 서로 무관하게
    둘 다 한다 — 로봇이 이미 꺼졌거나 오프라인이라 1번이 실패해도(patrol_done을
    영원히 못 받을 상황), 2번은 항상 실행돼서 그 기록이 "running"으로 영영 stuck되는
    일을 막는다(2026-09-10 실사용 중 발견: robot_id=4에 정확히 이런 고아 기록이 8건
    쌓여 있었다). asyncio.gather로 둘을 동시에 실행해야 한다 — 순차 await면 1번이
    응답 없이 지연될 때(예: 연결은 살아있는데 로봇이 멎은 경우) 2번까지 덩달아
    늦어져서 이 docstring이 약속한 "무관하게 즉시 실행"이 깨진다.
    """
    connected, _ = await asyncio.gather(
        robot_ws.send_command(payload.robot_code, {"type": "patrol_stop"}),
        _clear_running_patrol_run(payload.robot_code),
    )

    # P1 통합관제의 "순찰/이동 중" 표시가 patrol_run 실제 상태로 판단하므로(nav_status가
    # 아니라 — Nav2는 정지 후에도 계속 떠 있어 "대기 중"으로 안 바뀌던 문제, 2026-09-13),
    # 이 창에서 취소했든 다른 창에서 취소했든 전부 새로고침 없이 반영되게 브로드캐스트한다.
    await dashboard_ws.broadcast({"type": "patrol_run_updated", "robot_code": payload.robot_code})

    # drive-to-point(run_id=0)와 달리 여기서는 로봇 미연결을 에러로 만들지 않는다 —
    # "이미 오프라인이라 실제로 멈추진 못했지만, 화면/DB의 stuck 상태는 정리됐다"가
    # 이 엔드포인트의 정상 결과 중 하나다(위 docstring 참고). 호출부(PC)가
    # robot_connected로 그 차이를 사용자에게 알려주면 된다.
    return DriveToPointResponse(robot_connected=connected)


@runs_router.post("/prepare-navigation", response_model=DriveToPointResponse)
async def prepare_navigation(payload: PrepareNavigationRequest):
    """P6 페이지가 열리거나(showEvent) 다른 로봇의 맵으로 전환될 때(loadMapData) —
    실제 순찰/이동 목표 없이 Nav2만 미리 준비시킨다. 로봇이 안 켜져 있거나 아직
    안 켜져 있어도 에러가 아니다(robot_connected: false로 조용히 알려줄 뿐) — GUI가
    이걸로 팝업을 띄우지 않고 배너만 갱신한다.
    """
    connected = await robot_ws.send_command(
        payload.robot_code,
        {"type": "nav_prepare", "map_id": payload.map_id},
    )
    return DriveToPointResponse(robot_connected=connected)


@runs_router.post("/set-initial-pose", response_model=DriveToPointResponse)
async def set_initial_pose(payload: SetInitialPoseRequest):
    """P6 "위치 보정" — 지도 클릭+드래그로 사용자가 직접 지정한 로봇의 실제 위치·
    방향으로 AMCL 초기 위치를 즉석에서 다시 잡는다. nav_prepare/drive-to-point와
    달리 Nav2를 새로 켜지 않는다(이미 떠 있다고 가정) — 자동 초기 위치 추정(충전
    스테이션 좌표)이 로봇이 실제로 그 자리에 없어서 틀렸을 때 쓰는 수동 보정.
    """
    connected = await robot_ws.send_command(
        payload.robot_code,
        {
            "type": "set_initial_pose",
            "map_id": payload.map_id,
            "x": payload.x,
            "y": payload.y,
            "yaw": payload.yaw,
        },
    )
    if not connected:
        raise HTTPException(status_code=409, detail="로봇이 연결되어 있지 않습니다")
    return DriveToPointResponse(robot_connected=connected)


@runs_router.post("/shutdown-robot", response_model=DriveToPointResponse)
async def shutdown_robot(payload: ShutdownRobotRequest):
    """P6 "로봇 종료" 버튼 — 베이스 구동(teleop_launch.py)·Nav2/SLAM·rviz2·업링크·
    다운링크를 전부 내린다. 명령을 처리하는 다운링크 자신도 마지막에 스스로
    끝나므로(downlink_node.py::_shutdown_everything), 이 호출 직후 로봇 연결이
    끊기는 것이 정상이다 — 대시보드는 그걸로 "완전히 꺼졌다"고 판단한다.
    """
    connected = await robot_ws.send_command(payload.robot_code, {"type": "shutdown_all"})
    if not connected:
        raise HTTPException(status_code=409, detail="로봇이 연결되어 있지 않습니다")
    return DriveToPointResponse(robot_connected=connected)


# trigger/status 원본 코드 -> 한글 라벨. audit.py의 라벨 표(USER_ACTION_LABELS 등)와
# 같은 정책 — 새 값이 생기면 이 표에만 추가하면 된다.
TRIGGER_LABELS = {"schedule": "예약", "manual": "수동", "recheck": "재확인"}
STATUS_LABELS = {"running": "진행 중", "completed": "완료", "aborted": "중단", "failed": "실패"}

_RUN_LOG_QUERY = """
    SELECT pr.id, pr.started_at, pr.ended_at, pr.trigger, pr.status,
           pr.waypoints_total, pr.waypoints_done,
           r.code AS robot_code, rc.name AS recipient_name, rt.name AS route_name
    FROM patrol_run pr
    JOIN robot r ON r.id = pr.robot_id
    LEFT JOIN recipient rc ON rc.id = pr.recipient_id
    LEFT JOIN patrol_route rt ON rt.id = pr.route_id
    WHERE pr.started_at::date BETWEEN %(start)s AND %(end)s
    ORDER BY pr.started_at DESC
    LIMIT %(limit)s
"""

# audit-log와 같은 이유로 상한을 둔다(app/audit.py::MAX_ENTRIES 참고) — 과거 데이터를
# 통째로 내려주지 않기 위함. 필요해지면 같이 cursor 페이지네이션으로 바꾼다.
_RUN_LOG_MAX_ENTRIES = 300


@runs_router.get("/log", response_model=List[PatrolRunLogItem])
def list_patrol_run_logs(
    start: Optional[date] = Query(None, description="조회 시작일. 생략하면 오늘부터 7일 전"),
    end: Optional[date] = Query(None, description="조회 종료일. 생략하면 오늘"),
    q: Optional[str] = Query(None, description="로봇·대상자·경로·상태 텍스트 검색어"),
):
    """시스템 설정 > 경로순찰 로그 탭. patrol_run을 시각 역순으로, 사람이 읽을 이름과
    한글 라벨로 변환해 반환한다 — audit-log(GET /audit-log)와 같은 필터 관례
    (start/end/q, 기본 최근 7일)를 그대로 따른다.
    """
    end = end or date.today()
    start = start or (end - timedelta(days=7))

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(_RUN_LOG_QUERY, {"start": start, "end": end, "limit": _RUN_LOG_MAX_ENTRIES})
        rows = cur.fetchall()

    entries = [
        PatrolRunLogItem(
            id=row["id"],
            started_at=row["started_at"],
            ended_at=row["ended_at"],
            robot_code=row["robot_code"],
            recipient_name=row["recipient_name"],
            route_name=row["route_name"],
            trigger=TRIGGER_LABELS.get(row["trigger"], row["trigger"]),
            status=STATUS_LABELS.get(row["status"], row["status"]),
            waypoints_done=row["waypoints_done"],
            waypoints_total=row["waypoints_total"],
            highlight=row["status"] in ("failed", "aborted"),
        )
        for row in rows
    ]

    if q:
        keyword = q.strip().lower()
        if keyword:
            entries = [
                e
                for e in entries
                if keyword in e.robot_code.lower()
                or keyword in (e.recipient_name or "").lower()
                or keyword in (e.route_name or "").lower()
                or keyword in e.status.lower()
            ]

    return entries


@runs_router.get("/{run_id}", response_model=PatrolRunItem)
def get_patrol_run(run_id: int):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT pr.id, pr.robot_id, pr.recipient_id, pr.route_id, pr.schedule_id, pr.trigger,
                   pr.status, pr.waypoints_total, pr.waypoints_done, pr.started_at, pr.ended_at,
                   r.code AS robot_code
            FROM patrol_run pr JOIN robot r ON r.id = pr.robot_id
            WHERE pr.id = %s
            """,
            (run_id,),
        )
        row = cur.fetchone()

    if row is None:
        raise HTTPException(status_code=404, detail="순찰 기록을 찾을 수 없습니다")

    connected = robot_ws.is_connected(row["robot_code"])
    return PatrolRunItem(**{k: v for k, v in row.items() if k != "robot_code"}, robot_connected=connected)
