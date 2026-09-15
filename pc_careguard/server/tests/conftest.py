import secrets
from datetime import datetime, timedelta, timezone

import bcrypt
import pytest
from fastapi.testclient import TestClient

from app.core import db
from app.main import app


def _create_user_with_role(role_code: str) -> int:
    login_id = f"test_{role_code}_{secrets.token_hex(4)}"
    password_hash = bcrypt.hashpw(b"testpass123", bcrypt.gensalt()).decode()
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM role WHERE code = %s", (role_code,))
        role_row = cur.fetchone()
        cur.execute(
            """
            INSERT INTO careguard_user (login_id, password_hash, role_id, is_active, display_name)
            VALUES (%s, %s, %s, TRUE, %s)
            RETURNING id
            """,
            (login_id, password_hash, role_row["id"], f"테스트 {role_code}"),
        )
        user_id = cur.fetchone()["id"]
        conn.commit()
    return user_id


def _issue_token(user_id: int) -> str:
    token = secrets.token_urlsafe(32)
    expires_at = datetime.now(timezone.utc) + timedelta(hours=1)
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "INSERT INTO careguard_session (token, user_id, expires_at) VALUES (%s, %s, %s)",
            (token, user_id, expires_at),
        )
        conn.commit()
    return token


def _cleanup_user(user_id: int) -> None:
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM careguard_session WHERE user_id = %s", (user_id,))
        cur.execute("DELETE FROM careguard_user WHERE id = %s", (user_id,))
        conn.commit()


@pytest.fixture
def client():
    """기존 테스트 대부분이 인증 없이 전체 API를 호출하던 걸 유지하기 위해,
    여기서 admin 토큰을 기본으로 실어둔다 — 기존 테스트 파일들은 전혀 안 고쳐도 된다."""
    user_id = _create_user_with_role("admin")
    token = _issue_token(user_id)
    test_client = TestClient(app)
    test_client.headers.update({"Authorization": f"Bearer {token}"})
    yield test_client
    _cleanup_user(user_id)


@pytest.fixture
def role_client():
    """role_code를 넘기면 그 role의 토큰이 실린 TestClient를 만들어주는 팩토리.
    403 테스트처럼 admin이 아닌 role로 호출해봐야 하는 테스트 전용."""
    created_user_ids = []

    def _make(role_code: str) -> TestClient:
        user_id = _create_user_with_role(role_code)
        created_user_ids.append(user_id)
        token = _issue_token(user_id)
        c = TestClient(app)
        c.headers.update({"Authorization": f"Bearer {token}"})
        return c

    yield _make

    for uid in created_user_ids:
        _cleanup_user(uid)


@pytest.fixture
def anonymous_client():
    """토큰을 아예 안 실은 요청(비로그인) 테스트용."""
    return TestClient(app)


@pytest.fixture
def cleanup_robot():
    created_codes = []
    yield created_codes
    if not created_codes:
        return
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM robot WHERE code = ANY(%s)", (created_codes,))
        conn.commit()


@pytest.fixture
def cleanup_recipient():
    created_codes = []
    yield created_codes
    if not created_codes:
        return
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM recipient WHERE code = ANY(%s)", (created_codes,))
        conn.commit()