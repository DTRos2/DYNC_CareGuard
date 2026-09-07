# 터틀봇 ↔ 서버 ↔ 대시보드 실시간 연결 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 터틀봇을 켜면 서버에 WebSocket으로 실제 연결되고, 대시보드 "로봇 관리" 탭에 온라인/오프라인 상태가 폴링 없이 실시간으로 반영되게 만든다.

**Architecture (2026-09-07 Task 2 리뷰 중 수정됨 — 아래 "PIVOT" 참고):** REST 등록은 `GET`/`POST /robots`(Task 2, 완료). 실시간 온라인/오프라인은 **이미 `origin/develop`에 병합돼 있던 LIMO용 `/ws/robot?robot_code=`(`pc_careguard/server/app/ws/robot_ws.py`) + `/ws/dashboard`(`app/ws/dashboard_ws.py`)를 그대로 재사용**한다 — 새 WebSocket 엔드포인트를 서버에 만들지 않는다. 로봇 쪽은 `robot_careguard_ws/src/careguard_agent`(LIMO용, 이미 존재)와 같은 연결 패턴을 새 최상위 폴더 `careguard_turtlebot_agent/`에 이식한다. 대시보드도 이미 존재하는 `pc_careguard/dashboard/src/net/ws_client.{h,cpp}`(`WsClient`)를 그대로 재사용한다 — 새 WebSocket 클라이언트 클래스를 만들지 않는다.

**PIVOT (2026-09-07):** 이 계획은 원래 `feature/p7-log-tab` 기준으로 설계됐다. 실행 중(Task 2 리뷰) `origin/develop`에 이미 PR #24(`feature/p5-p6-map-patrol-pages`)로 로봇 WebSocket 연동 전체가 들어와 있다는 걸 발견했다 — 서버(`app/ws/robot_ws.py`, `app/ws/dashboard_ws.py`, 둘 다 `main.py`에 이미 등록됨), 로봇 에이전트(`robot_careguard_ws/src/careguard_agent/uplink_node.py` 등, `/map` 구독은 2026-09-04 실제 LIMO 하드웨어로 검증됨), 대시보드(`ws_client.{h,cpp}`, P5/P6 페이지가 이미 사용 중)까지 전부. 사용자 확인 결과 "기존 코드 재사용 + 터틀봇 전용 폴더는 최상단에 별도로" 방향으로 확정. 원래 Task 3(서버 WS 신규 엔드포인트)·Task 4~6(자체 HeartbeatWorker 설계)·Task 7(신규 RobotWsClient)은 전부 기존 시스템과 충돌하거나 중복이라 **Task 3을 기존 시스템 재사용 버전으로 갈아엎고, Task 4/5/6/7은 스킵, Task 8은 기존 `WsClient` 재사용으로 갈아엎었다.** 아래 각 태스크 제목에 표시.

**Tech Stack:** FastAPI + psycopg2 (서버, 기존과 동일), ROS2 Humble ament_python + `websockets`(async, LIMO agent와 동일 라이브러리) (로봇 에이전트), Qt6 Widgets + 기존 `Qt6::WebSockets`/`WsClient` (대시보드 — 새로 추가하는 것 없음).

**Spec:** `docs/superpowers/specs/2026-09-07-turtlebot-realtime-connectivity-design.md` (PIVOT 이후 서버/대시보드 WS 부분은 이 계획 파일이 스펙보다 최신 — 아래 각 태스크 본문이 우선한다)

## Global Constraints

