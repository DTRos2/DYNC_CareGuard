import uuid

import pytest


def _unique_code():
    return f"TEST-{uuid.uuid4().hex[:8]}"


def test_no_token_returns_401(anonymous_client):
    response = anonymous_client.get("/robots")
    assert response.status_code == 401


def test_guardian_cannot_create_recipient(role_client):
    guardian = role_client("guardian")
    response = guardian.post("/recipients", json={"name": "테스트", "home_address": "", "diseases": "", "note": ""})
    assert response.status_code == 403


def test_operator_cannot_list_accounts(role_client):
    operator = role_client("operator")
    response = operator.get("/accounts")
    assert response.status_code == 403


def test_operator_cannot_register_robot(role_client):
    operator = role_client("operator")
    response = operator.post("/robots", json={"code": _unique_code(), "name": "테스트 로봇", "model": "limo"})
    assert response.status_code == 403


def test_operator_can_list_robots(role_client):
    operator = role_client("operator")
    response = operator.get("/robots")
    assert response.status_code == 200


def test_guardian_cannot_access_teleop(role_client):
    guardian = role_client("guardian")
    response = guardian.post("/teleop/R-01", json={"linear": 0.0, "angular": 0.0})
    assert response.status_code == 403


def test_guardian_cannot_upload_map_image(role_client):
    guardian = role_client("guardian")
    response = guardian.post(
        "/media/upload-map",
        data={"robot_code": "R-01"},
        files={"file": ("test.pgm", b"fake", "application/octet-stream")},
    )
    assert response.status_code == 403


def test_admin_can_list_accounts(role_client):
    admin = role_client("admin")
    response = admin.get("/accounts")
    assert response.status_code == 200