import asyncio
import uuid
from datetime import datetime, timedelta

import pytest

from app.core import db
from app.services.scheduler import _medication_tick


def _unique_code():
    return f"TEST-{uuid.uuid4().hex[:8]}"


@pytest.fixture
def medication_case():
    """로봇 1대 + 대상자 1명 + 복약 스케줄 1건(방금 미수령 상태)을 만들고,
    테스트가 끝나면 만든 행을 전부 지운다(care_event/medication_log 포함)."""
    robot_code = _unique_code()
    recipient_code = _unique_code()
    past_dose_time = (datetime.now() - timedelta(minutes=15)).time()

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "INSERT INTO robot (code, name, model) VALUES (%s, %s, 'limo') RETURNING id",
            (robot_code, "테스트 로봇"),
        )
        robot_id = cur.fetchone()["id"]

        cur.execute(
            "INSERT INTO recipient (code, name, robot_id) VALUES (%s, %s, %s) RETURNING id",
            (recipient_code, "테스트 대상자", robot_id),
        )
        recipient_id = cur.fetchone()["id"]

        cur.execute(
            """
            INSERT INTO medication_schedule (recipient_id, slot_no, drug_name, dose_time, days_of_week)
            VALUES (%s, 1, '테스트약', %s, 127) RETURNING id
            """,
            (recipient_id, past_dose_time),
        )
        schedule_id = cur.fetchone()["id"]
        conn.commit()

    yield {"robot_id": robot_id, "recipient_id": recipient_id, "schedule_id": schedule_id}

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM care_event WHERE recipient_id = %s", (recipient_id,))
        cur.execute("DELETE FROM medication_log WHERE schedule_id = %s", (schedule_id,))
        cur.execute("DELETE FROM medication_schedule WHERE id = %s", (schedule_id,))
        cur.execute("DELETE FROM recipient WHERE id = %s", (recipient_id,))
        cur.execute("DELETE FROM robot_threshold WHERE robot_id = %s", (robot_id,))
        cur.execute("DELETE FROM robot WHERE id = %s", (robot_id,))
        conn.commit()


def test_medication_tick_creates_missed_log_and_event(medication_case):
    asyncio.run(_medication_tick())

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT result FROM medication_log WHERE schedule_id = %s",
            (medication_case["schedule_id"],),
        )
        logs = cur.fetchall()
        cur.execute(
            "SELECT event_type, severity, summary FROM care_event WHERE recipient_id = %s",
            (medication_case["recipient_id"],),
        )
        events = cur.fetchall()

    assert len(logs) == 1
    assert logs[0]["result"] == "missed"
    assert len(events) == 1
    assert events[0]["event_type"] == "MED_MISSED"
    assert events[0]["severity"] == "attention"
    assert "테스트약" in events[0]["summary"]


def test_medication_tick_does_not_duplicate_within_wait_window(medication_case):
    asyncio.run(_medication_tick())
    asyncio.run(_medication_tick())  # 재안내 간격(기본 10분) 안이라 추가 경보가 없어야 함

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) AS n FROM medication_log WHERE schedule_id = %s",
            (medication_case["schedule_id"],),
        )
        count = cur.fetchone()["n"]

    assert count == 1


def test_medication_tick_skips_when_already_delivered(medication_case):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            INSERT INTO medication_log (schedule_id, recipient_id, robot_id, slot_no, result, delivered_at)
            VALUES (%s, %s, %s, 1, 'delivered', NOW())
            """,
            (medication_case["schedule_id"], medication_case["recipient_id"], medication_case["robot_id"]),
        )
        conn.commit()

    asyncio.run(_medication_tick())

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) AS n FROM care_event WHERE recipient_id = %s",
            (medication_case["recipient_id"],),
        )
        count = cur.fetchone()["n"]

    assert count == 0