- 대시보드는 실시간 상태에 REST 폴링을 쓰지 않는다 — WebSocket push만 사용한다 (스펙 "아키텍처")
- 온라인 판정은 서버 프로세스 메모리로만 하고 DB 스키마는 변경하지 않는다 (스펙 "아키텍처") — 이미 존재하는 `robot_ws.py`의 `_connections: Dict[str, WebSocket]`이 이 역할을 한다. Task 2의 `robot_registry.connected_robots`는 REST `GET /robots`의 `is_online` 계산에만 쓰던 것인데, **PIVOT 이후엔 실시간 연결 여부의 진짜 출처가 아니다** — `robot_ws.py`의 `is_connected(robot_code)`가 진짜 출처. Task 3(신규)에서 이 불일치를 바로잡는다.
- `careguard_turtlebot_agent/`는 리포 최상위에 `src/` 없이 바로 둔다. `robot_careguard_ws/`, `pc_careguard/`, `shared/`, `docs/`, `deploy/`는 구조·내용 그대로 둔다 (스펙 "리포 구조 변경") — 특히 `robot_careguard_ws/src/careguard_agent`(LIMO용, 이미 존재)는 이번 작업에서 절대 건드리지 않는다. 참고만 하고 새 폴더에 이식한다.
- 로봇 IP·네임스페이스·노드 상태·담당 대상자 배정은 이번 범위 밖 — 대시보드 표의 해당 칸은 빈 값/placeholder로 남는다. "상태" 칸만 실제 온라인 여부를 반영한다 (스펙 "범위")
- 이 저장소에는 기존 자동 테스트가 전혀 없다(서버 Python 쪽 Task 1 이전, 대시보드 C++ 쪽, ROS2 노드 쪽 전부). 서버 쪽은 Task 1에서 `pytest`를 새로 들여왔고, 대시보드(Qt/C++)와 ROS2 노드는 기존 관례(`careguard_agent`도 "빌드/실행 미검증" 상태로 존재)대로 빌드 성공 + 수동 스모크 테스트로 검증한다 — 새 C++/ROS2 테스트 프레임워크를 이번에 들여오지 않는다
- 서버 쪽 기존 WS 코드(`app/ws/robot_ws.py`, `app/ws/dashboard_ws.py`)와 대시보드 쪽 기존 `ws_client.{h,cpp}`는 **이번 작업에서 수정하지 않는다** — LIMO에서 이미 쓰고 있는 코드라 건드리면 그쪽이 깨질 위험이 있다. 순수하게 "같은 프로토콜로 접속하는 새 클라이언트(터틀봇)"만 추가한다.

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
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
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
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/pytest tests/ -v
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
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/pytest tests/test_robots.py -v
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
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/pytest tests/test_robots.py -v
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

## Task 3 (PIVOT — Task 2 사후 수정): 서버 — `is_online`을 기존 `robot_ws.is_connected`로 재배선

Task 2가 승인된 뒤, `origin/develop`에 이미 병합돼 있던 LIMO용 로봇 WebSocket 시스템
(`pc_careguard/server/app/ws/robot_ws.py`)의 존재를 발견했다. 원래 계획은 온라인 여부를
새로 만들 `robot_ws.py`(Task 3)의 `connected_robots` 집합으로 판단할 생각이었는데, 그
새 엔드포인트를 이제 만들지 않기로 했으니(아래 Task 5/6/7 SKIPPED 참고) `robots.py`가
지금 보고 있는 `robot_registry.connected_robots`는 **아무도 채워주지 않는 죽은 상태**다
— 즉 `GET /robots`의 `is_online`이 항상 `false`로 나온다. 이미 존재하는
`app/ws/robot_ws.py`의 `is_connected(robot_code: str) -> bool`(코드 확인함, 이미 공개
함수로 존재)가 진짜 연결 상태의 출처이니 여기로 다시 연결한다.

**Files:**
- Modify: `pc_careguard/server/app/robots.py`
- Delete: `pc_careguard/server/app/robot_registry.py` (이제 아무도 안 씀 — 죽은 코드로 남기지 않는다)
- Modify: `pc_careguard/server/tests/test_robots.py`

**Interfaces:**
- Consumes: `pc_careguard/server/app/ws/robot_ws.py`의 `is_connected(robot_code: str) -> bool` (기존 함수, 이번에 수정하지 않음 — import만 해서 씀)
- Produces: `GET /robots`의 `is_online` 필드가 실제 `/ws/robot?robot_code=...` 연결 여부를 정확히 반영

