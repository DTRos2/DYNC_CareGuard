from typing import List

from fastapi import APIRouter, HTTPException

from ... import db
from ...schemas import (
    GuardianItem,
    MedicationScheduleItem,
    RecipientCreate,
    RecipientDetailItem,
    RecipientItem,
)

router = APIRouter(prefix="/recipients", tags=["recipient"])

_RECIPIENT_SELECT = """
    SELECT rc.id, rc.code, rc.name, rc.robot_id, r.code AS robot_code,
           rc.gender, rc.birth_date, rc.home_address, rc.diseases, rc.note, rc.created_at
    FROM recipient rc
    LEFT JOIN robot r ON r.id = rc.robot_id
"""


def _next_code(cur) -> str:
    """C-001부터 순번 채번. 대시보드가 예전엔 recipients_.size()+1로 추정했는데,
    등록 해제로 목록 길이가 줄면 코드가 겹칠 수 있어 서버가 유일한 원본을 갖는다."""
    cur.execute("SELECT code FROM recipient ORDER BY id DESC LIMIT 1")
    row = cur.fetchone()
    if row is None:
        return "C-001"
    last_num = int(row["code"].split("-")[1])
    return f"C-{last_num + 1:03d}"


@router.get("", response_model=List[RecipientItem])
def list_recipients():
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(_RECIPIENT_SELECT + " WHERE rc.deregistered_at IS NULL ORDER BY rc.id")
        rows = cur.fetchall()
    return [RecipientItem(**row) for row in rows]


@router.get("/{recipient_id}", response_model=RecipientDetailItem)
def get_recipient(recipient_id: int):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(_RECIPIENT_SELECT + " WHERE rc.id = %s AND rc.deregistered_at IS NULL", (recipient_id,))
        row = cur.fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="대상자를 찾을 수 없습니다")

        cur.execute(
            """
            SELECT id, name, phone, relation, priority, notify_enabled
            FROM guardian
            WHERE recipient_id = %s AND deregistered_at IS NULL
            ORDER BY priority
            """,
            (recipient_id,),
        )
        guardians = cur.fetchall()

        cur.execute(
            """
            SELECT id, slot_no, drug_name, dose_time, days_of_week
            FROM medication_schedule
            WHERE recipient_id = %s AND deregistered_at IS NULL
            ORDER BY slot_no, dose_time
            """,
            (recipient_id,),
        )
        schedules = cur.fetchall()

    return RecipientDetailItem(
        **row,
        guardians=[GuardianItem(**g) for g in guardians],
        schedules=[MedicationScheduleItem(**s) for s in schedules],
    )


@router.post("", response_model=RecipientDetailItem, status_code=201)
def create_recipient(payload: RecipientCreate):
    """P3 "대상자 등록" 모달. recipient + guardian(1순위 이상) + medication_schedule을
    한 트랜잭션으로 저장한다. 긴급 고정 행(119·112)은 화면 전용 표시라 guardians에
    안 실려 온다는 전제(대시보드 쪽에서 이미 걸러 보냄) — 여기서는 온 것만 그대로 넣는다.
    """
    for s in payload.schedules:
        if not (1 <= s.slot_no <= 3):
            raise HTTPException(status_code=422, detail="복약 스케줄 칸 번호는 1~3만 가능합니다")

    with db.get_connection() as conn, conn.cursor() as cur:
        robot_id = None
        if payload.robot_code:
            cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (payload.robot_code,))
            robot = cur.fetchone()
            if robot is None:
                raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
            robot_id = robot["id"]

        code = _next_code(cur)

        cur.execute(
            """
            INSERT INTO recipient (code, robot_id, name, gender, birth_date, home_address, diseases, note)
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
            RETURNING id
            """,
            (
                code,
                robot_id,
                payload.name,
                payload.gender,
                payload.birth_date,
                payload.home_address,
                payload.diseases,
                payload.note,
            ),
        )
        recipient_id = cur.fetchone()["id"]

        for g in payload.guardians:
            cur.execute(
                """
                INSERT INTO guardian (recipient_id, name, phone, relation, priority, notify_enabled)
                VALUES (%s, %s, %s, %s, %s, %s)
                """,
                (recipient_id, g.name, g.phone, g.relation, g.priority, g.notify_enabled),
            )

        for s in payload.schedules:
            cur.execute(
                """
                INSERT INTO medication_schedule (recipient_id, slot_no, drug_name, dose_time, days_of_week)
                VALUES (%s, %s, %s, %s, %s)
                """,
                (recipient_id, s.slot_no, s.drug_name, s.dose_time, s.days_of_week),
            )

        conn.commit()

    return get_recipient(recipient_id)


@router.put("/{recipient_id}", response_model=RecipientDetailItem)
def update_recipient(recipient_id: int, payload: RecipientCreate):
    """P3 대상자 상세 모달의 "저장". 기본정보를 갱신하고, 보호자·복약 스케줄은 다이얼로그가
    들고 있는 전체 목록으로 통째로 교체한다(기존 활성 행을 등록 해제 후 다시 심음) — 다이얼로그가
    이미 전체 상태를 관리하고 있어 부분 diff보다 훨씬 단순하고, POST와 같은 payload 모양을
    유지해 두 경로가 프런트의 같은 변환 로직을 그대로 재사용할 수 있다. 로그성 테이블
    (guardian_message 등)은 guardian_id에 FK 제약이 없고 소프트 삭제라 옛 행이 그대로 남으므로
    과거 기록 조회에는 영향 없다.
    """
    for s in payload.schedules:
        if not (1 <= s.slot_no <= 3):
            raise HTTPException(status_code=422, detail="복약 스케줄 칸 번호는 1~3만 가능합니다")

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM recipient WHERE id = %s AND deregistered_at IS NULL", (recipient_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=404, detail="대상자를 찾을 수 없습니다")

        robot_id = None
        if payload.robot_code:
            cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (payload.robot_code,))
            robot = cur.fetchone()
            if robot is None:
                raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
            robot_id = robot["id"]

        cur.execute(
            """
            UPDATE recipient
            SET robot_id = %s, name = %s, gender = %s, birth_date = %s,
                home_address = %s, diseases = %s, note = %s
            WHERE id = %s
            """,
            (
                robot_id,
                payload.name,
                payload.gender,
                payload.birth_date,
                payload.home_address,
                payload.diseases,
                payload.note,
                recipient_id,
            ),
        )

        cur.execute(
            "UPDATE guardian SET deregistered_at = NOW() WHERE recipient_id = %s AND deregistered_at IS NULL",
            (recipient_id,),
        )
        for g in payload.guardians:
            cur.execute(
                """
                INSERT INTO guardian (recipient_id, name, phone, relation, priority, notify_enabled)
                VALUES (%s, %s, %s, %s, %s, %s)
                """,
                (recipient_id, g.name, g.phone, g.relation, g.priority, g.notify_enabled),
            )

        cur.execute(
            "UPDATE medication_schedule SET deregistered_at = NOW() "
            "WHERE recipient_id = %s AND deregistered_at IS NULL",
            (recipient_id,),
        )
        for s in payload.schedules:
            cur.execute(
                """
                INSERT INTO medication_schedule (recipient_id, slot_no, drug_name, dose_time, days_of_week)
                VALUES (%s, %s, %s, %s, %s)
                """,
                (recipient_id, s.slot_no, s.drug_name, s.dose_time, s.days_of_week),
            )

        conn.commit()

    return get_recipient(recipient_id)
