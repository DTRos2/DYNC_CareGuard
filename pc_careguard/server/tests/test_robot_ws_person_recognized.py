import base64
import uuid

from app.api.v1.media import resolve_storage_path
from app.core import db


def _unique_code():
    return f"TEST-{uuid.uuid4().hex[:8]}"


def test_person_recognized_saves_capture_without_care_event(client, cleanup_robot):
    """person_recognized WS 메시지는 files(entity_type='recipient_recognition_capture')만
    만들고 care_event는 절대 만들지 않아야 한다(robot_ws.py::_handle_person_recognized —
    알림이 아니라 인식 기록이라서 intruder_detected와 다르게 처리됨)."""
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "대상자 인식 캡처 테스트"}).json()["id"]

    fake_jpeg = base64.b64encode(b"not a real jpeg, just bytes for the test").decode("ascii")
    file_id = None
    try:
        with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
            ws.send_json({
                "type": "person_recognized",
                "track_id": 12,
                "recipient_id": 999999,
                "verified_at_close_range": True,
                "nearest_dist": 0.8,
                "run_id": 1,
                "image_base64": fake_jpeg,
            })

        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) AS n FROM care_event WHERE robot_id = %s", (robot_id,))
            assert cur.fetchone()["n"] == 0

            cur.execute(
                "SELECT id, storage_path FROM files "
                "WHERE entity_type = 'recipient_recognition_capture' AND entity_id = %s",
                ("999999",),
            )
            file_row = cur.fetchone()
        assert file_row is not None
        file_id = file_row["id"]
        assert resolve_storage_path(file_row["storage_path"]).read_bytes() == base64.b64decode(fake_jpeg)
    finally:
        if file_id is not None:
            with db.get_connection() as conn, conn.cursor() as cur:
                cur.execute("SELECT storage_path FROM files WHERE id = %s", (file_id,))
                row = cur.fetchone()
                if row is not None:
                    resolve_storage_path(row["storage_path"]).unlink(missing_ok=True)
                cur.execute("DELETE FROM files WHERE id = %s", (file_id,))
                conn.commit()


def test_person_recognized_without_image_does_nothing(client, cleanup_robot):
    """image_base64가 없으면(캡처 실패) 조용히 무시하고 아무 것도 안 만들어야 한다."""
    code = _unique_code()
    cleanup_robot.append(code)
    client.post("/robots", json={"code": code, "name": "대상자 인식 사진없음 테스트"})

    with client.websocket_connect(f"/ws/robot?robot_code={code}") as ws:
        ws.send_json({"type": "person_recognized", "track_id": 1, "recipient_id": 999998})

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) AS n FROM files "
            "WHERE entity_type = 'recipient_recognition_capture' AND entity_id = %s",
            ("999998",),
        )
        assert cur.fetchone()["n"] == 0
