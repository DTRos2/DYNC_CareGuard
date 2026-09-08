# 터틀봇 ↔ 서버 ↔ 대시보드 실시간 연결 설계

## 배경

P7 설정 페이지의 "로봇 관리" 탭은 지금까지 로봇 목록을 하드코딩된 목업 데이터(`robots_`)로만
보여줬고, 등록 버튼도 로컬 벡터만 바꿀 뿐 서버에 아무것도 남기지 않았다(계정 관리와 달리
REST 연동이 아예 없었음). `robot` DB 테이블(`code`, `name`, `model`, `ros_domain_id`)은 이미
존재하지만 이걸 읽고 쓰는 API가 서버에 없고, 로봇(터틀봇) 쪽에서 서버로 접속하는 코드도
전혀 없다.

이번 작업의 목표: **터틀봇을 켜면 서버에 실제로 연결되고, 대시보드 "로봇 관리" 탭에 그
온라인/오프라인 상태가 실시간으로 뜨게 만든다.** 미션·이벤트·영상 등 그 이후 단계는
범위 밖이다.

## 범위

**포함**
- 대시보드에서 로봇을 등록하면 서버 DB(`robot` 테이블)에 실제로 저장됨
- 터틀봇의 라즈베리파이에서 실행하는 ROS2 노드가 서버에 WebSocket으로 접속해 주기적으로
  살아있음을 알림(heartbeat)
- 서버는 등록된 로봇의 접속 여부를 실시간으로 추적하고, 대시보드에 WebSocket으로 즉시 push
- 대시보드는 폴링 없이 WebSocket push만으로 온라인/오프라인 상태를 갱신

**제외 (다음 단계로 미룸)**
- 미션 명령, 이벤트 업로드, 영상/스냅샷 전송 (`careguard_agent`의 downlink, media_uploader는
  이번에 안 만듦)
- 로봇의 IP·네임스페이스·노드 상태(`6/6 정상` 같은 것)·담당 대상자 배정 — DB/서버에 아직
  이 데이터의 출처가 없으므로, 대시보드 표의 해당 칸은 이번 작업 후에도 빈 값/placeholder로
  남는다. "상태" 칸만 실제 `is_online` 값을 반영한다
- LIMO 로봇(`robot_careguard_ws` 하위 패키지들)은 건드리지 않음. 이번 작업은 터틀봇 전용

## 아키텍처

```
[라즈베리파이 (터틀봇)]              [PC: FastAPI 서버]                  [PC: Qt 대시보드]
careguard_turtlebot_agent/     --WS-->  connected_robots (registry)  --WS-->  RobotWsClient
  uplink_node.py                        /ws/robot/{code}                      /ws/dashboard 구독
  (5초마다 heartbeat 전송)                 ↓ 상태 변화 시 broadcast              (표의 온라인/오프라인
                                       /robots (GET·POST, REST)  <----------   즉시 반영, 폴링 없음)
                                          ↓                                   최초 목록 로드(REST) +
                                       robot 테이블 (기존 스키마 그대로 사용)      등록(REST)만 REST
```

- 로봇은 먼저 대시보드에서 등록돼 있어야 한다. DB에 없는 `code`로 들어오는 WebSocket 접속은
  서버가 즉시 거부한다 (`p7_robot_dialog.cpp`에 이미 이 의도의 안내 문구가 있었음)
- 온라인 여부는 DB 컬럼을 추가하지 않고 **서버 프로세스 메모리**(연결된 코드 집합)로만
  판단한다. 서버 재시작 시 리셋되지만 이번 범위엔 충분하다
- 로봇 → 서버는 WebSocket, 대시보드 → 서버는 REST(등록/최초 조회) + WebSocket(실시간 상태
  구독)을 함께 쓴다. 대시보드의 REST 폴링은 없다

## 서버 (`pc_careguard/server/app/`)

### `schemas.py`에 추가 (dto)
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

