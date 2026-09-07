# 터틀봇 ↔ 서버 ↔ 대시보드 실시간 연결 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 터틀봇을 켜면 서버에 WebSocket으로 실제 연결되고, 대시보드 "로봇 관리" 탭에 온라인/오프라인 상태가 폴링 없이 실시간으로 반영되게 만든다.

**Architecture:** 로봇(라즈베리파이의 ROS2 노드) → FastAPI 서버는 WebSocket(`/ws/robot/{code}`, heartbeat). 대시보드 등록/최초 조회는 REST(`GET`/`POST /robots`). 서버 → 대시보드는 WebSocket(`/ws/dashboard`)으로 상태 변화를 push. 온라인 여부는 서버 프로세스 메모리(`connected_robots` 집합)로만 판단하고 DB 컬럼은 추가하지 않는다.

**Tech Stack:** FastAPI + psycopg2 (서버, 기존과 동일), ROS2 Humble ament_python + `websocket-client` (로봇 에이전트), Qt6 Widgets + Qt6::WebSockets (대시보드).

**Spec:** `docs/superpowers/specs/2026-09-07-turtlebot-realtime-connectivity-design.md`

## Global Constraints

- 대시보드는 실시간 상태에 REST 폴링을 쓰지 않는다 — WebSocket push만 사용한다 (스펙 "아키텍처")
- 온라인 판정은 서버 프로세스 메모리로만 하고 DB 스키마는 변경하지 않는다 (스펙 "아키텍처")
- 미등록 `code`로 들어오는 `/ws/robot/{code}` 접속은 서버가 즉시 거부한다 (스펙 "서버")
- heartbeat 주기 5초, 서버 타임아웃 15초 (스펙 "서버")
- `careguard_turtlebot_agent/`는 리포 최상위에 `src/` 없이 바로 둔다. `robot_careguard_ws/`, `pc_careguard/`, `shared/`, `docs/`, `deploy/`는 구조·내용 그대로 둔다 (스펙 "리포 구조 변경")
- 로봇 IP·네임스페이스·노드 상태·담당 대상자 배정은 이번 범위 밖 — 대시보드 표의 해당 칸은 빈 값/placeholder로 남는다. "상태" 칸만 실제 `is_online`을 반영한다 (스펙 "범위")
- 이 저장소에는 기존 자동 테스트가 전혀 없다(서버 Python 쪽, 대시보드 C++ 쪽 모두). 서버 쪽은 이번에 `pytest`를 새로 들여오되(순수 신규 로직이라 가치가 큼), 대시보드(Qt/C++)와 ROS2 노드는 기존 관례대로 빌드 성공 + 수동 스모크 테스트로 검증한다 — 새 C++/ROS2 테스트 프레임워크를 이번에 들여오지 않는다

---

## Task 1: 서버 테스트 인프라

**Files:**
- Create: `pc_careguard/server/requirements-dev.txt`
- Create: `pc_careguard/server/pytest.ini`
- Create: `pc_careguard/server/tests/__init__.py` (빈 파일)
- Create: `pc_careguard/server/tests/conftest.py`
- Test: `pc_careguard/server/tests/test_health.py` (이 태스크 자체의 검증용 — 이후 태스크에서 지워도 됨)

**Interfaces:**
- Produces: `conftest.py`의 `client` fixture (`fastapi.testclient.TestClient` 인스턴스), `cleanup_robot` fixture(테스트가 등록한 `robot.code` 문자열 리스트 — 테스트 끝나면 그 code들을 DB에서 지움). 이후 모든 서버 태스크의 테스트가 이 두 fixture를 그대로 가져다 쓴다

- [ ] **Step 1: 사전 준비 확인**

로컬 Postgres가 떠 있어야 한다:
```bash
docker compose -f /home/cce/twomainProject/deploy/docker-compose.yml up -d postgres
```
`deploy/.env`가 없으면 `deploy/.env.example`을 복사해서 채운다 (이미 있으면 건너뛴다).

- [ ] **Step 2: 개발 의존성 파일 작성**

`pc_careguard/server/requirements-dev.txt`:
```
pytest
httpx
```

- [ ] **Step 3: venv에 설치**

```bash
cd /home/cce/twomainProject/pc_careguard/server
.venv/bin/pip install -r requirements-dev.txt
```

- [ ] **Step 4: pytest 설정 작성**

`pc_careguard/server/pytest.ini`:
```ini
[pytest]
pythonpath = .
```
(어느 디렉토리에서 `pytest`를 실행하든 `from app.xxx import ...`가 되게 하기 위함)

`pc_careguard/server/tests/__init__.py`: 빈 파일로 생성.

- [ ] **Step 5: conftest.py 작성**

`pc_careguard/server/tests/conftest.py`:
```python
import pytest
from fastapi.testclient import TestClient

from app import db
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
```

- [ ] **Step 6: 인프라가 도는지 확인하는 최소 테스트 작성**

`pc_careguard/server/tests/test_health.py`:
```python
def test_health_endpoint_returns_ok(client):
    response = client.get("/health")
    assert response.status_code == 200
    assert response.json() == {"status": "ok"}
```

- [ ] **Step 7: 실행해서 통과 확인**

```bash
cd /home/cce/twomainProject/pc_careguard/server
.venv/bin/pytest tests/ -v
```
Expected: `test_health_endpoint_returns_ok` PASS. (`/health`은 이미 `main.py`에 있는 기존 엔드포인트 — 새로 만들 필요 없음)

- [ ] **Step 8: 커밋**

```bash
git add pc_careguard/server/requirements-dev.txt pc_careguard/server/pytest.ini \
        pc_careguard/server/tests/__init__.py pc_careguard/server/tests/conftest.py \
        pc_careguard/server/tests/test_health.py
git commit -m "test(server): pytest 테스트 인프라 추가"
```

---

## Task 2: 서버 — 로봇 registry + REST (`GET`/`POST /robots`)

**Files:**
- Create: `pc_careguard/server/app/robot_registry.py`
- Modify: `pc_careguard/server/app/schemas.py` (파일 끝에 클래스 추가)
- Create: `pc_careguard/server/app/robots.py`
- Modify: `pc_careguard/server/app/main.py:1-16` (import + include_router 추가)
- Test: `pc_careguard/server/tests/test_robots.py`

**Interfaces:**
- Consumes: Task 1의 `client`, `cleanup_robot` fixture
- Produces:
  - `app.robot_registry.connected_robots: set[str]` — 접속 중인 로봇 code 집합
  - `app.robot_registry.dashboard_sockets: list[WebSocket]` — 접속 중인 대시보드 소켓들
  - `app.robot_registry.broadcast_status(code: str, online: bool) -> Awaitable[None]` — `dashboard_sockets` 전체에 상태 push, 끊긴 소켓은 정리
  - REST `GET /robots` → `List[RobotItem]`, `POST /robots` → `RobotItem`(201) / 409(코드 중복)
  - Task 3(`robot_ws.py`)이 `connected_robots`/`dashboard_sockets`/`broadcast_status`를 그대로 가져다 쓴다

- [ ] **Step 1: robot_registry.py 작성 (로직 자체, 아직 라우터에서 안 씀)**

`pc_careguard/server/app/robot_registry.py`:
```python
import json

from fastapi import WebSocket

# 접속 중인 로봇의 code 집합. DB 컬럼이 아니라 서버 프로세스 메모리로만 관리한다 —
# 재시작하면 리셋되지만 이번 범위(최소 연결)엔 충분하다.
connected_robots: set[str] = set()

# 실시간 상태를 구독 중인 대시보드 WebSocket들.
dashboard_sockets: list[WebSocket] = []


async def broadcast_status(code: str, online: bool) -> None:
    message = json.dumps({"type": "robot_status", "code": code, "online": online})
    dead: list[WebSocket] = []
    for socket in dashboard_sockets:
        try:
            await socket.send_text(message)
        except Exception:
            dead.append(socket)
    for socket in dead:
        dashboard_sockets.remove(socket)
```