- [ ] **Step 1: 실패하는 테스트로 교체**

`pc_careguard/server/tests/test_robots.py`에서 기존
`test_list_robots_is_online_reflects_connected_registry` 함수 전체를 아래로
교체한다(이 함수만 교체, 나머지 3개 테스트는 그대로 둔다):

```python
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
```

파일 상단 import에 `from app.robot_registry import connected_robots` 줄이 있으면 지운다
(더 이상 이 테스트가 그 모듈을 쓰지 않는다).

- [ ] **Step 2: 실행해서 실패 확인**

```bash
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/pytest tests/test_robots.py -v
```
Expected: `test_list_robots_is_online_reflects_real_ws_connection`만 FAIL —
`body_during[code]`가 `False`로 나옴 (아직 `robots.py`가 `robot_registry.connected_robots`를 보고 있어서)

- [ ] **Step 3: `robots.py` 수정**

`pc_careguard/server/app/robots.py` 상단의:
```python
from .robot_registry import connected_robots
```
을:
```python
from .ws.robot_ws import is_connected
```
로 바꾸고, `_row_to_item` 함수 안의:
```python
        is_online=row["code"] in connected_robots,
```
을:
```python
        is_online=is_connected(row["code"]),
```
로 바꾼다. 나머지(`LIST_QUERY`, `list_robots`, `register_robot`)는 그대로 둔다.

- [ ] **Step 4: `robot_registry.py` 삭제**

```bash
git rm pc_careguard/server/app/robot_registry.py
```

- [ ] **Step 5: 테스트 통과 확인**

```bash
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/pytest tests/ -v
```
Expected: 전부 PASS (이 파일 안의 4개 테스트 + Task 1의 health 테스트)

- [ ] **Step 6: 서버가 실제로 뜨는지 확인 (import 에러 없는지)**

```bash
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
timeout 5 .venv/bin/uvicorn app.main:app --port 8011 &
sleep 2
curl -s http://127.0.0.1:8011/health
kill %1 2>/dev/null; wait 2>/dev/null
```
Expected: `{"status":"ok"}` — `app/ws/robot_ws.py`·`dashboard_ws.py`를 포함해 기존 라우터들이
전부 정상적으로 import·기동됨을 확인 (이 태스크가 그쪽 파일을 안 건드렸다는 걸
간접 확인하는 것이기도 함)

- [ ] **Step 7: 커밋**

```bash
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity
git add pc_careguard/server/app/robots.py pc_careguard/server/tests/test_robots.py
git rm pc_careguard/server/app/robot_registry.py 2>/dev/null || true
git commit -m "fix(server): 로봇 온라인 여부를 기존 robot_ws.is_connected로 재배선"
```

---

## Task 4 (PIVOT — 원래 Task 4·5·6 통합): 로봇 에이전트 — `careguard_turtlebot_agent` (기존 `careguard_agent` 연결 패턴 재사용)

`robot_careguard_ws/src/careguard_agent/uplink_node.py`(LIMO용, 이미 존재하고 `/map`
구독은 2026-09-04 실제 하드웨어로 검증됨 — 이번 작업에서 이 파일은 절대 건드리지
않는다)의 연결 유지·재연결 로직(`_connect_forever`)을 그대로 재사용해, 새 최상위 폴더
`careguard_turtlebot_agent/`에 최소 버전(토픽 구독 없이 접속 유지만)으로 이식한다.

서버의 기존 `/ws/robot?robot_code=<code>`(`app/ws/robot_ws.py:34`, 이번에 수정하지
않음)는 접속만 되면 알아서 대시보드에 `robot_status` 브로드캐스트를 보낸다(코드 확인:
`robot_ws.py`의 `while True: await websocket.receive_json()`엔 타임아웃이 없다 — 로봇
쪽에서 별도 heartbeat 메시지를 안 보내도 되고, TCP 연결이 끊기는 것만으로
offline 판정이 된다). 그래서 이 태스크엔 heartbeat 로직 자체가 없다.

