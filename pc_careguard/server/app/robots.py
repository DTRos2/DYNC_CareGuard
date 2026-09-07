import psycopg2
from fastapi import APIRouter, HTTPException
from typing import List

from . import db
from .robot_registry import connected_robots
from .schemas import RegisterRobotRequest, RegisterRobotResponse, RobotItem

router = APIRouter(prefix="/robots", tags=["robots"])

LIST_QUERY = """
    SELECT id, code, name, model, ros_domain_id
    FROM robot
    WHERE deregistered_at IS NULL
    ORDER BY id
"""


def _row_to_item(row) -> RobotItem:
    return RobotItem(
        id=row["id"],
        code=row["code"],
        name=row["name"],
        model=row["model"],
        ros_domain_id=row["ros_domain_id"],
        is_online=row["code"] in connected_robots,
    )


@router.get("", response_model=List[RobotItem])
def list_robots():
    """로봇 관리 탭 목록. P7SettingsPage가 화면 진입 시 이걸로 채운다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(LIST_QUERY)
        rows = cur.fetchall()
    return [_row_to_item(r) for r in rows]


@router.post("", response_model=RegisterRobotResponse, status_code=201)
def register_robot(payload: RegisterRobotRequest):
    """로봇 등록 모달의 "등록" 버튼이 호출하는 엔드포인트."""
    with db.get_connection() as conn, conn.cursor() as cur:
        try:
            cur.execute(
                """
                INSERT INTO robot (code, name, model, ros_domain_id)
                VALUES (%s, %s, %s, %s)
                RETURNING id
                """,
                (payload.code, payload.name, payload.model, payload.ros_domain_id),
            )
        except psycopg2.errors.UniqueViolation:
            conn.rollback()
            raise HTTPException(status_code=409, detail="이미 등록된 로봇 코드입니다")

        robot_id = cur.fetchone()["id"]
        conn.commit()

    return RegisterRobotResponse(
        id=robot_id,
        code=payload.code,
        name=payload.name,
        model=payload.model,
        ros_domain_id=payload.ros_domain_id,
        is_online=False,
    )
