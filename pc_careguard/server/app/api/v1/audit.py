from datetime import date, timedelta
from typing import Any, Dict, List, Optional, Tuple

from fastapi import APIRouter, Query

from ...core import db
from ...schemas import AuditLogEntry

router = APIRouter(prefix="/audit-log", tags=["audit-log"])

# 감사 로그 한 줄 조회 시 과거 데이터를 통째로 내려주지 않기 위한 상한.
# 필요해지면 나중에 cursor 기반 페이지네이션으로 바꾸면 된다 — 지금은 응답 스키마
# (AuditLogEntry 리스트)를 그대로 유지한 채 이 상수만 조절해서 대응 가능.
MAX_ENTRIES = 300

# action_type -> (한글 라벨, 책임 추적 강조 여부).
# 새 조치 종류가 생기면 이 표에만 값을 추가하면 되고, 코드의 나머지 부분은 손댈 필요 없다.
# 표에 없는 action_type은 코드 문자열을 그대로 라벨로 쓰고 강조하지 않는다(_label_for 참고).
USER_ACTION_LABELS: Dict[str, Tuple[str, bool]] = {
    "login": ("로그인", False),
    "login_failed": ("로그인 실패", False),
    "login_locked": ("로그인 잠금", False),
    "logout": ("로그아웃", False),
    "estop": ("E-STOP", True),
    "estop_release": ("E-STOP 해제", False),
    "robot_command": ("로봇 명령", False),
    "robot_create": ("로봇 등록", False),
    "robot_update": ("로봇 수정", False),
    "robot_deregister": ("로봇 등록 해제", False),
    "view_live": ("실시간 열람", True),
    "view_video": ("영상 열람", True),
    "download_video": ("영상 다운로드", True),
    "recipient_create": ("대상자 등록", False),
    "recipient_update": ("대상자 수정", False),
    "recipient_delete": ("대상자 삭제", False),
    "guardian_create": ("보호자 등록", False),
    "guardian_update": ("보호자 수정", False),
    "guardian_delete": ("보호자 삭제", False),
    "policy_update": ("정책 수정", False),
    "medication_schedule_create": ("복약 일정 등록", False),
    "medication_schedule_update": ("복약 일정 수정", False),
    "medication_schedule_delete": ("복약 일정 삭제", False),
    "waypoint_create": ("웨이포인트 등록", False),
    "waypoint_update": ("웨이포인트 수정", False),
    "waypoint_delete": ("웨이포인트 삭제", False),
    "patrol_route_create": ("순찰 경로 등록", False),
    "patrol_route_update": ("순찰 경로 수정", False),
    "patrol_route_delete": ("순찰 경로 삭제", False),
    "patrol_schedule_create": ("순찰 일정 등록", False),
    "patrol_schedule_delete": ("순찰 일정 삭제", False),
    "call_button_register": ("호출 버튼 등록", False),
    "call_button_update": ("호출 버튼 수정", False),
    "call_button_delete": ("호출 버튼 삭제", False),
    "map_save": ("맵 저장", False),
    "map_activate": ("맵 배포", False),
    "map_delete": ("맵 삭제", False),
    "guardian_message_create": ("보호자 문자", False),
    "guardian_message_cancel": ("보호자 문자 취소", False),
    "account_create": ("계정 등록", False),
    "account_update": ("계정 수정", False),
    "account_delete": ("계정 삭제", False),
    "account_activate": ("계정 활성화", False),
    "account_deactivate": ("계정 비활성화", False),
    "password_change": ("비밀번호 변경", False),
    "role_update": ("역할 변경", False),
}

# care_action.action_type -> (한글 라벨, 강조 여부). user_action_log와 별개 표인 이유는
# 같은 코드값(view_video)이라도 두 테이블에서 의미가 달라서다(전자는 이벤트 대응 중 열람).
CARE_ACTION_LABELS: Dict[str, Tuple[str, bool]] = {
    "acknowledge": ("이벤트 확인", False),
    "view_video": ("영상 열람", True),
    "call_recipient": ("대상자 전화", False),
    "call_guardian": ("보호자 연락", False),
    "notify_guardian": ("보호자 알림", False),
    "report_119": ("119 신고", True),
    "report_112": ("112 신고", True),
    "dispatch_robot": ("로봇 출동", False),
    "med_reannounce": ("복약 재안내", False),
    "request_visit": ("방문 요청", False),
    "escalate": ("단계 격상", False),
    "resolve": ("이벤트 완료", True),
    "cancel": ("이벤트 취소", False),
}


def _label_for(labels: Dict[str, Tuple[str, bool]], action_type: str) -> Tuple[str, bool]:
    """표에 없는 action_type이 들어와도 죽지 않고 원본 문자열을 라벨로 보여준다."""
    return labels.get(action_type, (action_type, False))