**Files:**
- Create: `careguard_turtlebot_agent/package.xml`
- Create: `careguard_turtlebot_agent/setup.py`
- Create: `careguard_turtlebot_agent/setup.cfg`
- Create: `careguard_turtlebot_agent/resource/careguard_turtlebot_agent` (빈 파일, ament_python 마커)
- Create: `careguard_turtlebot_agent/careguard_turtlebot_agent/__init__.py` (빈 파일)
- Create: `careguard_turtlebot_agent/careguard_turtlebot_agent/ws_config.py`
- Create: `careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py`

**Interfaces:**
- Consumes: 서버의 기존 `/ws/robot?robot_code=<code>` (수정 없음). Task 3에서 고친
  `GET /robots`의 `is_online`으로 이 노드가 실제로 붙었는지 확인 가능.
- Produces: `CAREGUARD_ROBOT_CODE=<code> ros2 run careguard_turtlebot_agent uplink_node`로
  실행되는 노드. ROS2 파라미터가 아니라 환경변수(`CAREGUARD_SERVER_URL`,
  `CAREGUARD_ROBOT_CODE`) 기반 — 기존 `careguard_agent/ws_config.py`와 동일한 관례.

이 저장소에 이미 빈 디렉토리(`careguard_turtlebot_agent/`, `resource/`,
`careguard_turtlebot_agent/careguard_turtlebot_agent/`, `launch/`)가 있을 수 있다. 있으면
재사용하고, `launch/`는 이번 태스크에서 안 쓰니 비어 있어도 그대로 둔다(파일을 안
만든다 — 안 쓰는 빈 launch 파일을 만들 이유가 없다).

- [ ] **Step 1: package.xml**

```xml
<?xml version="1.0"?>
<?xml-model href="http://download.ros.org/schema/package_format3.xsd" schematypens="http://www.w3.org/2001/XMLSchema"?>
<package format="3">
  <name>careguard_turtlebot_agent</name>
  <version>0.1.0</version>
  <description>TurtleBot 최소 업링크 — 서버 /ws/robot에 접속해 온라인 상태를 알린다. robot_careguard_ws/src/careguard_agent(LIMO)의 연결 유지 패턴을 재사용했다.</description>
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

```python
from setuptools import find_packages, setup

package_name = 'careguard_turtlebot_agent'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
    ],
    install_requires=['setuptools', 'websockets'],
    zip_safe=True,
    maintainer='postwo',
    maintainer_email='ajtwlscldjs@gmail.com',
    description='TurtleBot -> CareGuard 서버 최소 업링크 (온라인 상태만)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'uplink_node = careguard_turtlebot_agent.uplink_node:main',
        ],
    },
)
```

- [ ] **Step 3: setup.cfg**

```ini
[develop]
script_dir=$base/lib/careguard_turtlebot_agent
[install]
install_scripts=$base/lib/careguard_turtlebot_agent
```

- [ ] **Step 4: 마커 파일들**

```bash
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity
mkdir -p careguard_turtlebot_agent/resource careguard_turtlebot_agent/careguard_turtlebot_agent
touch careguard_turtlebot_agent/resource/careguard_turtlebot_agent
touch careguard_turtlebot_agent/careguard_turtlebot_agent/__init__.py
```

- [ ] **Step 5: ws_config.py**

```python
import os

# LIMO용 robot_careguard_ws/src/careguard_agent/ws_config.py와 동일한 관례 —
# 서버 주소·로봇 코드를 환경변수 하나씩으로 통일한다.
SERVER_HTTP_URL = os.environ.get('CAREGUARD_SERVER_URL', 'http://127.0.0.1:8000')
SERVER_WS_URL = SERVER_HTTP_URL.replace('https://', 'wss://').replace('http://', 'ws://')

