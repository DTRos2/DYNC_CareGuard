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


def test_list_robots_is_online_reflects_connected_registry(client, cleanup_robot):
    from app.robot_registry import connected_robots

    code = _unique_code()
    cleanup_robot.append(code)
    client.post("/robots", json={"code": code, "name": "로봇3", "model": "turtlebot"})

    connected_robots.add(code)
    try:
        response = client.get("/robots")
        body = {r["code"]: r["is_online"] for r in response.json()}
        assert body[code] is True
    finally:
        connected_robots.discard(code)