### `robots.py` (신규, `accounts.py`와 동일 패턴)
- `router = APIRouter(prefix="/robots", tags=["robots"])`
- `GET /robots` — `robot` 테이블 조회(`deregistered_at IS NULL`) + `robot_ws.connected_robots`로
  `is_online` 계산해서 합쳐 반환
- `POST /robots` — insert. `code` 중복은 `robot_active_code_idx` 유니크 제약 위반 →
  `psycopg2.errors.UniqueViolation` 잡아서 409 (accounts.py의 로그인 ID 중복 처리와 동일 패턴)

### `robot_ws.py` (신규)
- 모듈 전역 `connected_robots: set[str]`, `dashboard_sockets: list[WebSocket]`
- `@router.websocket("/ws/robot/{code}")`
  1. DB에서 `code`가 `robot` 테이블에 있고 `deregistered_at IS NULL`인지 확인. 없으면
     `await websocket.close(code=4404)` (미등록 로봇 접속 거부)
  2. 있으면 `accept()` → `connected_robots.add(code)` → `_broadcast_status(code, True)`
  3. `while True: await asyncio.wait_for(websocket.receive_text(), timeout=15)` — 5초 주기
     heartbeat를 15초(3회분) 안에 못 받으면 `TimeoutError`로 루프 탈출
  4. `finally` 블록에서 `WebSocketDisconnect`든 timeout이든 상관없이
     `connected_robots.discard(code)` → `_broadcast_status(code, False)`
- `@router.websocket("/ws/dashboard")`
  1. `accept()` → `dashboard_sockets.append(websocket)`
  2. 접속 직후 현재 스냅샷 전송: `{"type": "snapshot", "online_codes": [...]}`
  3. 이후 서버→클라이언트 단방향(구독)만 사용. 클라이언트가 보내는 메시지는 없다고 가정하되,
     연결 유지를 위해 `await websocket.receive_text()`로 대기하다가 `WebSocketDisconnect` 시
     `dashboard_sockets.remove(websocket)`
- `_broadcast_status(code, online)`: `dashboard_sockets`의 각 소켓에
  `{"type": "robot_status", "code": code, "online": online}` 전송, 끊긴 소켓은 무시하고 제거

### `main.py`
- `from .robots import router as robots_router`
- `from .robot_ws import router as robot_ws_router`
- `app.include_router(robots_router)`, `app.include_router(robot_ws_router)`

## 로봇 에이전트 — 신규 최상위 폴더 `careguard_turtlebot_agent/`

`robot_careguard_ws/`(LIMO 전용, 기존)와 별개로 터틀봇 전용 패키지를 리포 최상위에 새로
둔다. `src/` 아래 워크스페이스 형태로 감싸지 않고, 패키지 자체가 최상위에 바로 온다
(사용자 확정 사항). 콜콘 빌드는 라즈베리파이에 별도 워크스페이스(예: `~/turtlebot_ws/src/`)를
만들고 이 폴더를 심볼릭 링크/복사해 넣는 방식으로 한다 — 라즈베리파이 설정 안내 참고.

```
careguard_turtlebot_agent/
├── package.xml
├── setup.py
├── setup.cfg
├── resource/careguard_turtlebot_agent      # ament_python 마커 파일(빈 파일)
├── careguard_turtlebot_agent/
│   ├── __init__.py
│   └── uplink_node.py
├── launch/
│   └── agent.launch.py
└── requirements.txt                         # websocket-client (rosdep 키 없는 순수 PyPI 패키지)
```

### `uplink_node.py`
- `rclpy.node.Node` 상속. 이번 범위엔 ROS2 토픽 구독/발행 없음 — 나중에 배터리 등 상태를
  heartbeat에 실어 보내기 쉽도록 노드 형태만 갖춘다
- 파라미터: `robot_code`(필수), `server_host`(필수), `server_port`(기본 8000)
- `websocket-client`(동기) 라이브러리를 별도 스레드에서 사용 — rclpy spin과 asyncio를 섞지
  않기 위함
