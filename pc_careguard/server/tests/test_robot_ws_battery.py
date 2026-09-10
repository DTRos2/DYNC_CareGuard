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