이 파일 자체는 라우터가 없어 TestClient로 직접 테스트하기보다, Task 3에서 `/ws/robot/{code}` 통합 테스트로 간접 검증한다(스펙에도 "서버: robots.py에 대해... robot_ws.py는 TestClient의 WebSocket 테스트로" 라고 돼 있음). 여기서는 import 에러가 없는지만 아래 Step 2에서 확인한다.

- [ ] **Step 2: schemas.py에 DTO 추가**

`pc_careguard/server/app/schemas.py` 파일 맨 끝에 추가:
```python


class RobotItem(BaseModel):
    id: int
    code: str
    name: str
    model: str
    ros_domain_id: Optional[int] = None
    is_online: bool


class RegisterRobotRequest(BaseModel):
    code: str
    name: str
    model: str = "limo"
    ros_domain_id: Optional[int] = None


class RegisterRobotResponse(RobotItem):
    pass
```

- [ ] **Step 3: 실패하는 테스트 작성 (POST 등록)**

`pc_careguard/server/tests/test_robots.py`:
```python
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
```

- [ ] **Step 4: 실행해서 실패 확인 (라우터가 아직 없음)**

```bash
cd /home/cce/twomainProject/pc_careguard/server
.venv/bin/pytest tests/test_robots.py -v
```
Expected: FAIL — `404 Not Found` (아직 `/robots` 라우트가 없음)

- [ ] **Step 5: robots.py 구현**

`pc_careguard/server/app/robots.py`:
```python
import psycopg2
from fastapi import APIRouter, HTTPException
from typing import List

from . import db
from .robot_registry import connected_robots
from .schemas import RegisterRobotRequest, RegisterRobotResponse, RobotItem

router = APIRouter(prefix="/robots", tags=["robots"])

LIST_QUERY = """
    SELECT id, code, name, model, ros_domain_id
    FROM robot
    WHERE deregistered_at IS NULL
    ORDER BY id
"""


def _row_to_item(row) -> RobotItem:
    return RobotItem(
        id=row["id"],
        code=row["code"],
        name=row["name"],
        model=row["model"],
        ros_domain_id=row["ros_domain_id"],
        is_online=row["code"] in connected_robots,
    )


@router.get("", response_model=List[RobotItem])
def list_robots():
    """로봇 관리 탭 목록. P7SettingsPage가 화면 진입 시 이걸로 채운다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(LIST_QUERY)
        rows = cur.fetchall()
    return [_row_to_item(r) for r in rows]


@router.post("", response_model=RegisterRobotResponse, status_code=201)
def register_robot(payload: RegisterRobotRequest):
    """로봇 등록 모달의 "등록" 버튼이 호출하는 엔드포인트."""
    with db.get_connection() as conn, conn.cursor() as cur:
        try:
            cur.execute(
                """
                INSERT INTO robot (code, name, model, ros_domain_id)
                VALUES (%s, %s, %s, %s)
                RETURNING id
                """,
                (payload.code, payload.name, payload.model, payload.ros_domain_id),
            )
        except psycopg2.errors.UniqueViolation:
            conn.rollback()
            raise HTTPException(status_code=409, detail="이미 등록된 로봇 코드입니다")

        robot_id = cur.fetchone()["id"]
        conn.commit()

    return RegisterRobotResponse(
        id=robot_id,
        code=payload.code,
        name=payload.name,
        model=payload.model,
        ros_domain_id=payload.ros_domain_id,
        is_online=False,
    )
```

- [ ] **Step 6: main.py에 라우터 등록**

`pc_careguard/server/app/main.py`, 기존:
```python
from .accounts import router as accounts_router
from .audit import router as audit_router
from .media import router as media_router
from .media import video_events_router
from .schemas import LoginRequest, LoginResponse

app = FastAPI(title="CareGuard Server")
app.include_router(media_router)
app.include_router(video_events_router)
app.include_router(accounts_router)
app.include_router(audit_router)
```
다음으로 교체:
```python
from .accounts import router as accounts_router
from .audit import router as audit_router
from .media import router as media_router
from .media import video_events_router
from .robots import router as robots_router
from .schemas import LoginRequest, LoginResponse

app = FastAPI(title="CareGuard Server")
app.include_router(media_router)
app.include_router(video_events_router)
app.include_router(accounts_router)
app.include_router(audit_router)
app.include_router(robots_router)
```
(`robot_ws` 라우터는 Task 3에서 여기에 한 줄 더 추가한다)

- [ ] **Step 7: 테스트 통과 확인**

```bash
cd /home/cce/twomainProject/pc_careguard/server
.venv/bin/pytest tests/test_robots.py -v
```
Expected: 4개 테스트 모두 PASS

- [ ] **Step 8: 커밋**

```bash
git add pc_careguard/server/app/robot_registry.py pc_careguard/server/app/schemas.py \
        pc_careguard/server/app/robots.py pc_careguard/server/app/main.py \
        pc_careguard/server/tests/test_robots.py
git commit -m "feat(server): 로봇 등록·조회 REST API 추가"
```

---

## Task 3: 서버 — WebSocket (`/ws/robot/{code}`, `/ws/dashboard`)

**Files:**
- Create: `pc_careguard/server/app/robot_ws.py`
- Modify: `pc_careguard/server/app/main.py` (import + include_router 한 줄 추가)
- Test: `pc_careguard/server/tests/test_robot_ws.py`

**Interfaces:**
- Consumes: Task 2의 `app.robot_registry.connected_robots`, `dashboard_sockets`, `broadcast_status`; `client`/`cleanup_robot` fixture
- Produces: `/ws/robot/{code}` (등록 안 된 code는 close code 4404로 거부, 등록된 code는 accept 후 5초 heartbeat 기준 15초 타임아웃), `/ws/dashboard` (accept 직후 `{"type":"snapshot","online_codes":[...]}` 전송, 이후 `{"type":"robot_status","code":...,"online":...}` push).모듈 상수 `robot_ws.HEARTBEAT_TIMEOUT`(기본 15) — 테스트에서 `monkeypatch`로 줄여서 씀

> **WebSocket 테스트 시 주의**: `TestClient.websocket_connect()`를 같은 스레드에서 두 개 이상 동시에 열어놓고 서로 메시지를 주고받게 하면(예: 대시보드 소켓을 연 채로 로봇 소켓을 열고 닫는 걸 같은 `with` 블록 흐름 안에서 순서대로 하는 식) 교착 상태에 빠질 수 있다(브로드캐스트가 상대방이 받아줄 때까지 블로킹되는데, 그 상대방을 읽어야 할 메인 스레드가 다른 소켓의 `with` 종료를 기다리며 멈춰 있는 경우). 아래 테스트들은 이 문제를 피하려고 **로봇 소켓 하나만 열어두고 `connected_robots`/`broadcast_status` 같은 서버 쪽 전역 상태를 직접 확인하는 방식**으로 작성돼 있다 — 이 패턴을 그대로 따른다.

- [ ] **Step 1: 실패하는 테스트 작성**