- 스레드 루프 (지수 백오프, 최대 30초):
  ```python
  backoff = 1
  while rclpy.ok():
      try:
          ws = create_connection(
              f"ws://{server_host}:{server_port}/ws/robot/{robot_code}", timeout=5)
          backoff = 1
          while rclpy.ok():
              ws.send('{"type": "heartbeat"}')
              time.sleep(5)
      except Exception:
          time.sleep(backoff)
          backoff = min(backoff * 2, 30)
  ```
- 서버가 미등록 코드로 즉시 close해도 같은 루프로 재시도한다 — 나중에 대시보드에서 등록만
  하면 로봇 재시작 없이 다음 재시도 때 자동으로 붙는다
- `rclpy.spin(node)`는 메인 스레드에서 실행, 종료는 rclpy 기본 동작에 맡긴다

## 대시보드 (`pc_careguard/dashboard`)

### `CMakeLists.txt`
- `find_package(Qt6 ... COMPONENTS Widgets SvgWidgets Multimedia MultimediaWidgets Network WebSockets)`
- `target_link_libraries`에 `Qt6::WebSockets` 추가

### `src/net/robot_ws_client.h` / `.cpp` (신규, `net/` 디렉토리도 신규)
- `RobotWsClient : public QObject`
- `QWebSocket`으로 `ws://<host>:<port>/ws/dashboard` 연결. host/port는 기존
  `CAREGUARD_SERVER_URL`(`http://127.0.0.1:8000` 기본값)에서 스킴만 `ws`로 바꿔 유도
- 수신 메시지(JSON) 파싱:
  - `{"type":"snapshot","online_codes":[...]}` → `snapshotReceived(QStringList onlineCodes)` 발생
  - `{"type":"robot_status","code":..,"online":..}` → `robotStatusChanged(QString code, bool online)` 발생
- 연결 끊기면 `QTimer`로 3초 간격 재시도 (고정 간격 — 대시보드는 상시 켜진 PC 프로그램이라
  지수 백오프까지는 불필요)

### `src/pages/p7_settings_page.{h,cpp}`
- 생성자: 하드코딩된 `robots_` 시드 제거. `fetchRobots()`(REST, 최초 1회) 호출 +
  `RobotWsClient` 생성 후 시그널 연결
- `fetchRobots()` / `handleRobotsReply()`: `accounts.py` 연동과 동일 패턴으로 GET `/robots` →
  `RobotRow` 채움. `RobotRow`의 `ip`/`ns`/`mapRecipient`/`nodeSummary`/`connectionDetail`은
  서버에 출처가 없으므로 빈 값으로 두고, `level`/`statusDetail`만 `is_online`으로 결정
  (`true`→`Tokens::Level::Normal`+"● 온라인", `false`→`Tokens::Level::Offline`+"오프라인")
- `snapshotReceived`/`robotStatusChanged` 슬롯: 해당 `code` 행만 찾아 `level`/`statusDetail`
  갱신 후 `populateRobotRow()`로 그 행만 다시 그림 (전체 `refreshRobotTable()` 불필요)
- "로봇 등록" 다이얼로그 저장 시 로컬 벡터만 바꾸던 것을 `registerRobot()`(POST `/robots`)
  호출로 교체 — 성공 응답 후 `fetchRobots()`로 목록 재조회 (계정 등록과 동일 패턴)

## 리포 구조 변경

`CONVENTIONS.md` 5절("리포 구조 [확정]") 표에 아래 행을 추가한다 (COMMIT_RULES.md 6절
변경 절차: 제안 근거는 이 스펙 문서, 반영은 이 작업의 PR에서 문서·코드 동시에):

| 경로 | 내용 | 실행 위치 |
|---|---|---|
| `careguard_turtlebot_agent/` | TurtleBot ↔ 서버 WebSocket 브리지 (ROS2 ament_python 패키지, `src/` 없이 최상위) | TurtleBot 라즈베리파이 |

기존 다섯 폴더(`robot_careguard_ws/`, `pc_careguard/`, `shared/`, `docs/`, `deploy/`)는
구조·내용 모두 그대로 둔다. `robot_careguard_ws/src/careguard_bringup·msgs·nav`도 이번
작업 범위 밖이라 손대지 않는다.