# 이 로봇의 robot.code (대시보드 "로봇 등록"으로 미리 만들어둔 값, 예: "R-05").
# LIMO 기본값(R-01)과 겹치지 않도록 기본값을 비워둔다 — 실행 시 반드시 넘겨야 한다.
ROBOT_CODE = os.environ.get('CAREGUARD_ROBOT_CODE', '')
```

- [ ] **Step 6: uplink_node.py**

```python
"""careguard_turtlebot_agent/uplink_node.py

터틀봇 -> 서버 최소 업링크. 서버의 기존 /ws/robot?robot_code=... 에 접속만
유지한다 — 접속 즉시 서버(app/ws/robot_ws.py)가 대시보드에
{"type":"robot_status","robot_code":...,"online":True}를 브로드캐스트하고,
연결이 끊기면 online:False를 브로드캐스트한다. 그래서 이 노드는 별도
heartbeat 메시지를 보낼 필요가 없다 — TCP 연결을 유지하는 것 자체가 전부다.

연결 유지·재연결 로직은 robot_careguard_ws/src/careguard_agent/uplink_node.py
(LIMO용)의 _connect_forever를 그대로 재사용했다. 토픽 릴레이(SLAM 지도·카메라·
순찰 진행률 등)는 이번 범위 밖이라 구독하지 않는다 — 필요해지면
careguard_agent/uplink_node.py를 참고해 여기에 추가하면 된다.
"""
import asyncio
import threading

import rclpy
from rclpy.node import Node

from . import ws_config

RECONNECT_DELAY_SEC = 3.0


class UplinkNode(Node):
    def __init__(self):
        super().__init__('uplink_node')

        if not ws_config.ROBOT_CODE:
            self.get_logger().error(
                "CAREGUARD_ROBOT_CODE 환경변수가 비어 있습니다. 예: "
                "CAREGUARD_ROBOT_CODE=R-05 ros2 run careguard_turtlebot_agent uplink_node")
            raise SystemExit(1)

        self._loop = asyncio.new_event_loop()
        self._ws = None
        self._connected = False

        self._thread = threading.Thread(target=self._run_ws_loop, daemon=True)
        self._thread.start()

    def _run_ws_loop(self):
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect_forever())

    async def _connect_forever(self):
        import websockets

        url = f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}'
        while rclpy.ok():
            try:
                async with websockets.connect(url) as ws:
                    self._ws = ws
                    self._connected = True
                    self.get_logger().info(f'서버 업링크 연결됨: {url}')
                    await ws.wait_closed()
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                self.get_logger().warn(f'업링크 연결 실패, {RECONNECT_DELAY_SEC}s 후 재시도: {exc}')
            self._connected = False
            self._ws = None
            await asyncio.sleep(RECONNECT_DELAY_SEC)


def main(args=None):
    rclpy.init(args=args)
    node = UplinkNode()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
```

- [ ] **Step 7: colcon build 확인**

```bash
mkdir -p /tmp/careguard_turtlebot_ws/src
ln -sfn /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/careguard_turtlebot_agent \
        /tmp/careguard_turtlebot_ws/src/careguard_turtlebot_agent
cd /tmp/careguard_turtlebot_ws
source /opt/ros/humble/setup.bash
colcon build --packages-select careguard_turtlebot_agent
```
Expected: `Finished <<< careguard_turtlebot_agent`

- [ ] **Step 8: 노드 기동 확인 (환경변수 없을 때/있을 때 둘 다)**

```bash
cd /tmp/careguard_turtlebot_ws
source install/setup.bash
python3 -c "import websockets" 2>/dev/null || pip install --user websockets

echo "--- CAREGUARD_ROBOT_CODE 없이 실행 (에러 로그 남기고 깨끗하게 종료돼야 함) ---"
ros2 run careguard_turtlebot_agent uplink_node; echo "exit code: $?"

