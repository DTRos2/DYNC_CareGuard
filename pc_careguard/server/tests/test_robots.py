import uuid


def _unique_code():
    return f"TEST-{uuid.uuid4().hex[:8]}"


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
