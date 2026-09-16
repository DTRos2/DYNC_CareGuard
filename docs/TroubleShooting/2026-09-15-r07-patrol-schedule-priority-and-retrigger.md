# 2026-09-15 트러블슈팅 — R-07 스케줄 순찰 stuck 사고 조사 및 순찰 우선순위 가드 구현

> 작성자: 도희정
> 대상 파일:
> `robot_careguard_ws/src/careguard_agent/careguard_agent/downlink_node.py`,
> `pc_careguard/server/app/api/v1/patrol.py`,
> `pc_careguard/server/app/ws/robot_ws.py`,
> `pc_careguard/db/schema/careguard_schema_v1.sql`, `pc_careguard/db/schema/02_alter_patrol_run_status_paused.sql`,
> `pc_careguard/dashboard/src/pages/p6_patrol_page.{h,cpp}`, `pc_careguard/dashboard/src/pages/p5_map_page.{h,cpp}`
> 대상 로봇: R-07(LIMO, wego@192.168.230.42)

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 주행 스케줄이 정상적으로 트리거되고(`patrol_run` `running` row 생성) 로봇에 명령도 도달했는데, 로봇이 끝내 움직이지 않고 `patrol_run`이 영원히 `running`/`waypoints_done=0`으로 stuck | `downlink_node._ensure_patrol_task_running()`이 "이 함수는 순찰이 실제로 진행 중일 땐 호출되지 않는다"는 잘못된 전제로 호출될 때마다 patrol_task를 무조건 kill+재시작하도록 짜여 있었음 — P6 페이지를 열거나 새로고침만 해도 나가는 `nav_prepare`가 이 함수를 호출해, 진행 중이던 goal을 SIGINT로 조용히 죽임 | `_ensure_patrol_task_running(map_id, override=False)`로 override 파라미터를 추가 — override 없이는 이미 goal이 실행 중이면 아무 것도 하지 않고 그대로 재사용 |
| 2 | (요청 사양) 순찰 중에도 수동 주행 버튼·텔레옵으로 로봇을 가로챌 수 있었고, 다른 팀원 PC는 지금 로봇이 순찰 중인지 실시간으로 알 방법이 없었으며, ESP32 SOS 긴급 호출은 순찰을 방해하지 않고 우선 처리돼야 했음 | 서버·로봇 어디에도 "이미 순찰/이동 중이면 새 요청을 막는다"는 가드 자체가 없었고, 순찰 상태 변경을 모든 대시보드에 실시간으로 알리는 브로드캐스트도 없었음 | 서버에 점유 가드(`_reject_if_already_patrolling`) 추가, ESP32 SOS는 기존 순찰을 `paused`로 남기고 끼어든 뒤 자동 재개하도록 구현, `patrol_run_updated` 브로드캐스트로 전 팀원 PC 배너·버튼 잠금 실시간 동기화, P5 텔레옵 패널도 순찰 중 비활성화 |

---

## 1. R-07 스케줄 순찰이 트리거됐지만 로봇이 움직이지 않고 stuck된 사고

### 현상

`patrol_schedule`에 R-07(route 21, 웨이포인트 2개)에 대해 매일 10:50 실행되는 스케줄을 등록해뒀다. 실제로 10:50:13에 스케줄러가 정상적으로 트리거되어 `patrol_run` row(id=32, `trigger='schedule'`, `status='running'`, `waypoints_total=2`)가 생성됐다. 그런데:

- 로봇은 실제로 움직이지 않았다(사용자 육안 확인).
- `patrol_run.waypoints_done`이 0에서 전혀 올라가지 않았다.
- `patrol_run.status`가 `running`에서 몇 시간이 지나도 `completed`/`failed`/`aborted` 어느 것으로도 바뀌지 않았다(진행 상황을 서버가 전혀 못 받는 "고아(orphan) 기록" 상태).
- 대시보드 P6 페이지 상단 배너에도 "주행 중" 표시가 뜨지 않았다.

### 원인 조사 과정

1차 가설로 "서버가 `robot_ws.send_command()`의 성공 여부를 확인 안 해서 WS 미연결 시 조용히 실패한다"를 세웠으나, DB의 `patrol_run.waypoints_total=2`(route steps가 정상적으로 조회돼 페이로드에 실렸다는 뜻)로 배제됐다. 또한 직전(10:45~10:47) 같은 로봇으로 수동 "경로 주행"이 정상적으로 `completed`까지 간 기록(run id=31)이 있어, WS 연결 자체는 살아있었다는 것도 확인했다.