echo "--- CAREGUARD_ROBOT_CODE 지정 후 실행 (서버 없어도 재연결 로그만 남고 안 죽어야 함) ---"
timeout 5 env CAREGUARD_ROBOT_CODE=TEST-SMOKE ros2 run careguard_turtlebot_agent uplink_node
```
Expected: 첫 번째는 `robot_code 환경변수가 비어 있습니다` 에러 로그 후 non-zero
종료. 두 번째는 "업링크 연결 실패, 3.0s 후 재시도" 경고 로그만 반복되고(서버가 안 떠
있으니 당연함) 5초 뒤 timeout으로 강제 종료됨 — 이 자체가 재연결 루프가 죽지 않고
도는 걸 보여주는 것.

- [ ] **Step 9: (선택, 수동) 실제 서버와 연결 확인**

```bash
# 터미널 1: 서버
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
.venv/bin/uvicorn app.main:app --port 8000

# 터미널 2: 로봇 등록 (Task 2 API)
curl -X POST http://127.0.0.1:8000/robots \
  -H "Content-Type: application/json" \
  -d '{"code":"R-TEST","name":"수동확인용","model":"turtlebot"}'

# 터미널 3: 노드 실행
cd /tmp/careguard_turtlebot_ws && source install/setup.bash
CAREGUARD_ROBOT_CODE=R-TEST CAREGUARD_SERVER_URL=http://127.0.0.1:8000 \
  ros2 run careguard_turtlebot_agent uplink_node

# 터미널 4: 온라인 상태 확인 (Task 3에서 고친 is_online)
curl -s http://127.0.0.1:8000/robots | python3 -m json.tool
```
`R-TEST`의 `is_online`이 `true`인지 확인. 터미널 3을 Ctrl+C로 끄고 다시
`curl http://127.0.0.1:8000/robots`로 `false`로 바뀌는지 확인(즉시 바뀜 —
서버 쪽에 타임아웃 대기가 없음). 확인 후 `R-TEST` row는 DB에서 지워도 되고 남겨둬도
다음 태스크에 지장 없음.

- [ ] **Step 10: 커밋**

```bash
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity
git add careguard_turtlebot_agent/package.xml careguard_turtlebot_agent/setup.py \
        careguard_turtlebot_agent/setup.cfg \
        careguard_turtlebot_agent/resource/careguard_turtlebot_agent \
        careguard_turtlebot_agent/careguard_turtlebot_agent/__init__.py \
        careguard_turtlebot_agent/careguard_turtlebot_agent/ws_config.py \
        careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py
git commit -m "feat(agent): careguard_turtlebot_agent 최소 업링크 노드 추가"
```

---

## Task 5: SKIPPED (PIVOT)

원래 계획: 자체 `HeartbeatWorker`(재연결·백오프 로직)를 pytest로 테스트하며 새로
설계. **더 이상 필요 없음** — 기존 시스템엔 로봇 쪽 heartbeat 메시지 개념 자체가
없고(서버가 TCP 연결 유지 여부만 본다), 재연결 로직은 Task 4에서 기존
`careguard_agent/uplink_node.py`의 `_connect_forever`를 그대로 재사용했다. 실행할 것 없음.

---

## Task 6: SKIPPED (PIVOT)

원래 계획: `uplink_node.py` + launch 파일을 새로 설계. **Task 4에 이미 포함됨**
(launch 파일은 이번 범위에선 안 씀 — `ros2 run` + 환경변수로 충분). 실행할 것 없음.

---

## Task 7: SKIPPED (PIVOT)

원래 계획: 대시보드에 `Qt6::WebSockets` 추가 + 새 `RobotWsClient` 클래스 작성. **이미
전부 존재함** — `pc_careguard/dashboard/CMakeLists.txt`에 `Qt6::WebSockets`가 이미
링크돼 있고, `pc_careguard/dashboard/src/net/ws_client.{h,cpp}`(`WsClient` 클래스, P5/P6
페이지가 이미 사용 중)가 이미 있다. 새로 만들 것 없음 — Task 8에서 이 기존 클래스를
그대로 재사용한다.

---

## Task 8 (PIVOT — 기존 `WsClient` 재사용): 대시보드 — `P7SettingsPage` 실제 연동

**Files:**
- Modify: `pc_careguard/dashboard/src/pages/p7_settings_page.h`
- Modify: `pc_careguard/dashboard/src/pages/p7_settings_page.cpp`