`pc_careguard/server/tests/test_robot_ws.py`:
```python
import time
import uuid
from unittest.mock import AsyncMock

import pytest
from starlette.websockets import WebSocketDisconnect

from app import robot_ws
from app.robot_registry import connected_robots


def _unique_code():
    return f"TEST-{uuid.uuid4().hex[:8]}"


def test_unregistered_code_is_rejected(client):
    with pytest.raises(WebSocketDisconnect) as exc_info:
        with client.websocket_connect(f"/ws/robot/{_unique_code()}"):
            pass
    assert exc_info.value.code == 4404


def test_registered_robot_connect_and_disconnect_updates_registry(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    client.post("/robots", json={"code": code, "name": "로봇", "model": "turtlebot", "ros_domain_id": 11})

    assert code not in connected_robots
    with client.websocket_connect(f"/ws/robot/{code}"):
        assert code in connected_robots

        response = client.get("/robots")
        body = {r["code"]: r["is_online"] for r in response.json()}
        assert body[code] is True
    assert code not in connected_robots


def test_robot_connect_and_disconnect_broadcasts_status(client, cleanup_robot, monkeypatch):
    code = _unique_code()
    cleanup_robot.append(code)
    client.post("/robots", json={"code": code, "name": "로봇", "model": "turtlebot", "ros_domain_id": 12})

    spy = AsyncMock()
    monkeypatch.setattr(robot_ws, "broadcast_status", spy)

    with client.websocket_connect(f"/ws/robot/{code}"):
        pass

    spy.assert_any_await(code, True)
    spy.assert_any_await(code, False)


def test_heartbeat_timeout_removes_robot_from_registry(client, cleanup_robot, monkeypatch):
    monkeypatch.setattr(robot_ws, "HEARTBEAT_TIMEOUT", 0.2)
    code = _unique_code()
    cleanup_robot.append(code)
    client.post("/robots", json={"code": code, "name": "로봇", "model": "turtlebot", "ros_domain_id": 13})

    with client.websocket_connect(f"/ws/robot/{code}"):
        assert code in connected_robots
        time.sleep(0.6)  # HEARTBEAT_TIMEOUT(0.2s)보다 넉넉히 길게 대기 — 서버가 스스로 타임아웃
        assert code not in connected_robots


def test_dashboard_socket_receives_snapshot_of_connected_robots(client, cleanup_robot):
    code = _unique_code()
    cleanup_robot.append(code)
    client.post("/robots", json={"code": code, "name": "로봇", "model": "turtlebot", "ros_domain_id": 14})

    with client.websocket_connect(f"/ws/robot/{code}"):
        with client.websocket_connect("/ws/dashboard") as dash:
            snapshot = dash.receive_json()
            assert snapshot["type"] == "snapshot"
            assert code in snapshot["online_codes"]
```

- [ ] **Step 2: 실행해서 실패 확인**

```bash
cd /home/cce/twomainProject/pc_careguard/server
.venv/bin/pytest tests/test_robot_ws.py -v
```
Expected: FAIL — `/ws/robot/...`, `/ws/dashboard` 라우트가 아직 없어 연결 자체가 안 됨

- [ ] **Step 3: robot_ws.py 구현**

`pc_careguard/server/app/robot_ws.py`:
```python
import asyncio
import json

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from . import db
from .robot_registry import broadcast_status, connected_robots, dashboard_sockets

router = APIRouter()

# 로봇이 5초마다 heartbeat를 보내는 것 기준, 3회분(15초) 안에 못 받으면 끊긴 것으로 본다.
HEARTBEAT_TIMEOUT = 15.0


async def _robot_registered(code: str) -> bool:
    def _query():
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT 1 FROM robot WHERE code = %s AND deregistered_at IS NULL", (code,))
            return cur.fetchone() is not None
    # psycopg2는 동기 라이브러리라 이벤트 루프를 막지 않게 스레드로 돌린다.
    return await asyncio.to_thread(_query)


@router.websocket("/ws/robot/{code}")
async def robot_socket(websocket: WebSocket, code: str):
    """터틀봇 careguard_turtlebot_agent/uplink_node.py가 접속하는 엔드포인트.

    등록 안 된 code는 즉시 거부한다 — 로봇은 먼저 대시보드에서 등록돼 있어야 한다.
    """
    if not await _robot_registered(code):
        await websocket.close(code=4404)
        return

    await websocket.accept()
    connected_robots.add(code)
    await broadcast_status(code, True)

    try:
        while True:
            await asyncio.wait_for(websocket.receive_text(), timeout=HEARTBEAT_TIMEOUT)
    except (WebSocketDisconnect, asyncio.TimeoutError):
        pass
    finally:
        connected_robots.discard(code)
        await broadcast_status(code, False)


@router.websocket("/ws/dashboard")
async def dashboard_socket(websocket: WebSocket):
    """대시보드 RobotWsClient가 접속하는 엔드포인트. 서버 -> 클라이언트 단방향 push."""
    await websocket.accept()
    dashboard_sockets.append(websocket)
    await websocket.send_text(json.dumps({"type": "snapshot", "online_codes": list(connected_robots)}))

    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        if websocket in dashboard_sockets:
            dashboard_sockets.remove(websocket)
```

- [ ] **Step 4: main.py에 라우터 등록**

`pc_careguard/server/app/main.py`, Task 2에서 만든 상태에 이어서:
```python
from .accounts import router as accounts_router
from .audit import router as audit_router
from .media import router as media_router
from .media import video_events_router
from .robot_ws import router as robot_ws_router
from .robots import router as robots_router
from .schemas import LoginRequest, LoginResponse

app = FastAPI(title="CareGuard Server")
app.include_router(media_router)
app.include_router(video_events_router)
app.include_router(accounts_router)
app.include_router(audit_router)
app.include_router(robots_router)
app.include_router(robot_ws_router)
```

- [ ] **Step 5: 테스트 통과 확인**

```bash
cd /home/cce/twomainProject/pc_careguard/server
.venv/bin/pytest tests/ -v
```
Expected: 이전 태스크 것 포함 전부 PASS

- [ ] **Step 6: 커밋**

```bash
git add pc_careguard/server/app/robot_ws.py pc_careguard/server/app/main.py \
        pc_careguard/server/tests/test_robot_ws.py
git commit -m "feat(server): 로봇·대시보드 WebSocket 연결 추가"
```

---

## Task 4: 로봇 에이전트 — ament_python 패키지 뼈대

**Files:**
- Create: `careguard_turtlebot_agent/package.xml`
- Create: `careguard_turtlebot_agent/setup.py`
- Create: `careguard_turtlebot_agent/setup.cfg`
- Create: `careguard_turtlebot_agent/resource/careguard_turtlebot_agent` (빈 파일)
- Create: `careguard_turtlebot_agent/careguard_turtlebot_agent/__init__.py` (빈 파일)
- Create: `careguard_turtlebot_agent/requirements.txt`

**Interfaces:**
- Produces: `colcon build`로 빌드되는 최상위 ROS2 ament_python 패키지 `careguard_turtlebot_agent`. entry point `uplink_node`(Task 6에서 실제 구현 채움 — 이 태스크에서는 파일이 없어도 `setup.py`에 entry_point 선언만 해둔다. `colcon build` 자체는 소스 파일 존재 여부를 안 따지므로 통과하고, `ros2 run`은 Task 6 이후에나 성공한다)

> 이 저장소에 이미 빈 디렉토리 3개(`careguard_turtlebot_agent/`, `careguard_turtlebot_agent/careguard_turtlebot_agent/`는 아니고 `launch/`, `resource/`, 그리고 `careguard_turtlebot_agent/careguard_turtlebot_agent/`)가 만들어져 있을 수 있다. 있으면 그대로 재사용하고, 없으면 새로 만든다.

- [ ] **Step 1: package.xml**