def _account_name(row: Dict[str, Any]) -> Optional[str]:
    """표시 이름이 없으면 로그인 ID로, 계정 자체가 없으면(시스템 조치) None으로."""
    return row["display_name"] or row["login_id"]


def _user_action_detail(row: Dict[str, Any]) -> str:
    """user_action_log 한 줄의 "대상 · 상세" 텍스트. payload.reason이 있으면 최우선으로 보여준다
    (영상 열람 사유 등 — media.py가 이 규칙으로 기록한다)."""
    payload = row["payload"] or {}
    reason = payload.get("reason")
    if reason:
        return f"사유: {reason}"
    if row["target_table"] and row["target_id"] is not None:
        return f"{row['target_table']} #{row['target_id']}"
    return ""


def _fetch_user_action_entries(start: date, end: date) -> List[Dict[str, Any]]:
    query = """
        SELECT ual.occurred_at, ual.action_type, ual.target_table, ual.target_id, ual.payload,
               u.login_id, u.display_name
        FROM user_action_log ual
        LEFT JOIN careguard_user u ON u.id = ual.user_id
        WHERE ual.occurred_at::date BETWEEN %s AND %s
        ORDER BY ual.occurred_at DESC
        LIMIT %s
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(query, (start, end, MAX_ENTRIES))
        rows = cur.fetchall()

    entries = []
    for row in rows:
        label, highlight = _label_for(USER_ACTION_LABELS, row["action_type"])
        entries.append(
            {
                "occurred_at": row["occurred_at"],
                "account": _account_name(row),
                "action": label,
                "detail": _user_action_detail(row),
                "highlight": highlight,
            }
        )
    return entries


def _care_action_detail(row: Dict[str, Any]) -> str:
    """care_action 한 줄의 "대상 · 상세" 텍스트 — 이벤트 코드 · 대상자, 메모가 있으면 이어붙인다."""
    parts = [row["event_uid"]]
    if row["recipient_name"]:
        parts.append(row["recipient_name"])
    detail = " · ".join(p for p in parts if p)
    if row["memo"]:
        detail = f"{detail} · 메모: {row['memo']}" if detail else f"메모: {row['memo']}"
    return detail


def _fetch_care_action_entries(start: date, end: date) -> List[Dict[str, Any]]:
    query = """
        SELECT ca.acted_at, ca.action_type, ca.memo,
               ce.event_uid, rc.name AS recipient_name,
               u.login_id, u.display_name
        FROM care_action ca
        JOIN care_event ce ON ce.id = ca.care_event_id
        LEFT JOIN recipient rc ON rc.id = ce.recipient_id
        LEFT JOIN careguard_user u ON u.id = ca.user_id
        WHERE ca.acted_at::date BETWEEN %s AND %s
        ORDER BY ca.acted_at DESC
        LIMIT %s
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(query, (start, end, MAX_ENTRIES))
        rows = cur.fetchall()

    entries = []
    for row in rows:
        label, highlight = _label_for(CARE_ACTION_LABELS, row["action_type"])
        entries.append(
            {
                "occurred_at": row["acted_at"],
                "account": _account_name(row),
                "action": label,
                "detail": _care_action_detail(row),
                "highlight": highlight,
            }
        )
    return entries


@router.get("", response_model=List[AuditLogEntry])
def list_audit_log(
    start: Optional[date] = Query(None, description="조회 시작일. 생략하면 오늘부터 7일 전"),
    end: Optional[date] = Query(None, description="조회 종료일. 생략하면 오늘"),
    q: Optional[str] = Query(None, description="계정·행위·상세 텍스트 검색어"),
):
    """시스템 설정 > 감사 로그 탭. user_action_log(대시보드 조작)와 care_action(이벤트 대응
    조치 — 119/112 신고, 이벤트 완료 등)을 합쳐 시각 역순으로 반환한다.

    두 테이블은 조치 종류 체계가 서로 달라(§CARE_ACTION_LABELS 주석 참고) 각자 라벨 표로
    변환한 뒤 합친다 — 새 원본 테이블을 추가하고 싶으면 같은 모양의 _fetch_*_entries()
    함수를 하나 더 만들어 아래 리스트에 더하기만 하면 된다.
    """
    end = end or date.today()
    start = start or (end - timedelta(days=7))

    entries = _fetch_user_action_entries(start, end) + _fetch_care_action_entries(start, end)
    entries.sort(key=lambda e: e["occurred_at"], reverse=True)
    entries = entries[:MAX_ENTRIES]

    if q:
        keyword = q.strip().lower()
        if keyword:
            entries = [
                e
                for e in entries
                if keyword in (e["account"] or "").lower()
                or keyword in e["action"].lower()
                or keyword in e["detail"].lower()
            ]

    return entries
