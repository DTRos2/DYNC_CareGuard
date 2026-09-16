import base64
import uuid

from app.api.v1.media import resolve_storage_path
from app.core import db


def _unique_code():
    return f"TEST-{uuid.uuid4().hex[:8]}"


def _care_events_for(robot_id):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT event_type, severity, trigger_path, recipient_id, status
            FROM care_event WHERE robot_id = %s ORDER BY id
            """,
            (robot_id,),
        )
        return cur.fetchall()


def _cleanup_care_events(robot_id):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM care_event WHERE robot_id = %s", (robot_id,))
        conn.commit()


def test_intruder_detected_creates_care_event(client, cleanup_robot):
    """patrol_intruder_watch_node.py가 보내는 것과 같은 모양의 메시지를 WS로 보내면
    care_event(INTRUDER)가 생성돼야 한다 — recipient_id는 아무도 매치 안 됐다는 뜻이라
    NULL이 맞다(robot_ws.py::_handle_intruder_detected 참고)."""
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "침입자 감지 테스트"}).json()["id"]

    try:
        with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
            ws.send_json({
                "type": "intruder_detected",
                "track_id": 3,
                "track_age_frames": 22,
                "stable_frac": 0.85,
                "nearest_dist": 0.71,
                "run_id": 1,
            })

        events = _care_events_for(robot_id)
        assert len(events) == 1
        assert events[0]["event_type"] == "INTRUDER"
        assert events[0]["severity"] == "emergency"
        assert events[0]["trigger_path"] == "B"
        assert events[0]["recipient_id"] is None
        assert events[0]["status"] == "open"
    finally:
        _cleanup_care_events(robot_id)


def test_intruder_detected_with_image_creates_capture_file(client, cleanup_robot):
    """image_base64가 실려 오면 files(entity_type='intruder_capture')를 만들고
    care_event.evidence.capture_file_id로 연결해야 한다(robot_ws.py::
    _handle_intruder_detected + media.py::save_intruder_capture)."""
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "침입자 캡처 테스트"}).json()["id"]

    fake_jpeg = base64.b64encode(b"not a real jpeg, just bytes for the test").decode("ascii")
    file_id = None
    try:
        with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
            ws.send_json({
                "type": "intruder_detected",
                "track_id": 7,
                "nearest_dist": 1.2,
                "run_id": 1,
                "image_base64": fake_jpeg,
            })

        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute(
                "SELECT id, evidence FROM care_event WHERE robot_id = %s", (robot_id,)
            )
            event = cur.fetchone()
        assert event is not None
        file_id = event["evidence"].get("capture_file_id")
        assert file_id is not None

        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute(
                "SELECT entity_type, entity_id, storage_path FROM files WHERE id = %s", (file_id,)
            )
            file_row = cur.fetchone()
        assert file_row["entity_type"] == "intruder_capture"
        assert file_row["entity_id"] == str(event["id"])
        stored_path = resolve_storage_path(file_row["storage_path"])
        assert stored_path.read_bytes() == base64.b64decode(fake_jpeg)
    finally:
        _cleanup_care_events(robot_id)
        if file_id is not None:
            with db.get_connection() as conn, conn.cursor() as cur:
                cur.execute("SELECT storage_path FROM files WHERE id = %s", (file_id,))
                row = cur.fetchone()
                if row is not None:
                    resolve_storage_path(row["storage_path"]).unlink(missing_ok=True)
                cur.execute("DELETE FROM files WHERE id = %s", (file_id,))
                conn.commit()


def test_intruder_detected_uses_running_patrol_run(client, cleanup_robot):
    """이 로봇에 status='running'인 patrol_run이 있으면 care_event.patrol_run_id가
    그 회차로 채워져야 한다(robot_ws.py::_handle_intruder_detected)."""
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "침입자+순찰 연동 테스트"}).json()["id"]

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "INSERT INTO patrol_run (robot_id, trigger, status, waypoints_total) "
            "VALUES (%s, 'manual', 'running', 0) RETURNING id",
            (robot_id,),
        )
        run_id = cur.fetchone()["id"]
        conn.commit()

    try:
        with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
            ws.send_json({"type": "intruder_detected", "track_id": 1})

        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT patrol_run_id FROM care_event WHERE robot_id = %s", (robot_id,))
            row = cur.fetchone()
        assert row["patrol_run_id"] == run_id
    finally:
        _cleanup_care_events(robot_id)
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("DELETE FROM patrol_run WHERE id = %s", (run_id,))
            conn.commit()
