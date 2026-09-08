import uuid

from app import db


def _unique_code():
    return f"TEST-{uuid.uuid4().hex[:8]}"


def _delete_threshold_row(robot_id: int) -> None:
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("DELETE FROM robot_threshold WHERE robot_id = %s", (robot_id,))
        conn.commit()


def test_register_robot_returns_created_item(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)

    response = client.post("/robots", json={
        "code": code,
        "name": "테스트 로봇",
        "model": "turtlebot",
        "ros_domain_id": 30,
    })

    assert response.status_code == 201
    body = response.json()
    assert body["code"] == code
    assert body["name"] == "테스트 로봇"
    assert body["model"] == "turtlebot"
    assert body["ros_domain_id"] == 30
    assert body["is_online"] is False


def test_register_robot_duplicate_code_returns_409(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    payload = {"code": code, "name": "로봇1", "model": "limo", "ros_domain_id": 1}

    first = client.post("/robots", json=payload)
    assert first.status_code == 201

    second = client.post("/robots", json=payload)
    assert second.status_code == 409


def test_list_robots_includes_registered_robot(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    client.post("/robots", json={"code": code, "name": "로봇2", "model": "limo", "ros_domain_id": 5})

    response = client.get("/robots")

    assert response.status_code == 200
    codes = [r["code"] for r in response.json()]
    assert code in codes


def test_register_robot_ros_domain_id_out_of_range_returns_422(client):
    response = client.post("/robots", json={
        "code": _unique_code(), "name": "로봇", "model": "turtlebot", "ros_domain_id": 500,
    })
    assert response.status_code == 422


def test_register_robot_code_too_long_returns_422(client):
    response = client.post("/robots", json={
        "code": "X" * 17, "name": "로봇", "model": "turtlebot",
    })
    assert response.status_code == 422


def test_deregister_robot_removes_it_from_list(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    created = client.post("/robots", json={"code": code, "name": "로봇4", "model": "turtlebot"}).json()

    response = client.delete(f"/robots/{created['id']}")

    assert response.status_code == 204
    codes = [r["code"] for r in client.get("/robots").json()]
    assert code not in codes


def test_deregister_robot_unknown_id_returns_404(client):
    response = client.delete("/robots/999999999")
    assert response.status_code == 404


def test_list_robots_is_online_reflects_real_ws_connection(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    client.post("/robots", json={"code": code, "name": "로봇3", "model": "turtlebot"})

    response_before = client.get("/robots")
    body_before = {r["code"]: r["is_online"] for r in response_before.json()}
    assert body_before[code] is False

    with client.websocket_connect(f"/ws/robot?robot_code={code}"):
        response_during = client.get("/robots")
        body_during = {r["code"]: r["is_online"] for r in response_during.json()}
        assert body_during[code] is True

    response_after = client.get("/robots")
    body_after = {r["code"]: r["is_online"] for r in response_after.json()}
    assert body_after[code] is False


def test_get_robot_thresholds_returns_defaults_when_never_saved(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "임계값 기본값 테스트"}).json()["id"]

    response = client.get(f"/robots/{robot_id}/thresholds")

    assert response.status_code == 200
    assert response.json() == {
        "battery_attention_pct": 30,
        "battery_return_pct": 20,
        "voice_wait_sec": 8,
        "reconnect_retry_sec": 30,
        "med_missed_wait_min": 10,
        "med_reannounce_count": 2,
    }


def test_set_robot_thresholds_persists_and_is_returned(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    robot_id = client.post("/robots", json={"code": code, "name": "임계값 저장 테스트"}).json()["id"]

    try:
        custom = {
            "battery_attention_pct": 40,
            "battery_return_pct": 25,
            "voice_wait_sec": 12,
            "reconnect_retry_sec": 15,
            "med_missed_wait_min": 20,
            "med_reannounce_count": 3,
        }
        put_response = client.put(f"/robots/{robot_id}/thresholds", json=custom)
        assert put_response.status_code == 204

        get_response = client.get(f"/robots/{robot_id}/thresholds")
        assert get_response.json() == custom
    finally:
        _delete_threshold_row(robot_id)


def test_apply_thresholds_to_all_robots_upserts_every_registered_robot(client, cleanup_robot):
    code_a = _unique_code()
    code_b = _unique_code()
    cleanup_robot.extend([code_a, code_b])
    robot_a_id = client.post("/robots", json={"code": code_a, "name": "일괄적용A"}).json()["id"]
    robot_b_id = client.post("/robots", json={"code": code_b, "name": "일괄적용B"}).json()["id"]

    try:
        custom = {
            "battery_attention_pct": 35,
            "battery_return_pct": 15,
            "voice_wait_sec": 10,
            "reconnect_retry_sec": 20,
            "med_missed_wait_min": 5,
            "med_reannounce_count": 1,
        }
        response = client.put("/robots/thresholds/apply-all", json=custom)
        assert response.status_code == 204

        assert client.get(f"/robots/{robot_a_id}/thresholds").json() == custom
        assert client.get(f"/robots/{robot_b_id}/thresholds").json() == custom
    finally:
        _delete_threshold_row(robot_a_id)
        _delete_threshold_row(robot_b_id)


def test_get_robot_thresholds_unknown_robot_returns_404(client):
    response = client.get("/robots/999999999/thresholds")
    assert response.status_code == 404


def test_set_robot_thresholds_unknown_robot_returns_404(client):
    response = client.put("/robots/999999999/thresholds", json={})
    assert response.status_code == 404
