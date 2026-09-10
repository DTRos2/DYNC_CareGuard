import pytest
from fastapi.testclient import TestClient

from app.core import db
from app.main import app


@pytest.fixture
def client():
    return TestClient(app)


@pytest.fixture
def cleanup_robot():
    """테스트가 등록한 robot.code들을 테스트가 끝나면 DB에서 지운다."""
    created_codes = []
    yield created_codes
    if not created_codes:
        return
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM robot WHERE code = ANY(%s)", (created_codes,))
        conn.commit()


@pytest.fixture
def cleanup_recipient():
    """테스트가 등록한 recipient.code들을 테스트가 끝나면 DB에서 지운다."""
    created_codes = []
    yield created_codes
    if not created_codes:
        return
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM recipient WHERE code = ANY(%s)", (created_codes,))
        conn.commit()
