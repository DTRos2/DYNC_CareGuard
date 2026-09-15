import uuid

from app.core import db
from app.ws import robot_ws


def _unique_code():
    return f"TEST-{uuid.uuid4().hex[:8]}"


def _system_events_for(robot_id):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT event_type, severity, message FROM system_event WHERE robot_id = %s ORDER BY id",
            (robot_id,),
        )
        return cur.fetchall()


def _cleanup_system_events(robot_id):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM system_event WHERE robot_id = %s", (robot_id,))
        conn.commit()


def _set_battery_thresholds(client, robot_id, attention_pct, return_pct):
    client.put(f"/robots/{robot_id}/thresholds", json={
        "battery_attention_pct": attention_pct,
        "battery_return_pct": return_pct,
        "voice_wait_sec": 8,
        "reconnect_retry_sec": 30,
        "med_missed_wait_min": 10,
        "med_reannounce_count": 2,
    })


def test_battery_below_attention_creates_warn_event(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "배터리 주의 테스트"}).json()["id"]
    _set_battery_thresholds(client, robot_id, attention_pct=30, return_pct=15)

    try:
        with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
            ws.send_json({"type": "battery_state", "percentage": 25})

        events = _system_events_for(robot_id)
        assert len(events) == 1
        assert events[0]["event_type"] == "battery_attention"
        assert events[0]["severity"] == "warn"
    finally:
        _cleanup_system_events(robot_id)
        robot_ws._battery_alert_state.pop(code, None)


def test_battery_below_return_creates_error_event(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "배터리 복귀 테스트"}).json()["id"]
    _set_battery_thresholds(client, robot_id, attention_pct=30, return_pct=15)

    try:
        with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
            ws.send_json({"type": "battery_state", "percentage": 10})

        events = _system_events_for(robot_id)
        assert len(events) == 1
        assert events[0]["event_type"] == "battery_return_needed"
        assert events[0]["severity"] == "error"
    finally:
        _cleanup_system_events(robot_id)
        robot_ws._battery_alert_state.pop(code, None)


def test_battery_state_does_not_repeat_same_alert(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "배터리 중복 방지 테스트"}).json()["id"]
    _set_battery_thresholds(client, robot_id, attention_pct=30, return_pct=15)

    try:
        with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
            ws.send_json({"type": "battery_state", "percentage": 25})
            ws.send_json({"type": "battery_state", "percentage": 22})
            ws.send_json({"type": "battery_state", "percentage": 20})

        events = _system_events_for(robot_id)
        assert len(events) == 1
    finally:
        _cleanup_system_events(robot_id)
        robot_ws._battery_alert_state.pop(code, None)


def test_battery_above_attention_does_not_create_event(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "배터리 정상 테스트"}).json()["id"]
    _set_battery_thresholds(client, robot_id, attention_pct=30, return_pct=15)

    try:
        with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
            ws.send_json({"type": "battery_state", "percentage": 80})

        assert _system_events_for(robot_id) == []
    finally:
        _cleanup_system_events(robot_id)
        robot_ws._battery_alert_state.pop(code, None)


