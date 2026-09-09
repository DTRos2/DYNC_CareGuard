from typing import Dict, List

from fastapi import APIRouter, HTTPException

from ... import db
from ...schemas import EventItem

router = APIRouter(prefix="/events", tags=["event"])

# care_event.event_type -> 한글 라벨. audit.py의 라벨 표(USER_ACTION_LABELS 등)와 같은 정책 —
# 새 이벤트 종류가 생기면 이 표에만 값을 추가하면 된다. 표에 없는 값은 원본 코드를 그대로 보여준다.
EVENT_TYPE_LABELS: Dict[str, str] = {
    "SOS_BUTTON": "SOS 버튼",
    "SOS_SCREEN": "SOS 화면 요청",
    "SOS_VOICE": "음성 도움 요청",
    "WELLNESS_NO_RESPONSE": "안부 무응답",
    "ABSENCE": "장시간 부재",
    "MED_MISSED": "복약 미수령",
    "MED_MISSED_STREAK": "복약 연속 미수령",
    "MED_ERROR": "복약 오류",
    "INTRUDER": "미등록 인물 감지",
    "NAV_FAILED": "주행 실패",
    "BATTERY_LOW": "배터리 부족",
    "ROBOT_OFFLINE": "통신 단절",
    "ESTOP": "비상 정지(E-STOP)",
}

_EVENT_SELECT = """
    SELECT ce.event_uid, ce.severity, ce.status, ce.event_type, ce.summary,
           ce.occurred_at, ce.resolved_at, r.code AS robot_code, rc.name AS recipient_name
    FROM care_event ce
    LEFT JOIN robot r ON r.id = ce.robot_id
    LEFT JOIN recipient rc ON rc.id = ce.recipient_id
"""


def _to_item(row) -> EventItem:
    return EventItem(
        event_uid=row["event_uid"],
        severity=row["severity"],
        event_type=row["event_type"],
        event_type_label=EVENT_TYPE_LABELS.get(row["event_type"], row["event_type"]),
        summary=row["summary"],
        occurred_at=row["occurred_at"],
        resolved=row["status"] == "resolved",
        resolved_at=row["resolved_at"],
        robot_code=row["robot_code"],
        recipient_name=row["recipient_name"],
    )


@router.get("", response_model=List[EventItem])
def list_events():
    """P2 이벤트 관제 큐. 기간·검색·미처리 필터는 대상자 목록(recipient.py)과 마찬가지로
    대시보드가 이미 로컬 필터(EventFilterProxyModel)를 갖고 있어, 서버는 취소된 것만 제외한
    전체를 내려준다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(_EVENT_SELECT + " WHERE ce.status != 'cancelled' ORDER BY ce.occurred_at DESC")
        rows = cur.fetchall()
    return [_to_item(row) for row in rows]


@router.post("/{event_uid}/resolve", response_model=EventItem)
def resolve_event(event_uid: str):
    """P2 상세 모달 "RESOLVED 전환"과 일괄 완료 처리가 공유하는 진입점(둘 다 이벤트 하나씩
    호출). 담당자 배정(resolved_by)은 대시보드에 로그인 계정 id를 들고 다니는 세션이 아직
    없어(로그인 아이디 문자열만 페이지별로 따로 쥐고 있음, P2 "담당자 배정" 버튼도 추후 구현)
    NULL로 남긴다 — 계정 세션이 생기면 여기만 채우면 된다. 이미 완료된 이벤트를 다시 불러도
    에러 없이 현재 상태 그대로 반환한다(재시도 안전)."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT status FROM care_event WHERE event_uid = %s", (event_uid,))
        existing = cur.fetchone()
        if existing is None:
            raise HTTPException(status_code=404, detail="이벤트를 찾을 수 없습니다")
        if existing["status"] != "resolved":
            cur.execute(
                "UPDATE care_event SET status = 'resolved', resolved_at = NOW() WHERE event_uid = %s",
                (event_uid,),
            )
            conn.commit()
        cur.execute(_EVENT_SELECT + " WHERE ce.event_uid = %s", (event_uid,))
        updated = cur.fetchone()
    return _to_item(updated)
