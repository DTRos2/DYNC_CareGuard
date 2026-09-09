from typing import List

from fastapi import APIRouter

from ... import db
from ...schemas import RobotItem

router = APIRouter(prefix="/robots", tags=["robot"])


@router.get("", response_model=List[RobotItem])
def list_robots():
    """등록된 전체 로봇 목록.

    GET /maps/active는 맵이 이미 있는 로봇만 내려줘서 매핑(SLAM) 페이지의 로봇
    선택 드롭다운엔 못 쓴다 — 아직 맵이 없는 로봇이야말로 매핑을 해야 할 대상이라
    거기서도 보여야 한다. 그래서 map을 거치지 않고 robot 테이블을 직접 조회한다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT id, code, name, model, ip, created_at
            FROM robot
            WHERE deregistered_at IS NULL
            ORDER BY code
            """
        )
        rows = cur.fetchall()

    return [RobotItem(**row) for row in rows]
