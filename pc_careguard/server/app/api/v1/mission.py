from fastapi import APIRouter, HTTPException, Depends

from ...core import db
from ...schemas import MissionCreate, MissionItem
from ...ws import robot_ws
from ...core.auth import CurrentUser, require_roles

# P3 "복약 재안내" 버튼 전용. medication_scheduler 자체가 이 코드베이스에 없어서(스케줄러는
# patrol_schedule만 폴링) 여기는 스케줄러를 거치지 않고 버튼 클릭 즉시 mission을 만들어
# 바로 로봇에 다운링크를 보낸다 — patrol.py::start_patrol_run과 같은 "즉시 실행" 성격.
#
# 로봇 쪽(downlink_node.py)은 아직 이 명령 타입을 모른다(patrol_start/slam_*/cmd_vel만
# 인식) — 그래서 실제 주행·음성 안내·약통 개방은 이번 스코프 밖이고, 이 엔드포인트는
# mission 기록 + 다운링크 전송 시도까지만 담당한다.
router = APIRouter(prefix="/missions", tags=["mission"])


@router.post("", response_model=MissionItem, status_code=201)
async def create_mission(payload: MissionCreate, current_user: CurrentUser = Depends(require_roles("admin", "operator"))):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (payload.robot_code,))
        robot = cur.fetchone()
        if robot is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
        robot_id = robot["id"]

        cur.execute(
            """
            INSERT INTO mission (robot_id, kind, status, dispatched_at)
            VALUES (%s, %s, 'dispatched', NOW())
            RETURNING id, robot_id, kind, status, created_at, dispatched_at
            """,
            (robot_id, payload.kind),
        )
        row = cur.fetchone()
        conn.commit()

    connected = await robot_ws.send_command(
        payload.robot_code, {"type": "medication_start", "mission_id": row["id"]}
    )
    return MissionItem(**row, robot_connected=connected)