`careguard_turtlebot_agent/package.xml`:
```xml
<?xml version="1.0"?>
<?xml-model href="http://download.ros.org/schema/package_format3.xsd" schematypens="http://www.w3.org/2001/XMLSchema"?>
<package format="3">
  <name>careguard_turtlebot_agent</name>
  <version>0.0.1</version>
  <description>TurtleBot과 CareGuard 서버를 잇는 WebSocket 브리지 (uplink heartbeat)</description>
  <maintainer email="ajtwlscldjs@gmail.com">postwo</maintainer>
  <license>Apache-2.0</license>

  <depend>rclpy</depend>

  <test_depend>python3-pytest</test_depend>

  <export>
    <build_type>ament_python</build_type>
  </export>
</package>
```

- [ ] **Step 2: setup.py**

`careguard_turtlebot_agent/setup.py`:
```python
from setuptools import find_packages, setup

package_name = 'careguard_turtlebot_agent'

setup(
    name=package_name,
    version='0.0.1',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
        ('share/' + package_name + '/launch', ['launch/agent.launch.py']),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='postwo',
    maintainer_email='ajtwlscldjs@gmail.com',
    description='TurtleBot과 CareGuard 서버를 잇는 WebSocket 브리지',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'uplink_node = careguard_turtlebot_agent.uplink_node:main',
        ],
    },
)
```
(`launch/agent.launch.py`는 Task 6에서 만든다 — `data_files`에 미리 넣어둔다)

- [ ] **Step 3: setup.cfg**

`careguard_turtlebot_agent/setup.cfg`:
```ini
[develop]
script_dir=$base/lib/careguard_turtlebot_agent
[install]
install_scripts=$base/lib/careguard_turtlebot_agent
```

- [ ] **Step 4: 마커 파일들**

```bash
mkdir -p /home/cce/twomainProject/careguard_turtlebot_agent/resource
mkdir -p /home/cce/twomainProject/careguard_turtlebot_agent/careguard_turtlebot_agent
mkdir -p /home/cce/twomainProject/careguard_turtlebot_agent/launch
touch /home/cce/twomainProject/careguard_turtlebot_agent/resource/careguard_turtlebot_agent
touch /home/cce/twomainProject/careguard_turtlebot_agent/careguard_turtlebot_agent/__init__.py
```

- [ ] **Step 5: requirements.txt**

`careguard_turtlebot_agent/requirements.txt`:
```
websocket-client
```

- [ ] **Step 6: 빌드 확인 (이 시점엔 uplink_node.py가 없어 콘솔 스크립트 대상이 비어 있지만, colcon 빌드 자체는 통과해야 한다)**

```bash
mkdir -p /tmp/careguard_turtlebot_ws/src
ln -sfn /home/cce/twomainProject/careguard_turtlebot_agent /tmp/careguard_turtlebot_ws/src/careguard_turtlebot_agent
cd /tmp/careguard_turtlebot_ws
source /opt/ros/humble/setup.bash
colcon build --packages-select careguard_turtlebot_agent
```
Expected: `Finished <<< careguard_turtlebot_agent`. (임시 심볼릭 링크 워크스페이스는 실제 라즈베리파이 배포 방식과 동일한 패턴 — 스펙의 "라즈베리파이 설정" 절차와 같다)

- [ ] **Step 7: 커밋**

```bash
cd /home/cce/twomainProject
git add careguard_turtlebot_agent/package.xml careguard_turtlebot_agent/setup.py \
        careguard_turtlebot_agent/setup.cfg careguard_turtlebot_agent/requirements.txt \
        careguard_turtlebot_agent/resource/careguard_turtlebot_agent \
        careguard_turtlebot_agent/careguard_turtlebot_agent/__init__.py
git commit -m "feat(agent): careguard_turtlebot_agent ROS2 패키지 뼈대 추가"
```

---

## Task 5: 로봇 에이전트 — `HeartbeatWorker` (재연결·백오프 로직, TDD)

**Files:**
- Create: `careguard_turtlebot_agent/careguard_turtlebot_agent/heartbeat_worker.py`
- Test: `careguard_turtlebot_agent/test/test_heartbeat_worker.py`

**Interfaces:**
- Consumes: 없음 (순수 파이썬, rclpy·실제 네트워크 의존 없음 — 그래서 라즈베리파이 없이도 이 개발 PC에서 바로 테스트 가능)
- Produces: `HeartbeatWorker(url, connect_fn, sleep_fn, interval=5.0, max_backoff=30.0)` 클래스.
  - `.run()`: `connect_fn(url, timeout=5)`로 연결 시도 → 성공하면 `interval`마다 `ws.send('{"type": "heartbeat"}')` 반복, 실패하면 `sleep_fn(backoff)` 후 backoff를 2배(최대 `max_backoff`)로 늘려 재시도. `_stop_event`(threading.Event)가 set되면 루프 종료
  - `.stop()`: 실행 중인 `run()` 루프를 다음 체크포인트에서 멈추게 함
  - Task 6이 `connect_fn=websocket.create_connection`, `sleep_fn=time.sleep`로 실제 인스턴스를 만들어 별도 스레드에서 `.run()`을 돌린다

- [ ] **Step 1: 실패하는 테스트 작성**

`careguard_turtlebot_agent/test/test_heartbeat_worker.py`:
```python
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from careguard_turtlebot_agent.heartbeat_worker import HeartbeatWorker


class FakeWs:
    def __init__(self):
        self.sent = []

    def send(self, msg):
        self.sent.append(msg)


def test_sends_heartbeat_every_interval_then_stops():
    fake_ws = FakeWs()
    calls = {"connect": 0, "sleep_args": []}

    def connect_fn(url, timeout):
        calls["connect"] += 1
        return fake_ws

    worker = HeartbeatWorker("ws://fake", connect_fn=connect_fn, sleep_fn=None, interval=5.0)

    def fake_sleep(seconds):
        calls["sleep_args"].append(seconds)
        if len(calls["sleep_args"]) >= 3:
            worker.stop()

    worker._sleep_fn = fake_sleep
    worker.run()

    assert calls["connect"] == 1
    assert fake_ws.sent == ['{"type": "heartbeat"}'] * 3
    assert calls["sleep_args"] == [5.0, 5.0, 5.0]


def test_reconnects_with_exponential_backoff_on_failure():
    attempts = {"n": 0}
    sleeps = []

    def connect_fn(url, timeout):
        attempts["n"] += 1
        if attempts["n"] < 3:
            raise ConnectionRefusedError("no server")
        return FakeWs()

    worker = HeartbeatWorker("ws://fake", connect_fn=connect_fn, sleep_fn=None, interval=5.0)

    def fake_sleep(seconds):
        sleeps.append(seconds)
        if attempts["n"] >= 3:
            worker.stop()

    worker._sleep_fn = fake_sleep
    worker.run()

    assert attempts["n"] == 3
    # 1·2차 연결 실패 -> backoff 1.0, 2.0으로 대기. 3차 성공 후 heartbeat 루프에서
    # interval(5.0)만큼 한 번 더 sleep 호출된 뒤 멈춘다.
    assert sleeps == [1.0, 2.0, 5.0]


def test_backoff_caps_at_max_backoff():
    sleeps = []

    def connect_fn(url, timeout):
        raise ConnectionRefusedError("always down")

    worker = HeartbeatWorker("ws://fake", connect_fn=connect_fn, sleep_fn=None, interval=5.0, max_backoff=30.0)

    def fake_sleep(seconds):
        sleeps.append(seconds)
        if len(sleeps) >= 7:
            worker.stop()

    worker._sleep_fn = fake_sleep
    worker.run()

    assert sleeps == [1.0, 2.0, 4.0, 8.0, 16.0, 30.0, 30.0]
```

- [ ] **Step 2: 실행해서 실패 확인**