R-07에 SSH로 접속해(`ssh wego@192.168.230.42`) `downlink_node`와 `patrol_task`의 ROS2 로그(`~/.ros/log/`)를 시각 기준으로 직접 대조했다. `patrol_task`의 자체 로그(`python3_13880_..._1789437015223.log`)에서 결정적 단서를 찾았다:

```
10:50:16.32 [patrol_task]   순찰 시작: run_id=32 지점 2개
10:50:16.58 [basic_navigator] Navigating to goal: -2.3089... 1.7272...
   (12분 39초 동안 진행 상황 로그 전무)
11:02:55.99 [patrol_task]   [ERROR] Error raised in execute callback: Failed to publish:
                              publisher's context is invalid, at ./src/rcl/publisher.c:389
11:02:56.00 [patrol_task]   [WARN] Goal state not set, assuming aborted.
```

`journalctl`로 크래시 순간의 전체 파이썬 스택트레이스를 확보하자, 결정적으로 `KeyboardInterrupt`(= SIGINT를 받았다는 명백한 증거)가 `rclpy.spin_until_future_complete()`(`BasicNavigator.goToPose()` 내부) 안에서 발생한 것이 드러났다:

```
File ".../robot_navigator.py", line 152, in goToPose
    rclpy.spin_until_future_complete(self, send_goal_future)
...
KeyboardInterrupt
```

SIGINT를 patrol_task 프로세스에 보내는 코드는 리포지토리 전체에서 딱 한 군데뿐이었다 — `downlink_node.py::_ensure_patrol_task_running()`:

```python
if self._patrol_task_process is not None and self._patrol_task_process.poll() is None:
    self._terminate_process_group(self._patrol_task_process, 'patrol_task')  # SIGINT 발사
    ...
```

그리고 이 함수의 (당시) docstring에 원인이 그대로 쓰여 있었다:

> "patrol_task는 Nav2와 달리 재시작 비용이 아주 작은 `ros2 run` 노드 하나라 ... 이 함수가 불릴 때마다 항상 새로 띄우는 쪽이 더 단순하고 안전하다 — **이 함수는 순찰이 실제로 진행 중일 땐 호출되지 않는다**"

이 전제가 깨진 지점을 찾았다. `_ensure_patrol_task_running()`은 `_start_patrol`(순찰 시작)뿐 아니라 `_prepare_navigation`(`nav_prepare` 메시지 핸들러)에서도 호출된다. 그리고 `nav_prepare`는 서버의 `POST /patrol-runs/prepare-navigation`에서 발행되는데, 이 엔드포인트는 **P6 페이지가 열리거나(`showEvent`) 다른 로봇의 맵으로 전환될 때(`loadMapData`) 자동으로 호출된다** — 즉 사용자가 순찰 버튼을 누르지 않아도, 그냥 대시보드 화면을 켜거나 지도를 전환하기만 해도 발사된다.

R-07의 patrol_task는 10:50:16에 정상적으로 goal을 받아 첫 웨이포인트로 이동을 시작했지만, 12분 39초 동안 그 지점에 도달하지 못하고 있다가(Nav2 자체가 막혀 있었을 가능성), 11:02:55~56경 다른 어떤 동작(팀원 SSH 세션 또는 대시보드 조작 등)이 `nav_prepare` 또는 `patrol_start`를 다시 트리거했고, `_ensure_patrol_task_running()`이 조건 없이 기존 patrol_task를 SIGINT로 죽이면서 미션 자체가 증발했다. 하필 그 크래시가 "실패를 서버에 보고하는" 퍼블리셔 호출과 겹치면서 컨텍스트 자체가 깨져, 서버는 실패 사실조차 통보받지 못하고 `patrol_run`이 `running`에 영원히 박제됐다.

### 해결

`_ensure_patrol_task_running()`에 `override: bool = False` 파라미터를 추가했다:

```python
def _ensure_patrol_task_running(self, map_id: int, override: bool = False) -> bool:
    if self._patrol_goal_handle is not None and not override:
        return True   # 이미 실행 중인 goal은 그대로 두고 아무것도 죽이지 않는다
    ...
```

