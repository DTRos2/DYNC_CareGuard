import psycopg2
from fastapi import APIRouter, HTTPException
from typing import List

from ...core import db
from ...ws.robot_ws import is_connected, nav_running
from ...schemas import (
    AssignRobotRecipientRequest,
    RegisterRobotRequest,
    RegisterRobotResponse,
    RobotItem,
    RobotThresholds,
)

router = APIRouter(prefix="/robots", tags=["robots"])

# map/recipient은 서브쿼리로만 곁들이고 FROM/JOIN 대상은 robot 하나뿐이다 — 아직 맵이
# 없는(SLAM 전) 로봇도 반드시 여기 나와야 한다. P5 매핑 페이지의 로봇 선택 드롭다운이
# "맵 없는 로봇"을 고를 수 있어야 하는 게 바로 이 목록이라(GET /maps/active는 맵 있는
# 로봇만 내려줘서 그 용도로 못 쓴다), map을 JOIN/필터링에 쓰지 않도록 주의할 것.
LIST_QUERY = """
    SELECT r.id, r.code, r.name, r.model, r.ros_domain_id, r.ip,
        (SELECT rec.name FROM recipient rec
         WHERE rec.robot_id = r.id AND rec.deregistered_at IS NULL
         LIMIT 1) AS recipient_name,
        (SELECT m.name FROM map m
         WHERE m.robot_id = r.id AND m.is_active = TRUE
         ORDER BY m.created_at DESC LIMIT 1) AS map_name
    FROM robot r
    WHERE r.deregistered_at IS NULL
    ORDER BY r.id
"""


def _map_recipient_label(map_name, recipient_name) -> str:
    if map_name and recipient_name:
        return f"{map_name} · {recipient_name}"
    if map_name:
        return f"{map_name} · 미배정"
    if recipient_name:
        return f"- · {recipient_name}"
    return "-"


def _row_to_item(row) -> RobotItem:
    return RobotItem(
        id=row["id"],
        code=row["code"],
        name=row["name"],
        model=row["model"],
        ros_domain_id=row["ros_domain_id"],
        ip=row["ip"],
        map_recipient=_map_recipient_label(row["map_name"], row["recipient_name"]),
        is_online=is_connected(row["code"]),
        is_nav_running=nav_running(row["code"]),
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
                INSERT INTO robot (code, name, model, ros_domain_id, ip)
                VALUES (%s, %s, %s, %s, %s)
                RETURNING id
                """,
                (payload.code, payload.name, payload.model, payload.ros_domain_id, payload.ip),
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
        ip=payload.ip,
        map_recipient="-",
        is_online=is_connected(payload.code),
    )


@router.put("/{robot_id}/recipient", status_code=204)
def assign_robot_recipient(robot_id: int, payload: AssignRobotRecipientRequest):
    """로봇 등록·수정 모달의 "담당 대상자" 선택을 저장한다.

    로봇 한 대엔 대상자 한 명만 배정되므로(RobotDialog 주석 참고), 이 로봇을 담당하던
    다른 대상자가 있으면 먼저 배정을 해제하고 나서 recipient_id가 가리키는 대상자를
    이 로봇 담당으로 설정한다. recipient_id가 None이면 배정 해제만 하고 끝낸다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM robot WHERE id = %s AND deregistered_at IS NULL", (robot_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")

        cur.execute(
            """
            UPDATE recipient SET robot_id = NULL
            WHERE robot_id = %s AND deregistered_at IS NULL AND id != COALESCE(%s, -1)
            """,
            (robot_id, payload.recipient_id),
        )

        if payload.recipient_id is not None:
            cur.execute(
                "UPDATE recipient SET robot_id = %s WHERE id = %s AND deregistered_at IS NULL",
                (robot_id, payload.recipient_id),
            )
            if cur.rowcount == 0:
                conn.rollback()
                raise HTTPException(status_code=404, detail="대상자를 찾을 수 없습니다")

        conn.commit()


THRESHOLD_COLUMNS = (
    "battery_attention_pct",
    "battery_return_pct",
    "voice_wait_sec",
    "reconnect_retry_sec",
    "med_missed_wait_min",
    "med_reannounce_count",
)


def _upsert_threshold(cur, robot_id: int, t: RobotThresholds) -> None:
    cur.execute(
        """
        INSERT INTO robot_threshold (robot_id, battery_attention_pct, battery_return_pct,
            voice_wait_sec, reconnect_retry_sec, med_missed_wait_min, med_reannounce_count, updated_at)
        VALUES (%s, %s, %s, %s, %s, %s, %s, NOW())
        ON CONFLICT (robot_id) DO UPDATE SET
            battery_attention_pct = EXCLUDED.battery_attention_pct,
            battery_return_pct = EXCLUDED.battery_return_pct,
            voice_wait_sec = EXCLUDED.voice_wait_sec,
            reconnect_retry_sec = EXCLUDED.reconnect_retry_sec,
            med_missed_wait_min = EXCLUDED.med_missed_wait_min,
            med_reannounce_count = EXCLUDED.med_reannounce_count,
            updated_at = NOW()
        """,
        (
            robot_id,
            t.battery_attention_pct,
            t.battery_return_pct,
            t.voice_wait_sec,
            t.reconnect_retry_sec,
            t.med_missed_wait_min,
            t.med_reannounce_count,
        ),
    )


@router.get("/{robot_id}/thresholds", response_model=RobotThresholds)
def get_robot_thresholds(robot_id: int):
    """시스템 설정 > 임계값·알림 탭 — 로봇 선택 콤보에서 고른 로봇의 저장된 값을 채운다.
    robot_threshold에 행이 없으면(한 번도 저장 안 한 로봇) 기본값을 그대로 돌려준다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM robot WHERE id = %s AND deregistered_at IS NULL", (robot_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")

        cur.execute("SELECT * FROM robot_threshold WHERE robot_id = %s", (robot_id,))
        row = cur.fetchone()

    if row is None:
        return RobotThresholds()
    return RobotThresholds(**{col: row[col] for col in THRESHOLD_COLUMNS})


@router.put("/{robot_id}/thresholds", status_code=204)
def set_robot_thresholds(robot_id: int, payload: RobotThresholds):
    """"이 로봇에 저장" 버튼 — 화면에 보이는 값을 이 로봇 하나에만 저장한다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM robot WHERE id = %s AND deregistered_at IS NULL", (robot_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
        _upsert_threshold(cur, robot_id, payload)
        conn.commit()


@router.put("/thresholds/apply-all", status_code=204)
def apply_thresholds_to_all_robots(payload: RobotThresholds):
    """"전체 로봇에 적용" 버튼 — 화면에 보이는 값을 그 시점에 등록된 로봇 전부에 복사해
    넣는다(일괄 배포). 이후 로봇마다 값이 갈라져도 되므로, 이건 실시간으로 동기화되는
    공유 설정이 아니라 그 순간의 스냅샷을 각 로봇 행에 따로 저장하는 동작이다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE deregistered_at IS NULL")
        robot_ids = [row["id"] for row in cur.fetchall()]
        for robot_id in robot_ids:
            _upsert_threshold(cur, robot_id, payload)
        conn.commit()


@router.delete("/{robot_id}", status_code=204)
def deregister_robot(robot_id: int):
    """로봇 등록 해제 — 계정 삭제와 동일하게 실제 행을 지우지 않고 deregistered_at만 채운다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "UPDATE robot SET deregistered_at = NOW() WHERE id = %s AND deregistered_at IS NULL",
            (robot_id,),
        )
        if cur.rowcount == 0:
            conn.rollback()
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
        conn.commit()