**Interfaces:**
- Consumes: Task 2/3의 서버 `GET/POST /robots`(REST, 최초 목록 로드·등록용); 이미 존재하는
  `pc_careguard/dashboard/src/net/ws_client.h`의 `WsClient` 클래스 — 생성자 인자 없음
  (`CAREGUARD_SERVER_URL` 환경변수를 자기가 알아서 읽음), `connectToServer()`로 접속 시작,
  `messageReceived(const QJsonObject&)` 시그널로 모든 타입의 메시지가 들어옴(P5 페이지가
  이미 이 패턴을 씀 — `src/pages/p5_map_page.cpp:79-81` 참고). 이 태스크가 관심 있는 건
  `{"type":"robot_status","robot_code":...,"online":...}` 뿐이고, 나머지 타입
  (`robot_position`/`slam_progress`/`slam_done`/`patrol_progress`/`patrol_done`/`camera_frame`
  등)은 무시한다.
- Produces: 로봇 관리 탭이 하드코딩 목업 대신 실제 서버 데이터로 채워지고, 등록 버튼이
  실제로 DB에 저장하며, 온라인/오프라인이 기존 WebSocket 인프라를 통해 실시간으로 갱신됨

이 프로젝트엔 C++ 자동 테스트 프레임워크가 없다(Global Constraints 참고) — 이 태스크는
**빌드 성공 + 수동 스모크 테스트**로 검증한다. `Qt6::WebSockets`는 이미
`CMakeLists.txt`에 링크돼 있으므로 이번 태스크에서 CMakeLists.txt를 건드릴 필요가 없다.

- [ ] **Step 1: 헤더에 선언 추가**

`pc_careguard/dashboard/src/pages/p7_settings_page.h`:

`class LiveSearchLineEdit;` 다음 줄에 전방 선언 추가:
```cpp
class LiveSearchLineEdit;
class WsClient;
```

`#include "dialogs/p7_account_dialog.h"` 다음 줄에 include 추가(이 헤더에서
`RobotDialog::RobotData`를 쓰려면 필요):
```cpp
#include "dialogs/p7_account_dialog.h"
#include "dialogs/p7_robot_dialog.h"
#include "theme/tokens.h"
```

`void openRobotDialog(int rowIndex);` 아래, `void populateRobotRow(int r);`/
`void refreshRobotTable();` 다음 줄에 메서드 선언 추가:
```cpp
    void openRobotDialog(int rowIndex);  // -1이면 신규 등록
    void populateRobotRow(int r);
    void refreshRobotTable();
    void fetchRobots();  // GET /robots
    void handleRobotsReply(QNetworkReply* reply);
    void registerRobot(const RobotDialog::RobotData& data);  // POST /robots
    void handleWsMessage(const QJsonObject& message);  // WsClient::messageReceived — type으로 분기
```

`QPushButton* deleteAccountBtn_ = nullptr;` 다음 줄(멤버 변수 영역)에 추가:
```cpp
    QPushButton* deleteAccountBtn_ = nullptr;
    WsClient* wsClient_ = nullptr;
```

- [ ] **Step 2: 생성자 — 하드코딩 시드 제거**

`pc_careguard/dashboard/src/pages/p7_settings_page.cpp`, 기존(생성자 시작 부분,
`robots_ = { ... };` 블록 전체, R-01~R-04 4개 항목):
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
다음으로 교체 (`robots_ = {...}` 블록만 통째로 삭제하고, `r03Nodes_`는 그대로 둔다 —
R-03 노드 상세 카드는 이 태스크와 무관한 별개의 하드코딩 목업이라 손대지 않는다):
```cpp
P7SettingsPage::P7SettingsPage(QWidget* parent) : QWidget(parent) {
    // 로봇 목록은 하드코딩하지 않고 서버 DB에서 불러온다 (fetchRobots(), 생성자 마지막에서 호출).

    r03Nodes_ = {
```

