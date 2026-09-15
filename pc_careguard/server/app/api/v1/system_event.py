from datetime import date, timedelta
from typing import Dict, List, Optional

from fastapi import APIRouter, Query

from ...core import db
from ...schemas import SystemEventLogItem

router = APIRouter(prefix="/system-events", tags=["system-event"])

# audit-log와 같은 이유로 상한을 둔다(app/audit.py::MAX_ENTRIES 참고).
MAX_ENTRIES = 300

# system_event.event_type -> 한글 라벨. event.py::EVENT_TYPE_LABELS와 같은 정책 —
# 새 경보 종류가 생기면 이 표에만 값을 추가하면 된다. 지금은 배터리 임계값
# (robot_ws.py::_handle_battery_state)만 이 테이블에 쓰고 있다.
EVENT_TYPE_LABELS: Dict[str, str] = {
    "battery_attention": "배터리 주의",
    "battery_return_needed": "충전소 복귀 필요",
    "battery_recovered": "배터리 정상 회복",
}

_QUERY = """
    SELECT se.created_at, se.event_type, se.severity, se.message, r.code AS robot_code
    FROM system_event se
    LEFT JOIN robot r ON r.id = se.robot_id
    WHERE se.created_at::date BETWEEN %(start)s AND %(end)s
    ORDER BY se.created_at DESC
    LIMIT %(limit)s
"""


@router.get("", response_model=List[SystemEventLogItem])
def list_system_events(
    start: Optional[date] = Query(None, description="조회 시작일. 생략하면 오늘부터 7일 전"),
    end: Optional[date] = Query(None, description="조회 종료일. 생략하면 오늘"),
    q: Optional[str] = Query(None, description="로봇·종류·메시지 텍스트 검색어"),
):
    """시스템 설정 > 임계값·알림 로그 탭. robot_ws.py의 _handle_battery_state가
    임계값을 넘는 순간마다 남기는 system_event를 시각 역순으로 반환한다.
    audit-log/patrol-runs와 같은 필터 관례(start/end/q, 기본 최근 7일)를 따른다.
    """
    end = end or date.today()
    start = start or (end - timedelta(days=7))

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(_QUERY, {"start": start, "end": end, "limit": MAX_ENTRIES})
        rows = cur.fetchall()

    entries = [
        SystemEventLogItem(
            occurred_at=row["created_at"],
            robot_code=row["robot_code"],
            event_type=EVENT_TYPE_LABELS.get(row["event_type"], row["event_type"]),
            severity=row["severity"],
            message=row["message"],
            highlight=row["severity"] == "error",
        )
        for row in rows
    ]

    if q:
        keyword = q.strip().lower()
        if keyword:
            entries = [
                e
                for e in entries
                if keyword in (e.robot_code or "").lower()
                or keyword in e.event_type.lower()
                or keyword in e.message.lower()
            ]

    return entries