```bash
cd /home/cce/twomainProject/careguard_turtlebot_agent
python3 -m pytest test/test_heartbeat_worker.py -v
```
Expected: FAIL — `heartbeat_worker` 모듈이 아직 없음

- [ ] **Step 3: 구현**

`careguard_turtlebot_agent/careguard_turtlebot_agent/heartbeat_worker.py`:
```python
import threading


class HeartbeatWorker:
    """WebSocket으로 서버에 접속해 주기적으로 heartbeat를 보낸다.

    연결이 끊기면 지수 백오프(최대 max_backoff초)로 재연결을 시도한다.
    connect_fn/sleep_fn을 주입받는 이유는 순수 로직만 테스트하기 위함 —
    실제 사용 시(uplink_node.py)엔 websocket.create_connection/time.sleep을 넘긴다.
    """

    def __init__(self, url, connect_fn, sleep_fn, interval=5.0, max_backoff=30.0):
        self._url = url
        self._connect_fn = connect_fn
        self._sleep_fn = sleep_fn
        self._interval = interval
        self._max_backoff = max_backoff
        self._stop_event = threading.Event()

    def stop(self):
        self._stop_event.set()

    def run(self):
        backoff = 1.0
        while not self._stop_event.is_set():
            try:
                ws = self._connect_fn(self._url, timeout=5)
                backoff = 1.0
                while not self._stop_event.is_set():
                    ws.send('{"type": "heartbeat"}')
                    self._sleep_fn(self._interval)
            except Exception:
                self._sleep_fn(backoff)
                backoff = min(backoff * 2, self._max_backoff)
```

- [ ] **Step 4: 테스트 통과 확인**

```bash
cd /home/cce/twomainProject/careguard_turtlebot_agent
python3 -m pytest test/test_heartbeat_worker.py -v
```
Expected: 3개 테스트 모두 PASS

- [ ] **Step 5: 커밋**

```bash
cd /home/cce/twomainProject
git add careguard_turtlebot_agent/careguard_turtlebot_agent/heartbeat_worker.py \
        careguard_turtlebot_agent/test/test_heartbeat_worker.py
git commit -m "feat(agent): heartbeat 재연결·백오프 로직 추가"
```

---

## Task 6: 로봇 에이전트 — `uplink_node.py` + launch 파일

**Files:**
- Create: `careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py`
- Create: `careguard_turtlebot_agent/launch/agent.launch.py`

**Interfaces:**
- Consumes: Task 5의 `HeartbeatWorker`
- Produces: `ros2 run careguard_turtlebot_agent uplink_node --ros-args -p robot_code:=R-05 -p server_host:=<IP> [-p server_port:=8000]`로 실행되는 노드. 파라미터 `robot_code`/`server_host`(필수, 기본값 없으면 노드가 에러 로그를 남기고 값 없이는 동작 안 함)/`server_port`(기본 8000)

> 이 태스크는 실제 하드웨어(라즈베리파이) 없이 이 개발 PC에서도 `rclpy`가 설치돼 있어 노드 생성 자체는 검증 가능하지만, "서버에 실제로 붙는지"는 Task 3의 서버가 로컬에 떠 있어야 확인 가능하다 — 아래 Step 4가 그 수동 확인이다. 자동 테스트는 만들지 않는다(네트워크·프로세스 수명주기가 얽혀 있어 TDD로 다루기보다 수동 확인이 더 정직함 — Task 5에서 로직은 이미 단위 테스트로 커버됨).

- [ ] **Step 1: uplink_node.py 작성**

`careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py`:
```python
import threading

import rclpy
from rclpy.node import Node
from websocket import create_connection

from .heartbeat_worker import HeartbeatWorker


class UplinkNode(Node):
    def __init__(self):
        super().__init__('uplink_node')

        self.declare_parameter('robot_code', '')
        self.declare_parameter('server_host', '')
        self.declare_parameter('server_port', 8000)

        robot_code = self.get_parameter('robot_code').value
        server_host = self.get_parameter('server_host').value
        server_port = self.get_parameter('server_port').value

        if not robot_code or not server_host:
            self.get_logger().error(
                "robot_code, server_host 파라미터가 필요합니다. 예: "
                "--ros-args -p robot_code:=R-05 -p server_host:=192.168.0.9")
            raise SystemExit(1)

        url = f"ws://{server_host}:{server_port}/ws/robot/{robot_code}"
        self._worker = HeartbeatWorker(url, connect_fn=create_connection, sleep_fn=_sleep)
        self._thread = threading.Thread(target=self._worker.run, daemon=True)
        self._thread.start()
        self.get_logger().info(f"uplink 시작: {url}")

    def destroy_node(self):
        self._worker.stop()
        super().destroy_node()


def _sleep(seconds):
    import time
    time.sleep(seconds)


def main():
    rclpy.init()
    node = UplinkNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
```

- [ ] **Step 2: launch 파일 작성**

`careguard_turtlebot_agent/launch/agent.launch.py`:
```python
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument('robot_code', description='대시보드에 등록한 로봇 코드, 예: R-05'),
        DeclareLaunchArgument('server_host', description='CareGuard 서버 IP'),
        DeclareLaunchArgument('server_port', default_value='8000'),
        Node(
            package='careguard_turtlebot_agent',
            executable='uplink_node',
            name='uplink_node',
            parameters=[{
                'robot_code': LaunchConfiguration('robot_code'),
                'server_host': LaunchConfiguration('server_host'),
                'server_port': LaunchConfiguration('server_port'),
            }],
        ),
    ])
```

- [ ] **Step 3: 빌드 + import 확인**

```bash
python3 -c "import websocket" || pip install --user websocket-client
cd /tmp/careguard_turtlebot_ws
source /opt/ros/humble/setup.bash
colcon build --packages-select careguard_turtlebot_agent
source install/setup.bash
ros2 run careguard_turtlebot_agent uplink_node --ros-args -p robot_code:=TEST -p server_host:=127.0.0.1 &
sleep 2
kill %1 2>/dev/null
```
Expected: `colcon build` 성공, 노드가 에러 없이 뜨고(로그에 "uplink 시작: ws://127.0.0.1:8000/ws/robot/TEST") kill로 정상 종료됨. 이 시점엔 서버(Task 3)가 안 떠 있어도 되고, 안 떠 있으면 `HeartbeatWorker`가 그냥 백오프 재시도를 반복하는 로그만 남긴다(에러로 죽지 않음).

- [ ] **Step 4: (선택, 수동) 실제 서버와 붙는지 확인**

```bash
# 터미널 1
cd /home/cce/twomainProject/pc_careguard/server
.venv/bin/uvicorn app.main:app --reload

# 터미널 2 — 먼저 대시보드/curl로 로봇을 등록해야 함
curl -X POST http://127.0.0.1:8000/robots \
  -H "Content-Type: application/json" \
  -d '{"code":"R-TEST","name":"수동확인용","model":"turtlebot","ros_domain_id":30}'

# 터미널 3
cd /tmp/careguard_turtlebot_ws && source install/setup.bash
ros2 run careguard_turtlebot_agent uplink_node --ros-args -p robot_code:=R-TEST -p server_host:=127.0.0.1

# 터미널 4
curl http://127.0.0.1:8000/robots   # is_online: true 확인
```
확인 후 터미널 3의 노드를 Ctrl+C로 끄고, `curl http://127.0.0.1:8000/robots`로 15초 이내 `is_online: false`로 바뀌는지 확인. 확인이 끝나면 `curl -X DELETE`가 없으므로 테스트용 `R-TEST` row는 DB에서 수동으로 지운다(`psql`로 `DELETE FROM robot WHERE code = 'R-TEST';` 또는 다음 태스크 진행에 지장 없으니 남겨둬도 무방).