def test_battery_return_needed_requires_recovery_margin(client, cleanup_robot):
    """2026-09-14 R-07(LIMO)에서 실제로 확인된 문제 재현 — 모터 구동/정지에 따라
    복귀 임계값(28%) 근처에서 27%↔31%로 반복해서 튀는데, 여유분 없이 경계만 보면
    return_needed <-> normal이 계속 뒤집힌다. _handle_battery_state를 직접 호출해서
    (WS 경로의 5초 스로틀을 우회) 여러 판정을 연속으로 검증한다."""
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "배터리 히스테리시스 테스트"}).json()["id"]
    _set_battery_thresholds(client, robot_id, attention_pct=30, return_pct=28)

    try:
        robot_ws._handle_battery_state(code, 27)
        assert robot_ws._battery_alert_state[code] == "return_needed"

        # return_pct(28%)를 살짝 넘는 31%로 튀어도 여유분(+5%p=33%) 미만이라 복귀 필요 유지.
        robot_ws._handle_battery_state(code, 31)
        assert robot_ws._battery_alert_state[code] == "return_needed"

        # 다시 27%로 떨어져도 상태 변화 없음 — 위 두 호출 다 "새 이벤트 없음"이어야 한다.
        robot_ws._handle_battery_state(code, 27)
        assert robot_ws._battery_alert_state[code] == "return_needed"

        events = _system_events_for(robot_id)
        assert len(events) == 1
        assert events[0]["event_type"] == "battery_return_needed"

        # 여유분을 확실히 넘기면(34% > 28+5) 비로소 정상으로 복귀 — 다만 "정상 회복"
        # 로그는 더 이상 안 남긴다(2026-09-14 사용자 요청, 아래 covers 참고).
        robot_ws._handle_battery_state(code, 34)
        assert robot_ws._battery_alert_state[code] == "normal"

        events = _system_events_for(robot_id)
        assert len(events) == 1
    finally:
        _cleanup_system_events(robot_id)
        robot_ws._battery_alert_state.pop(code, None)


def test_battery_recovered_event_is_not_logged(client, cleanup_robot):
    """2026-09-14 사용자 요청 — "배터리 주의"/"배터리 복귀 필요"는 로그로 남기되,
    거기서 정상으로 돌아왔다는 "배터리 정상 회복" 로그는 필요 없다고 해서 뺐다.
    상태 추적(_battery_alert_state)은 히스테리시스 판정에 계속 필요해 그대로 갱신되지만
    system_event는 안 남아야 한다."""
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "배터리 정상회복 로그 제외 테스트"}).json()["id"]
    _set_battery_thresholds(client, robot_id, attention_pct=30, return_pct=15)

    try:
        robot_ws._handle_battery_state(code, 25)
        assert robot_ws._battery_alert_state[code] == "attention"
        assert len(_system_events_for(robot_id)) == 1

        robot_ws._handle_battery_state(code, 80)
        assert robot_ws._battery_alert_state[code] == "normal"
        # 상태는 normal로 바뀌었지만 system_event는 추가되지 않는다.
        assert len(_system_events_for(robot_id)) == 1
    finally:
        _cleanup_system_events(robot_id)
        robot_ws._battery_alert_state.pop(code, None)


def test_battery_attention_requires_recovery_margin(client, cleanup_robot):
    """return_needed와 같은 증상이 attention_pct(30%) 근처에서도 재현됐다 —
    여유분 없이 경계만 보면 "배터리 주의" <-> "정상 회복"이 번갈아 반복 생성된다."""
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "배터리 주의 히스테리시스 테스트"}).json()["id"]
    _set_battery_thresholds(client, robot_id, attention_pct=30, return_pct=15)

    try:
        robot_ws._handle_battery_state(code, 29)
        assert robot_ws._battery_alert_state[code] == "attention"

        # attention_pct(30%)를 살짝 넘는 33%로 튀어도 여유분(+5%p=35%) 미만이라 주의 유지.
        robot_ws._handle_battery_state(code, 33)
        assert robot_ws._battery_alert_state[code] == "attention"

        robot_ws._handle_battery_state(code, 29)
        assert robot_ws._battery_alert_state[code] == "attention"

        events = _system_events_for(robot_id)
        assert len(events) == 1
        assert events[0]["event_type"] == "battery_attention"

        # 여유분을 확실히 넘기면(36% > 30+5) 비로소 정상으로 복귀 — "정상 회복" 로그는
        # 안 남긴다(test_battery_recovered_event_is_not_logged 참고).
        robot_ws._handle_battery_state(code, 36)
        assert robot_ws._battery_alert_state[code] == "normal"

        events = _system_events_for(robot_id)
        assert len(events) == 1
    finally:
        _cleanup_system_events(robot_id)
        robot_ws._battery_alert_state.pop(code, None)