## 라즈베리파이 설정 (코드 변경 아님 — 안내 사항)

> **2026-09-07 갱신**: 아래는 PIVOT(구현 계획 문서 상단 "PIVOT" 절 참고 — 서버·대시보드
> WebSocket을 새로 만들지 않고 `origin/develop`에 이미 있던 LIMO용 시스템을 그대로
> 재사용하기로 한 결정) 반영 후의 실제 절차다. 이전 버전은 존재하지 않는
> `requirements.txt`, `websocket-client`, `--ros-args -p robot_code:=...` 등 실제 구현과
> 다른 내용이 있었다 — 아래로 대체한다.

전제: 라즈베리파이에 ROS2와 터틀봇 드라이버는 이미 설치돼 있고, 부팅만 한 상태.

1. **코드 가져오기**: 이 저장소를 Pi에서 `git pull` (Pi에 이미 클론돼 있다고 가정 — 사용자가
   직접 처리)
2. **콜콘 워크스페이스 준비**: `careguard_turtlebot_agent`는 `src/`로 감싸여 있지 않으므로,
   별도 워크스페이스(예: `~/turtlebot_ws/src/`)를 만들고 그 안에 이 폴더를 심볼릭 링크
   (`ln -s ~/<repo>/careguard_turtlebot_agent ~/turtlebot_ws/src/careguard_turtlebot_agent`)
3. **파이썬 의존성**: `pip install --user websockets` (비동기 WebSocket 클라이언트 —
   LIMO용 `careguard_agent`가 쓰는 것과 동일한 라이브러리. `requirements.txt` 파일은 없다 —
   `package.xml`이 rosdep 키로 선언하지 않은 순수 PyPI 패키지라 수동 설치가 필요하다)
4. **빌드**: `cd ~/turtlebot_ws && colcon build --packages-select careguard_turtlebot_agent`
5. **네트워크**: Pi와 PC 서버가 같은 LAN/Wi-Fi에 있어야 함. PC 서버의 실제 IP를 확인해서
   다음 실행 시 환경변수에 넣는다 (`hostname -I` 등)
6. **로봇 등록(먼저 해도, 나중에 해도 됨)**: 대시보드 "로봇 관리" 탭 → "로봇 등록"에서
   모델을 `turtlebot`으로, 코드(예: `R-05`, 16자 이하)와 `ROS_DOMAIN_ID`(0~232)를 정해서
   등록. 이 코드가 다음 실행의 `CAREGUARD_ROBOT_CODE`가 됨. **주의**: 재사용 중인 기존
   `/ws/robot?robot_code=...` 엔드포인트는 미등록 코드도 그냥 받아준다(등록 여부를
   확인하지 않음) — 그래서 등록 전에 노드를 먼저 켜도 접속 자체는 된다. 다만 그 상태에선
   `GET /robots` 목록에 아예 없는 코드라 대시보드 "로봇 관리" 탭에는 안 보인다. 그러니
   대시보드에 보이게 하려면 등록을 반드시 해야 한다(접속 거부 때문이 아니라 목록에
   나타나게 하기 위해서).
7. **실행** — ROS2 파라미터가 아니라 환경변수로 설정한다:
   ```bash
   source ~/turtlebot_ws/install/setup.bash
   CAREGUARD_ROBOT_CODE=R-05 CAREGUARD_SERVER_URL=http://<PC IP>:8000 \
     ros2 run careguard_turtlebot_agent uplink_node
   ```
   `CAREGUARD_ROBOT_CODE`를 안 주면(빈 문자열) 노드가 에러 로그를 남기고 즉시 종료된다.
8. 서버가 아직 안 떠 있거나 네트워크가 잠깐 끊겨도 노드는 3초 간격으로 계속 재연결을
   시도한다(죽지 않음) — 서버가 뜨면 다음 재시도 때 자동으로 붙는다.