- [ ] **Step 5: 커밋**

```bash
cd /home/cce/twomainProject
git add careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py \
        careguard_turtlebot_agent/launch/agent.launch.py
git commit -m "feat(agent): uplink_node ROS2 노드 추가"
```

---

## Task 7: 대시보드 — `Qt6::WebSockets` + `RobotWsClient`

**Files:**
- Modify: `pc_careguard/dashboard/CMakeLists.txt:11` (COMPONENTS에 `WebSockets` 추가), `:13-90`(소스 목록에 신규 파일 추가), `:93`(`target_link_libraries`에 `Qt6::WebSockets` 추가)
- Create: `pc_careguard/dashboard/src/net/robot_ws_client.h`
- Create: `pc_careguard/dashboard/src/net/robot_ws_client.cpp`

**Interfaces:**
- Produces: `RobotWsClient(const QString& serverUrl, QObject* parent)` — `.start()`로 접속 시작. 시그널 `snapshotReceived(const QStringList& onlineCodes)`, `robotStatusChanged(const QString& code, bool online)`
- Task 8이 이 클래스를 `P7SettingsPage`에서 생성해서 시그널을 받는다

이 프로젝트엔 C++ 자동 테스트 프레임워크가 없다(Global Constraints 참고) — 이 태스크는 **빌드 성공**으로 검증한다.

- [ ] **Step 1: CMakeLists.txt 수정 — Qt6 컴포넌트**

`pc_careguard/dashboard/CMakeLists.txt:11`, 기존:
```cmake
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets SvgWidgets Multimedia MultimediaWidgets Network)
```
다음으로 교체:
```cmake
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets SvgWidgets Multimedia MultimediaWidgets Network WebSockets)
```

- [ ] **Step 2: CMakeLists.txt 수정 — 소스 목록에 추가**

같은 파일, `src/widgets/checkable_list_widget.cpp`(63-64행 부근) 다음 줄에 삽입:
```cmake
    src/widgets/checkable_list_widget.h
    src/widgets/checkable_list_widget.cpp
    src/net/robot_ws_client.h
    src/net/robot_ws_client.cpp
    src/dialogs/event_detail_modal.h
```
(마지막 줄 `src/dialogs/event_detail_modal.h`는 원래 있던 다음 줄 — 그 위에 두 줄만 끼워 넣는다는 뜻)

- [ ] **Step 3: CMakeLists.txt 수정 — 링크**

`pc_careguard/dashboard/CMakeLists.txt:93`, 기존:
```cmake
target_link_libraries(careguard_dashboard PRIVATE Qt6::Widgets Qt6::SvgWidgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Network)
```
다음으로 교체:
```cmake
target_link_libraries(careguard_dashboard PRIVATE Qt6::Widgets Qt6::SvgWidgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Network Qt6::WebSockets)
```

- [ ] **Step 4: robot_ws_client.h 작성**

`pc_careguard/dashboard/src/net/robot_ws_client.h`:
```cpp
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QWebSocket;
class QTimer;

// /ws/dashboard에 상시 접속해 로봇 온라인/오프라인 상태를 실시간으로 받는다.
// 폴링 없음 — 서버가 상태 변화 있을 때만 push한다.
class RobotWsClient : public QObject {
    Q_OBJECT
public:
    explicit RobotWsClient(const QString& serverUrl, QObject* parent = nullptr);
    void start();

signals:
    void snapshotReceived(const QStringList& onlineCodes);
    void robotStatusChanged(const QString& code, bool online);

private:
    void connectSocket();
    void handleTextMessage(const QString& message);

    QWebSocket* socket_;
    QTimer* reconnectTimer_;
    QUrl wsUrl_;
};
```

- [ ] **Step 5: robot_ws_client.cpp 작성**

`pc_careguard/dashboard/src/net/robot_ws_client.cpp`:
```cpp
#include "robot_ws_client.h"

#include <QAbstractSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QWebSocket>

namespace {
constexpr int kReconnectIntervalMs = 3000;
}

RobotWsClient::RobotWsClient(const QString& serverUrl, QObject* parent)
    : QObject(parent), socket_(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this)),
      reconnectTimer_(new QTimer(this)) {
    QUrl url(serverUrl);
    url.setScheme(url.scheme() == QStringLiteral("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
    url.setPath(QStringLiteral("/ws/dashboard"));
    wsUrl_ = url;

    reconnectTimer_->setInterval(kReconnectIntervalMs);
    connect(reconnectTimer_, &QTimer::timeout, this, &RobotWsClient::connectSocket);
    connect(socket_, &QWebSocket::textMessageReceived, this, &RobotWsClient::handleTextMessage);
}

void RobotWsClient::start() {
    connectSocket();
    reconnectTimer_->start();
}

void RobotWsClient::connectSocket() {
    if (socket_->state() == QAbstractSocket::UnconnectedState) {
        socket_->open(wsUrl_);
    }
}

void RobotWsClient::handleTextMessage(const QString& message) {
    const QJsonObject obj = QJsonDocument::fromJson(message.toUtf8()).object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("snapshot")) {
        QStringList codes;
        for (const QJsonValue& v : obj.value(QStringLiteral("online_codes")).toArray()) {
            codes << v.toString();
        }
        emit snapshotReceived(codes);
    } else if (type == QStringLiteral("robot_status")) {
        emit robotStatusChanged(obj.value(QStringLiteral("code")).toString(),
                                 obj.value(QStringLiteral("online")).toBool());
    }
}
```

- [ ] **Step 6: 빌드 확인**

```bash
cd /home/cce/twomainProject/pc_careguard/dashboard/build
cmake -DCMAKE_PREFIX_PATH=/home/cce/Qt/6.11.1/gcc_64 .. 2>&1 | tail -20
cmake --build . -j"$(nproc)" 2>&1 | tail -40
```
Expected: `Built target careguard_dashboard` (아직 `RobotWsClient`를 아무도 안 쓰므로 링크 경고 없이 그냥 빌드만 성공하면 됨 — 실제 연동은 Task 8)

- [ ] **Step 7: 커밋**

```bash
cd /home/cce/twomainProject
git add pc_careguard/dashboard/CMakeLists.txt pc_careguard/dashboard/src/net/robot_ws_client.h \
        pc_careguard/dashboard/src/net/robot_ws_client.cpp
git commit -m "feat(dashboard): Qt6 WebSockets 기반 RobotWsClient 추가"
```

---

## Task 8: 대시보드 — `P7SettingsPage` 실제 연동

**Files:**
- Modify: `pc_careguard/dashboard/src/pages/p7_settings_page.h`
- Modify: `pc_careguard/dashboard/src/pages/p7_settings_page.cpp`

**Interfaces:**
- Consumes: Task 2/3의 서버 `GET/POST /robots`, `/ws/dashboard`; Task 7의 `RobotWsClient`
- Produces: 로봇 관리 탭이 하드코딩 목업 대신 실제 서버 데이터로 채워지고, 등록 버튼이 실제로 DB에 저장하며, 온라인/오프라인이 WebSocket push로 실시간 갱신됨

이것도 C++ — **빌드 성공 + 수동 스모크 테스트**로 검증한다(Global Constraints 참고).

- [ ] **Step 1: 헤더에 선언 추가**

`pc_careguard/dashboard/src/pages/p7_settings_page.h`:

`class LiveSearchLineEdit;` 다음 줄(17행)에 전방 선언 추가:
```cpp
class LiveSearchLineEdit;
class RobotWsClient;
```