- [ ] **Step 3: 생성자 끝 — fetchRobots + 기존 WsClient 연결**

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

    wsClient_ = new WsClient(this);
    connect(wsClient_, &WsClient::messageReceived, this, &P7SettingsPage::handleWsMessage);
    wsClient_->connectToServer();
}
```

cpp 파일 상단 include 목록에 `#include "net/ws_client.h"`를 추가한다
(`#include "dialogs/p7_robot_dialog.h"` 다음 줄):
```cpp
#include "dialogs/p7_robot_dialog.h"
#include "net/ws_client.h"
#include "theme/calendar_style.h"
```

- [ ] **Step 4: 등록 시 실제 API 호출로 교체**

`pc_careguard/dashboard/src/pages/p7_settings_page.cpp`, `openRobotDialog` 안의 기존:
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
다음으로 교체 (신규 등록만 서버로 보내고, 기존 로봇 수정은 이번 범위 밖이라 기존 로컬
처리 그대로 둔다):
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

- [ ] **Step 5: fetchRobots/handleRobotsReply/registerRobot/handleWsMessage 구현**

`pc_careguard/dashboard/src/pages/p7_settings_page.cpp`의 `refreshRobotTable()` 함수
바로 뒤에 추가:
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

void P7SettingsPage::handleWsMessage(const QJsonObject& message) {
    if (message.value(QStringLiteral("type")).toString() != QStringLiteral("robot_status")) return;

    const QString code = message.value(QStringLiteral("robot_code")).toString();
    const bool online = message.value(QStringLiteral("online")).toBool();
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
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/dashboard/build
cmake -DCMAKE_PREFIX_PATH=/home/cce/Qt/6.11.1/gcc_64 .. 2>&1 | tail -20
cmake --build . -j"$(nproc)" 2>&1 | tail -60
```
Expected: `Built target careguard_dashboard`

- [ ] **Step 7: 수동 스모크 테스트**

```bash
# 터미널 1: 서버
docker compose -f /home/cce/twomainProject/deploy/docker-compose.yml up -d postgres
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/server
.venv/bin/uvicorn app.main:app --reload

# 터미널 2: 대시보드 실행
/home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity/pc_careguard/dashboard/build/careguard_dashboard
```
확인 항목:
1. "로봇 관리" 탭에 로봇 목록이 뜬다(이전 태스크에서 만든 테스트 로봇이 남아있을 수 있음)
2. "로봇 등록" → 모델 `turtlebot`, 코드/이름/ROS_DOMAIN_ID 입력 후 저장 → 목록에 나타나고 "상태"가 오프라인
3. 터미널 3에서 Task 4의 방식으로 같은 코드로 `uplink_node` 실행
   (`CAREGUARD_ROBOT_CODE=<코드> ros2 run careguard_turtlebot_agent uplink_node`) →
   대시보드가 몇 초 안에(폴링 없이, push로) "온라인"으로 자동 갱신되는지 확인
4. `uplink_node`를 Ctrl+C로 종료 → 대시보드가 즉시(서버 쪽 타임아웃이 없으므로 몇 초 내)
   "오프라인"으로 자동 갱신되는지 확인

- [ ] **Step 8: 커밋**

```bash
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity
git add pc_careguard/dashboard/src/pages/p7_settings_page.h pc_careguard/dashboard/src/pages/p7_settings_page.cpp
git commit -m "feat(dashboard): 로봇 관리 탭을 실제 서버·기존 WsClient와 연동"
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
cd /home/cce/twomainProject/.worktrees/feature-turtlebot-realtime-connectivity
git add CONVENTIONS.md
git commit -m "docs(docs): careguard_turtlebot_agent 최상위 폴더 규칙 반영"
```

---

## 이 계획에 없는 것 (스펙의 "제외" 그대로)

- 미션 명령, 이벤트 업로드, 영상/스냅샷 전송
- 로봇 IP·네임스페이스·노드 상태·담당 대상자 배정을 실제 데이터로 채우는 것
- LIMO 로봇(`robot_careguard_ws`) 관련 변경
- 기존 로봇 정보 수정(PATCH) API — 등록(POST)만 실 서버 연동