9. **온라인 여부는 heartbeat가 아니라 TCP 연결 자체로 판단**한다 — 재사용 중인
   `robot_ws.py`엔 타임아웃 로직이 없다. 노드가 접속해 있는 동안은 계속 온라인, 연결이
   끊기면(Ctrl+C 등) 즉시(수 초 안에) 오프라인으로 바뀐다. 로봇 쪽에서 별도 heartbeat
   메시지를 보낼 필요가 없다.
10. `ROS_DOMAIN_ID`는 이 워크스페이스의 다른 ROS2 노드(터틀봇 bringup 등)와 통일해서 써야
    같은 도메인 안에서 서로 통신 가능 — 대시보드에 등록한 값과 실제 Pi에서 `export
    ROS_DOMAIN_ID=<값>`으로 export하는 값을 맞춰둔다 (uplink_node 자체는 이번 범위에선
    ROS2 토픽을 구독하지 않으므로 이 값에 직접 의존하진 않지만, 같은 로봇의 다른
    노드들과의 일관성을 위해 등록 시점에 실제 값을 적어두는 것)

## 테스트 계획

> 2026-09-07 갱신: PIVOT 이후 서버·대시보드 쪽 WebSocket은 새로 만들지 않고 기존
> `app/ws/robot_ws.py`/`dashboard_ws.py`, 대시보드 `WsClient`를 재사용한다 — 아래는 실제로
> 구현·실행된 테스트 기준이다.

- **서버**: `pc_careguard/server/tests/test_robots.py` — `POST/GET/DELETE /robots` 전체
  (등록, 중복 코드 409, `ros_domain_id`/`code` 범위를 벗어나면 422, 목록 조회, 등록
  해제 후 목록에서 빠지는지, 존재하지 않는 id 삭제 시 404), 그리고 `is_online`이 실제
  `/ws/robot?robot_code=...` WebSocket 연결 여부(재사용 중인 기존 `robot_ws.is_connected`)를
  정확히 반영하는지. 실행: `cd pc_careguard/server && PYTEST_DISABLE_PLUGIN_AUTOLOAD=1
  .venv/bin/pytest tests/ -v` (이 환경은 ROS2가 전역 `PYTHONPATH`를 오염시켜 그 환경변수가
  없으면 pytest 플러그인 로딩이 깨진다 — 로컬 Postgres가 떠 있어야 함).
- **로봇 에이전트**: `careguard_turtlebot_agent`엔 자동 테스트가 없다 — 재사용한 LIMO용
  `careguard_agent`도 마찬가지 관례(빌드/실행 검증만). `colcon build` 성공 + 수동 실행으로
  검증(환경변수 없을 때 에러 종료, 있을 때 재연결 루프가 안 죽는지).
- **대시보드**: C++ 자동 테스트 프레임워크가 이 리포에 없다 — 빌드 성공 + 수동 스모크
  테스트로 검증. `pc_careguard/dashboard/src/net/ws_client.{h,cpp}`(기존, 이번에 안 건드림)
  자체는 이미 P5 페이지에서 쓰이고 있어 별도 테스트를 새로 만들지 않았다.
- **엔드투엔드 수동 확인**: 서버 기동 → 대시보드에서 로봇 등록 → 라즈베리파이(또는 같은
  네트워크의 아무 PC)에서 `CAREGUARD_ROBOT_CODE=<코드> ros2 run careguard_turtlebot_agent
  uplink_node` 실행 → 대시보드 "로봇 관리" 탭에 "온라인"으로 바뀌는지, 노드 종료 시 몇
  초 안에 "오프라인"으로 바뀌는지 눈으로 확인. **2026-09-07 기준 이 구간은 샌드박스
  환경에 GUI 자동화 도구가 없어 로그인 화면 이후를 실제로 구동해 확인하지는
  못했다** — 코드 리딩으로는 문제없어 보이지만(전체 브랜치 리뷰에서 별도로 재확인함),
  실제 배포 전에 사람이 한 번 직접 로그인해서 눈으로 확인하는 걸 권장한다.