`void openRobotDialog(int rowIndex);  // -1이면 신규 등록` 다음(100행) 근처, `void populateRobotRow(int r);`/`void refreshRobotTable();` 아래에 메서드 선언 추가:
```cpp
    void openRobotDialog(int rowIndex);  // -1이면 신규 등록
    void populateRobotRow(int r);
    void refreshRobotTable();
    void fetchRobots();  // GET /robots
    void handleRobotsReply(QNetworkReply* reply);
    void registerRobot(const RobotDialog::RobotData& data);  // POST /robots
    void handleRobotsSnapshot(const QStringList& onlineCodes);
    void handleRobotStatusChanged(const QString& code, bool online);
```
(`RobotDialog::RobotData`를 쓰려면 `p7_settings_page.h` 상단에 `#include "dialogs/p7_robot_dialog.h"`가 필요 — 기존엔 `p7_account_dialog.h`만 include돼 있었다. `#include "dialogs/p7_account_dialog.h"` 다음 줄에 추가:)
```cpp
#include "dialogs/p7_account_dialog.h"
#include "dialogs/p7_robot_dialog.h"
#include "theme/tokens.h"
```

`QPushButton* deleteAccountBtn_ = nullptr;` 다음 줄(멤버 변수 영역)에 추가:
```cpp
    QPushButton* deleteAccountBtn_ = nullptr;
    RobotWsClient* robotWsClient_ = nullptr;
```

- [ ] **Step 2: 생성자 — 하드코딩 시드 제거, 실 연동 추가**

`pc_careguard/dashboard/src/pages/p7_settings_page.cpp`, 기존(168-188행):
```cpp
P7SettingsPage::P7SettingsPage(QWidget* parent) : QWidget(parent) {
    robots_ = {
        {QStringLiteral("R-01"), QStringLiteral("192.168.0.11"), QStringLiteral("/limo_01"),
         QStringLiteral("행복빌라 302 · 김순자"), Tokens::Level::Normal, QStringLiteral("● 온라인"),
         QStringLiteral("6/6 정상"), QStringLiteral("1호기"), QStringLiteral("limo"), 41,
         QStringLiteral("김순자 (C-001)"), QStringLiteral("192.168.0.9"),
         QStringLiteral("최근 ping 응답 0.6s 전 · 세션 5d 1h")},
        {QStringLiteral("R-02"), QStringLiteral("192.168.0.12"), QStringLiteral("/limo_02"),
         QStringLiteral("은빛아파트 101 · 박영호"), Tokens::Level::Normal, QStringLiteral("● 온라인"),
         QStringLiteral("6/6 정상"), QStringLiteral("2호기"), QStringLiteral("limo"), 42,
         QStringLiteral("박영호 (C-002)"), QStringLiteral("192.168.0.9"),
         QStringLiteral("최근 ping 응답 0.5s 전 · 세션 4d 3h")},
        {QStringLiteral("R-03"), QStringLiteral("192.168.0.13"), QStringLiteral("/limo_03"),
         QStringLiteral("솔마을 7호 · 이말순"), Tokens::Level::Attention, QStringLiteral("● 온라인"),
         QStringLiteral("5/6 · voice 응답 지연"), QStringLiteral("3호기"), QStringLiteral("limo"), 43,
         QStringLiteral("이말순 (C-003)"), QStringLiteral("192.168.0.9"),
         QStringLiteral("최근 ping 응답 0.4s 전 · 세션 3d 2h")},
        {QStringLiteral("R-04"), QStringLiteral("192.168.0.14"), QStringLiteral("/limo_04"),
         QStringLiteral("동산주택 2층 · 최금례"), Tokens::Level::Offline, QStringLiteral("오프라인"),
         Tokens::Format::EMPTY, QStringLiteral("4호기"), QStringLiteral("limo"), 44,
         QStringLiteral("최금례 (C-004)"), QStringLiteral("192.168.0.9"), QString()},
    };

    r03Nodes_ = {
```
다음으로 교체 (`robots_ = {...}` 블록만 통째로 삭제하고, `r03Nodes_`는 그대로 둔다):
```cpp
P7SettingsPage::P7SettingsPage(QWidget* parent) : QWidget(parent) {
    // 로봇 목록은 하드코딩하지 않고 서버 DB에서 불러온다 (fetchRobots(), 생성자 마지막에서 호출).

    r03Nodes_ = {
```

- [ ] **Step 3: 생성자 끝 — fetchRobots + RobotWsClient 기동**

`pc_careguard/dashboard/src/pages/p7_settings_page.cpp`, 기존(생성자 마지막 부분):
```cpp
    netManager_ = new QNetworkAccessManager(this);
    fetchAccounts();
    fetchRecipients();
    fetchAuditLog();
}
```
다음으로 교체:
```cpp
    netManager_ = new QNetworkAccessManager(this);
    fetchAccounts();
    fetchRecipients();
    fetchAuditLog();
    fetchRobots();

    robotWsClient_ = new RobotWsClient(serverUrl_, this);
    connect(robotWsClient_, &RobotWsClient::snapshotReceived, this, &P7SettingsPage::handleRobotsSnapshot);
    connect(robotWsClient_, &RobotWsClient::robotStatusChanged, this, &P7SettingsPage::handleRobotStatusChanged);
    robotWsClient_->start();
}
```
`#include "net/robot_ws_client.h"`를 cpp 파일 상단 include 목록에 추가 (`#include "dialogs/p7_robot_dialog.h"` 다음 줄):
```cpp
#include "dialogs/p7_robot_dialog.h"
#include "net/robot_ws_client.h"
#include "theme/calendar_style.h"
```

- [ ] **Step 4: 등록 시 실제 API 호출로 교체**

`pc_careguard/dashboard/src/pages/p7_settings_page.cpp`, `openRobotDialog` 안의 기존(840-866행):
```cpp
    connect(dialog, &RobotDialog::robotSaved, this,
            [this, rowIndex, isNew, previousRecipient](const RobotDialog::RobotData& saved) {
        RobotRow row;
        if (isNew) {
            row.id = saved.code;
            row.ip = QStringLiteral("—");
            row.ns = QStringLiteral("/limo_%1").arg(saved.code.mid(2));
            row.level = Tokens::Level::Offline;
            row.statusDetail = QStringLiteral("오프라인");
            row.nodeSummary = Tokens::Format::EMPTY;
        } else {
            row = robots_[static_cast<size_t>(rowIndex)];
        }
        row.name = saved.name;
        row.model = saved.model;
        row.rosDomainId = saved.rosDomainId;
        row.recipientLabel = saved.recipientLabel;
        if (isNew || saved.recipientLabel != previousRecipient) {
            row.mapRecipient = saved.recipientLabel;
        }
        if (isNew) {
            robots_.push_back(row);
        } else {
            robots_[static_cast<size_t>(rowIndex)] = row;
        }
        refreshRobotTable();
    });
```
다음으로 교체 (신규 등록만 서버로 보내고, 기존 로봇 수정은 이번 범위 밖이라 기존 로컬 처리 그대로 둔다):
```cpp
    connect(dialog, &RobotDialog::robotSaved, this,
            [this, rowIndex, isNew, previousRecipient](const RobotDialog::RobotData& saved) {
        if (isNew) {
            // 신규 등록은 서버(DB)에 저장하고, 응답 후 목록을 다시 불러와 반영한다.
            registerRobot(saved);
            return;
        }
        // 기존 로봇 수정은 이번 범위 밖 — 지금까지처럼 로컬 표시만 갱신한다.
        RobotRow row = robots_[static_cast<size_t>(rowIndex)];
        row.name = saved.name;
        row.model = saved.model;
        row.rosDomainId = saved.rosDomainId;
        row.recipientLabel = saved.recipientLabel;
        if (saved.recipientLabel != previousRecipient) {
            row.mapRecipient = saved.recipientLabel;
        }
        robots_[static_cast<size_t>(rowIndex)] = row;
        refreshRobotTable();
    });
```

