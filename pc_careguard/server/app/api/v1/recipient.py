from datetime import date, timedelta
from typing import List, Optional

import anyio
from fastapi import APIRouter, HTTPException

from ...core import db
from ...schemas import (
    GuardianItem,
    MedicationScheduleItem,
    RecipientCreate,
    RecipientDetailItem,
    RecipientItem,
)
from ...ws import dashboard_ws

router = APIRouter(prefix="/recipients", tags=["recipient"])

_RECIPIENT_SELECT = """
    SELECT rc.id, rc.code, rc.name, rc.robot_id, r.code AS robot_code,
           rc.gender, rc.birth_date, rc.home_address, rc.diseases, rc.note,
           rc.operator_id, op.display_name AS operator_name, rc.created_at
    FROM recipient rc
    LEFT JOIN robot r ON r.id = rc.robot_id
    LEFT JOIN careguard_user op ON op.id = rc.operator_id AND op.deleted_at IS NULL
"""


def _resolve_operator_id(cur, operator_id):
    """대상자 상세/등록 모달의 "관리 담당자" 콤보값 검증. accounts.py의 담당 배정과 같은
    제약(role.code='operator', 삭제되지 않은 계정)을 여기서도 강제해, 이 경로로도 존재하지
    않거나 역할이 다른 계정을 몰래 담당자로 넣을 수 없게 한다."""
    if operator_id is None:
        return None
    cur.execute(
        """
        SELECT u.id FROM careguard_user u
        JOIN role r ON r.id = u.role_id
        WHERE u.id = %s AND r.code = 'operator' AND u.deleted_at IS NULL
        """,
        (operator_id,),
    )
    if cur.fetchone() is None:
        raise HTTPException(status_code=404, detail="관제담당자를 찾을 수 없습니다")
    return operator_id


# 요일 비트마스크: 월=1 화=2 수=4 목=8 금=16 토=32 일=64 — 대시보드
# bitmaskFromDaysLabel()/daysLabelFromBitmask()와 동일 규칙. Python date.weekday()는
# 월=0..일=6이라 1 << weekday()가 그대로 이 비트값과 맞아떨어진다.
def _weekday_bit(d: date) -> int:
    return 1 << d.weekday()


def _risk_by_recipient(cur, recipient_ids: Optional[List[int]] = None) -> dict:
    """대상자별 "현재 위험도" — 열려있는(open/acknowledged) care_event 중 가장 심각한 것 하나.
    severity='info'는 대시보드 Tokens::Level에 대응 값이 없어 위험도로 치지 않는다(media.py의
    SEVERITY_TAGS와 같은 분류 — emergency/attention만 태그가 있고 info는 순찰과 같이 묶임).
    recipient_ids를 주면 그 목록만, None이면 전체 대상자를 계산한다(목록/상세 화면이 공유).
    """
    query = """
        SELECT recipient_id, severity, summary
        FROM care_event
        WHERE recipient_id IS NOT NULL AND status IN ('open', 'acknowledged')
          AND severity IN ('attention', 'emergency')
    """
    params: list = []
    if recipient_ids is not None:
        query += " AND recipient_id = ANY(%s)"
        params.append(recipient_ids)
    query += """
        ORDER BY recipient_id,
                 CASE severity WHEN 'emergency' THEN 2 WHEN 'attention' THEN 1 ELSE 0 END DESC,
                 occurred_at DESC
    """
    cur.execute(query, params)
    result = {}
    for row in cur.fetchall():
        # recipient_id별로 이미 심각도·최신순 정렬돼 있으니 처음 나오는 행이 곧 "가장 심각한 것".
        result.setdefault(row["recipient_id"], row)
    return result


def _today_meds_by_recipient(cur, recipient_ids: Optional[List[int]] = None) -> dict:
    """대상자별 오늘 슬롯 상태(카드의 복약 배지용). 오늘 요일에 해당하는 스케줄 슬롯만 담고,
    슬롯 수가 대상자마다 달라도(꼭 3개일 필요 없음) 있는 만큼만 계산한다.
    """
    schedule_query = "SELECT recipient_id, slot_no, days_of_week FROM medication_schedule WHERE deregistered_at IS NULL"
    schedule_params: list = []
    if recipient_ids is not None:
        schedule_query += " AND recipient_id = ANY(%s)"
        schedule_params.append(recipient_ids)
    cur.execute(schedule_query, schedule_params)
    schedules = cur.fetchall()

    log_query = "SELECT recipient_id, slot_no, result FROM medication_log WHERE created_at::date = CURRENT_DATE"
    log_params: list = []
    if recipient_ids is not None:
        log_query += " AND recipient_id = ANY(%s)"
        log_params.append(recipient_ids)
    cur.execute(log_query, log_params)
    logs = cur.fetchall()
    log_lookup = {(l["recipient_id"], l["slot_no"]): l["result"] for l in logs}

    today_bit = _weekday_bit(date.today())
    result: dict = {}
    for s in schedules:
        if not (s["days_of_week"] & today_bit):
            continue
        outcome = log_lookup.get((s["recipient_id"], s["slot_no"]))
        if outcome == "delivered":
            state = "done"
        elif outcome in ("missed", "not_found", "error", "skipped"):
            state = "missed"
        else:
            state = "pending"  # 아직 확인 시도(medication_log) 자체가 없음
        result.setdefault(s["recipient_id"], []).append({"slot_no": s["slot_no"], "state": state})
    return result