- `nav_prepare`(`_prepare_navigation`)는 override 없이(기본값 `False`) 호출하므로, 트리거 종류(P6 페이지 열기/맵 전환/서버 맵 메타데이터 갱신)와 무관하게 이제 진행 중인 순찰을 절대 죽이지 않는다.
- `_start_patrol`에도 진입점에 같은 가드를 방어적으로 한 번 더 추가했다: `override` 없이 이미 goal이 실행 중이면 새 요청을 무시하고 경고만 남긴다(서버가 §2의 점유 가드로 이미 막지만, 혹시 모를 경합에 대비한 이중 방어선).
- 정말로 지금 실행 중인 것을 밀어내야 하는 경우(ESP32 SOS, 저배터리 자동 복귀)만 `override=True`를 명시적으로 넘겨 기존처럼 kill+재시작한다(§2 참고).

### 남은 과제 / 참고

- 첫 웨이포인트로 이동을 시작한 뒤 12분 39초 동안 진행 피드백이 전혀 없었던 부분(Nav2가 그 지점에 도달하지 못하고 있었을 가능성)은 이번 조사에서 원인까지는 규명하지 못했다 — 별도로 AMCL/Nav2 쪽 확인이 필요하다.
- 크래시 시각(11:02:07~22)에 `192.168.230.27`에서 `wego` 계정으로 SSH 접속이 있었던 정황을 확인했으나(접속 즉시 종료되는 패턴 — 스크립트성 명령 실행으로 추정), 정확히 무엇이 `nav_prepare`/`patrol_start`를 재발행시켰는지는 확정하지 못했다. 다만 §1 수정으로 트리거 원인과 무관하게 재발을 막았다.

---

## 2. 순찰 우선순위 가드 — 수동 주행/텔레옵 잠금 + ESP32 SOS 우선 끼어들기·자동 재개

§1 사고를 계기로, 다음 4가지 요구사항이 추가로 정리됐다:

1. 주행 스케줄(또는 수동 순찰)이 진행 중이면 대시보드의 수동 주행 버튼(웨이포인트 "이동", "경로 주행")을 비활성화해야 한다.
2. 같은 상황에서 P5의 텔레옵(조이스틱) 조작도 막아야 한다.
3. 모든 팀원 PC의 P6 상단 배너에 "주행 중" 상태가 실시간으로 반영돼야 한다(지금 로봇이 순찰 중인지 팀원 모두가 즉시 알 수 있어야 함).
4. ESP32 SOS 긴급 호출은 예외다 — 순찰 중이어도 그 순찰을 멈추고 즉시 해당 웨이포인트로 이동해야 하며, SOS 대응이 끝나면 원래 순찰을 이어서 재개해야 한다.

### 원인(설계 공백)

서버·로봇 어디에도 "이미 순찰/이동 중인 로봇에 새 요청이 들어오면 어떻게 할지"에 대한 정책이 없었다. §1에서 본 것처럼 로봇 쪽은 무조건 밀어내는 쪽(사고), 서버 쪽은 아예 검사 자체가 없는 쪽이었다. 그리고 "지금 순찰 중"이라는 상태를 실시간으로 다른 대시보드에 알리는 메커니즘도 없었다 — 기존 `patrol_run_created` 브로드캐스트는 P8 로그 탭 전용이었고, P6의 배너는 페이지를 직접 열거나 새로고침해야만, 혹은 `patrol_progress`(첫 웨이포인트 완료 시점) WS를 받아야만 갱신됐다.

### 해결

**서버 (`patrol.py`)**
- `_reject_if_already_patrolling(cur, robot_id)`: 해당 로봇에 `status IN ('running', 'paused')`인 `patrol_run`이 있으면 409로 거부. `start_patrol_run()`(수동 "순찰 시작" 버튼과 스케줄 자동 트리거가 공유)과 `dispatch_drive_to_point()`(P6 "이동")의 일반 경로에 적용.
- `dispatch_drive_to_point()`는 `severity`(ESP32 SOS 표식)가 있으면 이 가드를 타지 않는다. 대신:
  1. 그 로봇의 현재 `running` `patrol_run`을 찾아 `status='paused'`로 바꾼다(완전히 중단하지 않음).
  2. `robot_ws.register_pending_resume(robot_code, paused_run_id)`로 "이 SOS 이동이 끝나면 재개하라"는 표식을 등록한다.
  3. WS 페이로드에 `override=True`를 실어 보내 로봇이 지금 하던 걸 밀어내고 즉시 SOS 지점으로 가게 한다.