- [ ] **Step 5: fetchRobots/handleRobotsReply/registerRobot/스냅샷·상태 핸들러 구현**

`pc_careguard/dashboard/src/pages/p7_settings_page.cpp`의 `refreshRobotTable()` 함수(기존 783-791행) 바로 뒤에 추가:
```cpp
void P7SettingsPage::fetchRobots() {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/robots")));
    QNetworkReply* reply = netManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleRobotsReply(reply);
        reply->deleteLater();
    });
}

void P7SettingsPage::handleRobotsReply(QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        robots_.clear();
        if (robotTable_ != nullptr) refreshRobotTable();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    std::vector<RobotRow> rows;
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject obj = value.toObject();
        RobotRow row;
        row.id = obj.value("code").toString();
        row.name = obj.value("name").toString();
        row.model = obj.value("model").toString();
        row.rosDomainId = obj.value("ros_domain_id").toInt(0);
        const bool online = obj.value("is_online").toBool();
        row.level = online ? Tokens::Level::Normal : Tokens::Level::Offline;
        row.statusDetail = online ? QStringLiteral("● 온라인") : QStringLiteral("오프라인");
        rows.push_back(row);
    }
    robots_ = rows;
    if (robotTable_ != nullptr) refreshRobotTable();
}

void P7SettingsPage::registerRobot(const RobotDialog::RobotData& data) {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/robots")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{
        {"code", data.code},
        {"name", data.name},
        {"model", data.model},
        {"ros_domain_id", data.rosDomainId},
    };
    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200 && statusCode != 201) {
            const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
            const QString detail = err.value("detail").toString();
            reply->deleteLater();
            QMessageBox::warning(this, QStringLiteral("로봇 등록 실패"),
                                  detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
            return;
        }
        reply->deleteLater();
        fetchRobots();
    });
}

void P7SettingsPage::handleRobotsSnapshot(const QStringList& onlineCodes) {
    for (auto& row : robots_) {
        const bool online = onlineCodes.contains(row.id);
        row.level = online ? Tokens::Level::Normal : Tokens::Level::Offline;
        row.statusDetail = online ? QStringLiteral("● 온라인") : QStringLiteral("오프라인");
    }
    if (robotTable_ != nullptr) refreshRobotTable();
}

void P7SettingsPage::handleRobotStatusChanged(const QString& code, bool online) {
    for (int r = 0; r < static_cast<int>(robots_.size()); ++r) {
        if (robots_[static_cast<size_t>(r)].id == code) {
            robots_[static_cast<size_t>(r)].level = online ? Tokens::Level::Normal : Tokens::Level::Offline;
            robots_[static_cast<size_t>(r)].statusDetail =
                online ? QStringLiteral("● 온라인") : QStringLiteral("오프라인");
            if (robotTable_ != nullptr) populateRobotRow(r);
            break;
        }
    }
}
```

- [ ] **Step 6: 빌드**

```bash
cd /home/cce/twomainProject/pc_careguard/dashboard/build
cmake -DCMAKE_PREFIX_PATH=/home/cce/Qt/6.11.1/gcc_64 .. 2>&1 | tail -20
cmake --build . -j"$(nproc)" 2>&1 | tail -60
```
Expected: `Built target careguard_dashboard`

- [ ] **Step 7: 수동 스모크 테스트**

```bash
# 터미널 1: 서버
docker compose -f /home/cce/twomainProject/deploy/docker-compose.yml up -d postgres
cd /home/cce/twomainProject/pc_careguard/server && .venv/bin/uvicorn app.main:app --reload

# 터미널 2: 대시보드 실행
/home/cce/twomainProject/pc_careguard/dashboard/build/careguard_dashboard
```
확인 항목:
1. "로봇 관리" 탭에 로봇 목록이 뜬다(처음엔 비어있거나 이전 태스크에서 만든 테스트 로봇들이 남아있을 수 있음)
2. "로봇 등록" → 모델 `turtlebot`, 코드/이름/ROS_DOMAIN_ID 입력 후 저장 → 목록에 나타나고 "상태"가 오프라인
3. 터미널 3에서 Task 6의 방식으로 같은 코드로 `uplink_node` 실행 → 대시보드가 몇 초 안에(폴링 없이, push로) "온라인"으로 자동 갱신되는지 확인
4. `uplink_node`를 Ctrl+C로 종료 → 15초 안에 대시보드가 "오프라인"으로 자동 갱신되는지 확인

- [ ] **Step 8: 커밋**

```bash
cd /home/cce/twomainProject
git add pc_careguard/dashboard/src/pages/p7_settings_page.h pc_careguard/dashboard/src/pages/p7_settings_page.cpp
git commit -m "feat(dashboard): 로봇 관리 탭을 실제 서버·WebSocket과 연동"
```

---

## Task 9: 문서 — `CONVENTIONS.md` 갱신

**Files:**
- Modify: `CONVENTIONS.md` (5절 "리포 구조" 표)

**Interfaces:** 없음 (문서 전용)

- [ ] **Step 1: 5절 표에 행 추가**

`CONVENTIONS.md`의 "## 5. 리포 구조 [확정]" 표, 기존:
```markdown
| 경로 | 내용 | 실행 위치 |
|---|---|---|
| `robot_careguard_ws/` | ROS2 워크스페이스 전체 | LIMO 로봇 (`deploy/sync_to_limo.sh`로 rsync 배포) |
| `pc_careguard/` | FastAPI 서버, Qt 대시보드, DB 스키마 | 관제 PC |
| `shared/api_contract/` | OpenAPI 스펙, 이벤트 코드 정의 | 양쪽이 참조만 함 |
| `docs/` | 시나리오서, 와이어프레임, 상세 설계 문서 | - |
| `deploy/` | `docker-compose.yml`, `sync_to_limo.sh` | 관제 PC에서 실행 |
```
다음으로 교체:
```markdown
| 경로 | 내용 | 실행 위치 |
|---|---|---|
| `robot_careguard_ws/` | ROS2 워크스페이스 전체 | LIMO 로봇 (`deploy/sync_to_limo.sh`로 rsync 배포) |
| `careguard_turtlebot_agent/` | TurtleBot ↔ 서버 WebSocket 브리지 (ROS2 ament_python 패키지, `src/` 없이 최상위) | TurtleBot 라즈베리파이 (직접 git pull) |
| `pc_careguard/` | FastAPI 서버, Qt 대시보드, DB 스키마 | 관제 PC |
| `shared/api_contract/` | OpenAPI 스펙, 이벤트 코드 정의 | 양쪽이 참조만 함 |
| `docs/` | 시나리오서, 와이어프레임, 상세 설계 문서 | - |
| `deploy/` | `docker-compose.yml`, `sync_to_limo.sh` | 관제 PC에서 실행 |
```

- [ ] **Step 2: 커밋**

```bash
cd /home/cce/twomainProject
git add CONVENTIONS.md
git commit -m "docs(docs): careguard_turtlebot_agent 최상위 폴더 규칙 반영"
```

---

## 이 계획에 없는 것 (스펙의 "제외" 그대로)

- 미션 명령, 이벤트 업로드, 영상/스냅샷 전송
- 로봇 IP·네임스페이스·노드 상태·담당 대상자 배정을 실제 데이터로 채우는 것
- LIMO 로봇(`robot_careguard_ws`) 관련 변경
- 기존 로봇 정보 수정(PATCH) API — 등록(POST)만 실 서버 연동