def _medication_history(cur, recipient_id: int, schedules) -> list:
    """최근 28일 하루 단위 복약 이력(상세 모달 히트맵용). 그날 요일에 스케줄된 슬롯을 전부
    수령했으면 1.0, 하나라도 못 받았으면 0.0, 그날 스케줄 자체가 없으면 -1.0(빈칸) — 부분
    수령 구간은 두지 않는다(요청에 따른 정책). schedules는 get_recipient가 이미 조회해둔
    현재 활성 스케줄 목록을 그대로 받는다(과거 시점의 스케줄 변경 이력까지는 추적하지 않음).
    """
    cur.execute(
        "SELECT slot_no, result, created_at FROM medication_log "
        "WHERE recipient_id = %s AND created_at >= NOW() - INTERVAL '28 days'",
        (recipient_id,),
    )
    logs = cur.fetchall()

    today = date.today()
    history = []
    for offset in range(27, -1, -1):
        day = today - timedelta(days=offset)
        scheduled_slots = {s["slot_no"] for s in schedules if s["days_of_week"] & _weekday_bit(day)}
        if not scheduled_slots:
            history.append({"date": day, "adherence": -1.0})
            continue
        delivered_slots = {
            l["slot_no"] for l in logs if l["created_at"].date() == day and l["result"] == "delivered"
        }
        adherence = 1.0 if scheduled_slots.issubset(delivered_slots) else 0.0
        history.append({"date": day, "adherence": adherence})
    return history


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
        ids = [row["id"] for row in rows]
        risk = _risk_by_recipient(cur, ids)
        meds_today = _today_meds_by_recipient(cur, ids)

    items = []
    for row in rows:
        event = risk.get(row["id"])
        items.append(
            RecipientItem(
                **row,
                level=event["severity"] if event else "normal",
                status_summary=event["summary"] if event else "등록됨 · 이상 없음",
                meds_today=meds_today.get(row["id"], []),
            )
        )
    return items


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

        risk = _risk_by_recipient(cur, [recipient_id])
        meds_today = _today_meds_by_recipient(cur, [recipient_id])
        history = _medication_history(cur, recipient_id, schedules)

    event = risk.get(recipient_id)
    return RecipientDetailItem(
        **row,
        level=event["severity"] if event else "normal",
        status_summary=event["summary"] if event else "등록됨 · 이상 없음",
        meds_today=meds_today.get(recipient_id, []),
        guardians=[GuardianItem(**g) for g in guardians],
        schedules=[MedicationScheduleItem(**s) for s in schedules],
        history=history,
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

        operator_id = _resolve_operator_id(cur, payload.operator_id)
        code = _next_code(cur)

        cur.execute(
            """
            INSERT INTO recipient (code, robot_id, name, gender, birth_date, home_address, diseases, note, operator_id)
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
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
                operator_id,
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

    # 다른 대시보드가 이미 P3를 열어두고 있을 수 있다 — 재시작 없이도 새 카드가
    # 보이도록 push한다(update_recipient의 patrol_schedules_changed와 같은 이유).
    anyio.from_thread.run(dashboard_ws.broadcast, {"type": "recipient_changed", "recipient_id": recipient_id})

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

        operator_id = _resolve_operator_id(cur, payload.operator_id)

        cur.execute(
            """
            UPDATE recipient
            SET robot_id = %s, name = %s, gender = %s, birth_date = %s,
                home_address = %s, diseases = %s, note = %s, operator_id = %s
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
                operator_id,
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

        # 슬롯 번호(slot_no) 기준으로 있는 건 그 자리에서 갱신하고, 새 슬롯만 새로 심는다
        # (예전엔 전부 등록 해제 후 통째로 다시 만들어서, 주소만 고쳐도 상관없던 기존
        # 슬롯들까지 매번 새 id를 받았다). id를 유지해야 P6 주행 스케줄의
        # patrol_schedule.medication_schedule_id 참조가 안 끊기고, 아래에서 시각이 바뀌면
        # 그 참조를 쓰는 주행 스케줄에도 바로 반영된다 — "대상자 복약 스케줄을 바꿔도
        # 주행 스케줄엔 반영이 안 된다" 리포트의 원인이었다.
        cur.execute(
            "SELECT id, slot_no FROM medication_schedule WHERE recipient_id = %s AND deregistered_at IS NULL",
            (recipient_id,),
        )
        existing_by_slot = {row["slot_no"]: row["id"] for row in cur.fetchall()}
        incoming_slots = {s.slot_no for s in payload.schedules}
        # 갱신·매칭 해제로 영향받은 patrol_route.id들 — 커밋 뒤 이 경로를 열어둔 대시보드에
        # "다시 조회하라"고 push한다(대시보드를 껐다 켜야만 반영되던 문제).
        touched_route_ids: set = set()

        for s in payload.schedules:
            existing_id = existing_by_slot.get(s.slot_no)
            if existing_id is not None:
                cur.execute(
                    """
                    UPDATE medication_schedule
                    SET drug_name = %s, dose_time = %s, days_of_week = %s
                    WHERE id = %s
                    """,
                    (s.drug_name, s.dose_time, s.days_of_week, existing_id),
                )
                cur.execute(
                    """
                    UPDATE patrol_schedule
                    SET run_time = %s, days_of_week = %s
                    WHERE medication_schedule_id = %s AND deregistered_at IS NULL
                    RETURNING route_id
                    """,
                    (s.dose_time, s.days_of_week, existing_id),
                )
                touched_route_ids.update(row["route_id"] for row in cur.fetchall())
            else:
                cur.execute(
                    """
                    INSERT INTO medication_schedule (recipient_id, slot_no, drug_name, dose_time, days_of_week)
                    VALUES (%s, %s, %s, %s, %s)
                    """,
                    (recipient_id, s.slot_no, s.drug_name, s.dose_time, s.days_of_week),
                )

        removed_ids = [mid for slot, mid in existing_by_slot.items() if slot not in incoming_slots]
        if removed_ids:
            cur.execute(
                "UPDATE medication_schedule SET deregistered_at = NOW() WHERE id = ANY(%s)",
                (removed_ids,),
            )
            # 이 슬롯을 매칭으로 쓰던 주행 스케줄은 참조만 끊는다("선택 안 함" 상태로
            # 돌아감) — 스케줄(시각·요일) 자체는 그대로 두고 매칭만 해제한다.
            cur.execute(
                """
                UPDATE patrol_schedule SET medication_schedule_id = NULL
                WHERE medication_schedule_id = ANY(%s) RETURNING route_id
                """,
                (removed_ids,),
            )
            touched_route_ids.update(row["route_id"] for row in cur.fetchall())

        conn.commit()

    # P6 페이지가 이 경로를 이미 불러와 보여주고 있을 수 있다 — 대시보드를 껐다 켜지
    # 않아도 최신 시각·요일을 GET /patrol-schedules로 다시 받아가도록 push한다(P4의
    # video_event_created와 같은 이유·같은 패턴). 동기(def) 핸들러라 robot_ws.py처럼
    # 바로 await 못 하고 anyio.from_thread.run으로 이벤트 루프 스레드에 맡긴다.
    for route_id in touched_route_ids:
        anyio.from_thread.run(dashboard_ws.broadcast, {"type": "patrol_schedules_changed", "route_id": route_id})

    # 이름·담당 로봇·보호자 등 카드 자체가 바뀌었을 수도 있다 — 위 patrol_schedules_changed와
    # 별개로 항상 push한다(스케줄 변경이 없어도 반영되어야 함).
    anyio.from_thread.run(dashboard_ws.broadcast, {"type": "recipient_changed", "recipient_id": recipient_id})

    return get_recipient(recipient_id)


@router.post("/{recipient_id}/deactivate", status_code=204)
def deactivate_recipient(recipient_id: int):
    """P3 상세 모달의 "대상자 비활성화". 하드 삭제 대신 recipient.deregistered_at을 찍어
    목록(GET /recipients)·조회(GET /recipients/{id})에서만 제외한다 — guardian·
    medication_schedule과 같은 소프트 삭제 정책. robot_id·operator_id는 그대로 남겨두는데,
    두 FK 모두 조회 쪽이 이미 deregistered_at IS NULL로 걸러서(accounts.py의 CHARGE_QUERY
    포함) 이 대상자가 다시 로봇 담당·관제담당자 요약에 나타나지 않는다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "UPDATE recipient SET deregistered_at = NOW() WHERE id = %s AND deregistered_at IS NULL",
            (recipient_id,),
        )
        if cur.rowcount == 0:
            conn.rollback()
            raise HTTPException(status_code=404, detail="대상자를 찾을 수 없습니다")
        conn.commit()

    anyio.from_thread.run(dashboard_ws.broadcast, {"type": "recipient_changed", "recipient_id": recipient_id})