**서버 (`robot_ws.py`)**
- SOS 이동의 `patrol_done`(run_id=0)을 받으면, 등록해둔 재개 표식을 확인해 `_resume_patrol_run(run_id)`을 호출한다.
- `_resume_patrol_run`은 `patrol_route.steps`에서 이미 완료한 개수(`waypoints_done`)만큼 건너뛰고 **남은 웨이포인트만** 새 goal로 보낸다. patrol_task는 매번 받은 goal 전체를 처음부터 도는 구조라, 재개 시 로봇에 보고하는 진행률(`i`, 남은 목록 안에서의 인덱스)에 중단 전까지의 개수를 더해야 DB의 누적 진행률이 정확해진다 — `_resume_offsets` 딕셔너리로 이 오프셋을 보정한다.
- 취소(`cancel_patrol_run`)는 `running`뿐 아니라 `paused`도 같이 `aborted`로 정리하고, 재개 표식도 지운다(SOS 도중 취소하면 재개하지 않아야 하므로).
- `GET /patrol-runs?status=running` 조회는 `paused`도 같이 "이 로봇을 점유 중"으로 잡는다(P1 통합관제·P6 버튼 잠금 등 모든 호출부에 일관되게 적용).
- 저배터리 자동 복귀도 같은 이유로 `override=True`를 실어 기존 동작을 유지한다(다만 재개 대상은 아님 — 요청 사양).

**DB**
- `patrol_run.status` CHECK 제약에 `paused`를 추가(`02_alter_patrol_run_status_paused.sql`).

**로봇 (`downlink_node.py`)**
- §1에서 추가한 `override` 파라미터를 그대로 재사용 — SOS·저배터리 자동 복귀 호출부만 `override=True`를 넘겨 지금 실행 중인 patrol_task를 kill+재시작한다.

**대시보드 (`p6_patrol_page.cpp`)**
- 서버가 순찰 상태 변경 시점(시작/일시정지/재개/취소)마다 `patrol_run_updated` 브로드캐스트를 보내도록 하고, P6가 이를 받으면 `refreshRobotState()`(GET /patrol-runs·GET /robots·GET /slam-jobs 재조회)로 DB 기준 상태를 다시 맞춘다 — 이 페이지가 직접 시작하지 않은 주행(다른 팀원 PC, 스케줄러, SOS)도 새로고침 없이 즉시 배너·버튼 잠금에 반영된다.
- `patrol_done`도 낙관적으로 즉시 `isPatrolling_=false`로 바꾸던 것을 같은 재조회 경로로 바꿨다 — SOS가 순찰을 밀어낸 경우 run_id=0인 SOS 이동 자체의 `patrol_done`만 보고 잠금을 풀면 안 되기 때문이다(뒤이어 자동 재개되므로).
- 기존 `canDrive()`가 이미 `isPatrolling_` 기준으로 웨이포인트 "이동"·"경로 주행" 버튼을 잠그고 있어서, `isPatrolling_`만 정확·즉시 반영되면 버튼 차단은 별도 작업 없이 자동으로 따라왔다.

**대시보드 (`p5_map_page.cpp`)**
- `robotPatrolling_` 멤버와 `refreshPatrolState()`를 신설 — 이 로봇이 순찰/이동 중이면(`GET /patrol-runs?status=running`) `TeleopPanel`을 비활성화하고 안내 툴팁을 띄운다. `patrol_run_updated`/`patrol_done` WS 수신 시, 그리고 로봇 전환(`switchToRobot`) 시 갱신한다.

### 검증

- `cmake --build`로 대시보드 빌드 통과, 서버 `app.main` import(라우트 24개 로드) 확인.
- 개발 DB에 `02_alter_patrol_run_status_paused.sql` 적용, 적용 시점에 stuck된 `running`/`paused` row가 없음을 확인.
- 실 로봇(R-07) 배포 후 사용자가 직접 스케줄 주행을 재현해 정상 트리거·정상 주행을 확인함.
