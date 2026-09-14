# 2026-09-14 트러블슈팅 — 대시보드 로봇 상세/이벤트 UI 정리, 터틀봇 라이다·다중 웨이포인트 순찰

> 작성자: 최치언
> 같은 날짜의 `2026-09-14-turtlebot-r06-nav2-recovery.md`(Nav2 자체를 살린 기록) 이후,
> 실제로 대시보드에서 로봇을 써보면서 나온 후속 리포트들을 정리한다. 대시보드 쪽
> UI 문제 4건과, 터틀봇에 라이다·다중 웨이포인트 순찰을 새로 추가하면서 겪은
> 진짜 동시성 버그(로봇이 "주행 취소"를 눌러도 안 멈추는 문제) 하나가 핵심이다.
> 대상 파일: `pc_careguard/dashboard/src/dialogs/robot_control_modal.cpp`,
> `pc_careguard/dashboard/src/pages/p1_overview_page.cpp`,
> `pc_careguard/dashboard/src/pages/p2_event_page.{h,cpp}`,
> `pc_careguard/dashboard/src/pages/p6_patrol_page.cpp`,
> `pc_careguard/dashboard/src/windows/mainwindow.cpp`,
> `careguard_turtlebot_agent/careguard_turtlebot_agent/{uplink_node.py,downlink_node.py}`,
> `careguard_turtlebot_agent/package.xml`
> 대상 로봇: R-06(터틀봇, yeyu@100.110.210.121)

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 통합관제 상세보기 맵 미리보기에 웨이포인트 원만 뜨고 실제 지도(벽) 배경이 없음 | `RobotControlModal::MiniMapView`가 웨이포인트 좌표 범위로만 정규화해 그리고, 애초에 배경 이미지를 안 불러오게 설계돼 있었음 | P6WaypointCanvas와 같은 방식(fetchBytes+PgmImageLoader+CanvasGeometry)으로 실제 맵 이미지를 불러와 그리도록 확장 |
| 2 | 로봇이 오프라인이 돼도 배터리%가 마지막 값에 고정된 채 계속 표시됨 | `row.level`/`row.mission`은 오프라인이면 덮어쓰는데 `row.battery`만 그 규칙이 빠져 있었음 | 오프라인이면 배터리도 -1("-")로 처리 |
| 3 | 로봇 상세 모달의 "이벤트로 이동"을 누르면 이벤트 페이지로 넘어가도 모달이 계속 떠 있고, 검색은 안 되고, 상세 모달이 중복으로 뜸 | 같은 동작을 하는 버튼이 2개(중복), 클릭해도 모달 자신은 안 닫힘, event_uid를 무시하고 페이지 전환만 함, 이후엔 상세 모달까지 자동으로 열려 모달이 연달아 뜸 | 중복 버튼 제거, 클릭 시 모달 닫기, P2에 `filterToEventUid()`(검색만, 상세 모달은 안 엶) 신설해 연결 |
| 4 | "실시간 알림"에 어제 이전 이벤트가 섞여 보임 | `GET /events`가 날짜 제한 없는 전체 이력이고, 클라이언트도 "최신 20건"으로만 자름 — 당일 이벤트가 20건 미만인 날엔 예전 이벤트가 채워짐 | `populateAlertFeed()`에서 20건 컷 전에 로컬 기준 오늘 날짜만 남기도록 필터 추가(로봇별 "진행 중 이벤트" 배지는 날짜 무관하게 그대로 둠) |
| 5 | R-06 초기 위치를 잡아도 P6 지도에 라이다 스캔 점이 전혀 안 뜸 | P6의 실시간 라이다 오버레이(`slam_scan_update`)가 LIMO `uplink_node`에만 구현돼 있고 터틀봇엔 아예 없었음 | LIMO와 동일하게 `/scan` 구독 + `tf2_ros`로 map→스캔 프레임 TF 조회 + 좌표 변환 후 전송하는 로직을 터틀봇 `uplink_node.py`에 이식 |
| 6 | 경로를 만들어 "경로 주행"을 눌러도 터틀봇이 가만히 있음 | 터틀봇 `downlink_node`가 웨이포인트가 든 진짜 순찰(`patrol_start`)을 아예 처리 안 하고 경고만 남기고 무시하도록 만들어져 있었음(배터리 자동 복귀만 지원) | LIMO `patrol_task.py`의 상태 기계(웨이포인트 순서대로 이동 → 실패 시 중단 → 완료 시 충전소 복귀 → 진행률 보고)를 영상·음성 연동 없이 이식(`_run_patrol`) |
| 7 | 순찰 중 버튼이 계속 "경로 주행"으로 남아 있어 멈출 방법이 없음 | `testBtn_`가 순찰 상태와 무관하게 항상 같은 라벨·같은 핸들러였음(팝오버의 웨이포인트별 토글과 달리 경로 전체 토글이 없었음) | 팝오버와 같은 패턴으로 `isPatrolling_ && drivingWaypointId_.isEmpty()`일 때 "주행 취소"로 토글, 클릭 시 `cancelCurrentDrive` 재사용 |
| 8 | **"주행 취소"를 눌렀는데 로봇이 계속 움직임(진짜 안전 문제)** | `rclpy.spin(node)`와 `BasicNavigator.goToPose()` 내부의 `rclpy.spin_until_future_complete()`가 둘 다 인자를 안 주면 **프로세스 전체가 공유하는 전역 executor**를 쓴다 — 메인 스레드가 이미 그 executor를 돌리고 있는데 순찰 스레드가 동시에 또 돌리려다 `ValueError: generator already executing`로 매번 즉시 크래시, 크래시한 스레드가 `_nav_active`를 바로 되돌려서 "취소할 게 없다"고 나오지만 Nav2는 이미 목표를 접수해 실제로는 계속 주행 중이었음 | `main()`이 `rclpy.spin(node)` 대신 이 노드 전용 `SingleThreadedExecutor`를 직접 만들어 쓰도록 수정 — 전역 executor는 `self._navigator`의 spin 전용으로 남게 됨 |

---

## 1. 로봇 상세보기 맵 미리보기에 실제 배경 이미지가 없음

### 원인

`robot_control_modal.cpp`의 `MiniMapView`는 처음부터 "지금 웨이포인트가 어디 있는지"만 가볍게 보여주려고 만든 위젯이라, 웨이포인트 좌표 범위로 화면을 정규화해 원+라벨만 그리고 실제 맵 이미지(점유 격자)는 아예 안 불러왔다.

### 해결

P6 `WaypointCanvas`가 이미 쓰고 있는 절차(`ApiClient::fetchBytes`로 맵 이미지 바이트 수신 → `PgmImageLoader::loadFromData`로 디코딩 → `CanvasGeometry::imagePlacement`/`metersToCanvas`로 실제 map 프레임 미터 좌표를 캔버스 픽셀로 환산)를 `MiniMapView`에도 그대로 옮겼다. 맵 이미지가 없는 로봇(미등록 등)은 기존처럼 웨이포인트 좌표 범위 기준 정규화로 폴백한다. `RobotControlModal::loadChargeWaypoint()`가 `fetchActiveMaps`로 받은 `MapInfo`의 `imageUrl`/`resolution`/`originX`/`originY`를 그대로 `MiniMapView::setMapImage()`에 넘긴다.

### 검증

R-06 상세보기를 스크린샷으로 확인 — 실제 건물 도면이 배경으로 뜨고 웨이포인트가 정확한 위치에 겹쳐 그려짐을 확인했다.

---

## 2. 오프라인 로봇의 배터리가 마지막 값에 고정 표시됨

### 증상

"LIMO 통신이 끊겼는데 배터리 값을 그대로 띄우고 있다"는 리포트.

### 원인

`p1_overview_page.cpp::rebuildRowsFromData()`에서 로봇이 오프라인이면 위험도 배지(`row.level`)와 임무 텍스트(`row.mission = "통신 두절"`)는 이미 덮어쓰도록 돼 있었는데, `row.battery = liveBattery_.value(robot.code, -1);`만 이 규칙이 빠져 있어 `liveBattery_`(마지막으로 받은 `battery_state`)를 그대로 계속 보여주고 있었다.

### 해결

```cpp
row.battery = robot.isOnline ? liveBattery_.value(robot.code, -1) : -1;
```

-1이면 테이블이 "-"로 표시한다(기존 `if (row.battery >= 0)` 분기 재사용).

### 검증

실제로 R-07이 오프라인인 상태에서 확인 — R-07(및 다른 오프라인 로봇들)은 "—"로, 온라인인 R-06만 실제 배터리 85%가 정상 표시됨을 스크린샷으로 확인했다.

---

## 3. 로봇 상세 모달의 이벤트 버튼 정리

### 증상 3가지

1. "이벤트로 이동"을 눌러도 페이지 전환만 되고 검색이 안 됨(event_uid를 무시).
2. 같은 동작을 하는 버튼이 모달 안에 2개("이벤트 상세 열기 →"와 "이벤트로 이동") 있었음.
3. 눌러서 이벤트 페이지로 넘어가도 로봇 상세 모달 자신은 안 닫히고 뒤에 계속 떠 있음.
4. (뒤에 고치면서 새로 발견) 위 3가지를 고치고 나니, 이번엔 로봇 상세 모달이 닫히자마자 이벤트 상세 모달(`EventDetailModal`)이 곧바로 또 떠서 모달이 연달아 두 번 뜨는 것처럼 보임.

### 원인 및 해결

- `mainwindow.cpp`가 `P1OverviewPage::eventNavigationRequested`를 받고도 `eventUid`를 버리고 `switchToPage(1)`만 하고 있었다 — P4 영상 페이지의 같은 시그널 처리(`eventPage_->openEventByUid(eventUid)`)와 다르게 방치돼 있었음.
- `robot_control_modal.cpp`에서 하단 "이벤트로 이동" 버튼을 제거하고, 이벤트 박스 안 "이벤트 상세 열기 →" 하나로 통일. 클릭 시 `close()`도 같이 호출하도록 수정.
- `mainwindow.cpp`를 처음엔 P4와 똑같이 `openEventByUid()`로 연결했더니, 이 함수가 검색+상세 모달 자동 오픈까지 다 하는 함수라 "로봇 모달이 닫히자마자 또 다른 모달이 뜬다"는 새 리포트가 나왔다. `P2EventPage`에 `filterToEventUid()`(검색·기간만 맞추고 상세 모달은 안 엶)를 새로 분리해 P1 쪽은 이걸 쓰도록 최종 정리했다 — P4의 기존 동작(상세 모달 자동 오픈)은 그대로 유지.

---

## 4. "실시간 알림"에 다른 날짜 이벤트가 섞임

### 원인

서버 `GET /events`(`pc_careguard/server/app/api/v1/event.py`)가 날짜 필터 없이 전체 이력을 `occurred_at DESC`로 반환하고, 대시보드도 그중 최신 20건(`kMaxAlerts`)만 자른다. 오늘 이벤트가 20건 미만인 날엔 어제 이전 이벤트가 그 자리를 채운다. (실제로 확인한 날은 오늘 하루에만 87건이 쌓여 있어 우연히 "당일만" 보이는 것처럼 보였을 뿐이었다.)

### 해결

`P1OverviewPage::populateAlertFeed()`에서 20건 컷 전에 `event.occurredAt.toLocalTime().date() != QDate::currentDate()`인 이벤트를 건너뛰도록 필터를 추가했다. 로봇별 "진행 중 이벤트"(미해결 이벤트, `latestUnresolvedEventByRobot`)는 날짜와 무관하게 계속 보여야 해서 `events_` 원본 자체는 건드리지 않고 이 피드 함수 안에서만 걸렀다.

---

## 5. 터틀봇에 P6 라이다 스캔 오버레이가 없음

### 원인

P6 순찰 지도의 실시간 라이다 스캔 표시(`slam_scan_update` WS 메시지, 같은 날 다른 세션이 PR #81로 추가)는 LIMO `uplink_node.py`(`_send_scan_overlay`)에만 구현돼 있고, 터틀봇 `uplink_node.py`는 `/amcl_pose`·`/battery_state`·카메라만 구독해서 애초에 스캔 데이터를 보낸 적이 없었다.

### 해결

LIMO와 동일한 방식을 그대로 이식했다: `/scan`(LaserScan, `qos_profile_sensor_data`) 구독 → `tf2_ros.Buffer`/`TransformListener`로 `map→스캔프레임` TF 조회 → 각 유효 레인지를 map 프레임 좌표로 변환(최대 450포인트로 다운샘플) → `slam_scan_update` 전송. `position`(`_on_pose`)은 이미 `/amcl_pose`를 직접 구독해 TF가 필요 없지만, 스캔 포인트는 스캔 프레임 기준이라 map 프레임 변환에 TF 조회가 꼭 필요해서 `tf2_ros`가 이번에 새로 들어갔다(`package.xml`에도 의존성 추가).

### 검증

R-06에 배포 후 서버 `/ws/dashboard`에 직접 붙어 10초간 관찰 — `slam_scan_update(R-06)` 33건 수신, 포인트도 실제 map 프레임 좌표로 정상 변환됨을 확인했다.

---

## 6. 터틀봇 다중 웨이포인트 순찰(경로 주행) 미지원

### 원인

터틀봇 `downlink_node.py::_start_patrol`은 애초에 "배터리 낮으면 충전소로 복귀"만 되게 좁게 설계돼 있어서, 웨이포인트가 든 `patrol_start`(P6 "경로 주행"이 보내는 진짜 순찰 요청)가 오면 경고만 남기고 그냥 무시했다. 로그로 실제 확인:

```
[WARN] 터틀봇은 아직 웨이포인트 순찰/이동을 지원하지 않습니다 — 배터리 자동 복귀(빈 웨이포인트 + return_to_charging)만 처리합니다
```

### 해결

LIMO `careguard_mission/patrol_task.py::_execute`의 상태 기계를 가져오되, 그쪽이 액션 서버 하나로 같이 처리하는 영상 녹화(`careguard_perception`)·음성 안내(`careguard_hmi`)는 뺐다(둘 다 LIMO 전용 워크스페이스 패키지라 터틀봇엔 없음). `_run_patrol()`을 새로 추가해:

- 웨이포인트를 순서대로 `goToPose`로 이동
- 취소 확인은 매 지점마다(LIMO의 `cancel_check` 패턴과 동일)
- 하나라도 실패하면 즉시 중단(성공한 걸로 잘못 기록되는 걸 막기 위해 — LIMO도 2026-09-08에 같은 이유로 고친 부분)
- 끝나면(`return_to_charging`) 충전소로 복귀
- `patrol_progress`/`patrol_done`을 서버에 보고(`robot_ws.py`가 DB의 `patrol_run` 행과 대시보드로 그대로 릴레이)

기존 `_drive_to_charge`(충전 복귀 단독 주행)와 목적지 하나로 가는 로직(`goToPose`+대기+취소확인)을 `_go_to_pose_and_wait()`로 공유하도록 리팩터링해 중복을 없앴다.

---

## 7. 순찰 중 "경로 주행" 버튼이 멈추는 용도로 안 바뀜

### 원인

`p6_patrol_page.cpp`의 `testBtn_`("경로 주행")는 순찰 중이든 아니든 늘 같은 라벨·같은 클릭 핸들러(`runTestRun`)였다. 웨이포인트 팝오버의 "경로 이동"↔"주행 취소" 토글(`refreshPopoverDriveButton`/`WaypointPopover::setDriveButtonState`)은 이미 있었지만, 그건 팝오버로 연 개별 웨이포인트 수동 이동 전용이고 경로 전체 주행에는 없었다.

### 해결

`updateDrivingButtonsEnabled()`에 `WaypointPopover::setDriveButtonState`와 같은 패턴을 추가했다 — `isPatrolling_ && drivingWaypointId_.isEmpty()`(팝오버가 시작한 개별 이동이 아니라 이 버튼이 시작한 경로 전체 주행이라는 뜻)일 때 "주행 취소"(danger variant)로 바뀌고, 취소는 오프라인이어도 항상 누를 수 있게 뒀다(DB의 stuck 기록 정리를 위해, 팝오버와 같은 이유). `runTestRun()` 맨 앞에 이 상태면 새 주행을 시작하는 대신 `cancelCurrentDrive(QString())`(팝오버의 "주행 취소"와 같은 `/patrol-runs/cancel` 경로)를 호출하도록 분기를 추가했다.

---

## 8. "주행 취소"를 눌러도 로봇이 계속 움직임 — 진짜 동시성 버그

### 증상

6번을 배포하고 실제로 "경로 주행"을 눌러 로봇이 웨이포인트로 향하는 중에 "주행 취소"를 눌렀는데, 로봇이 멈추지 않고 목표 지점까지 그대로 주행을 계속함(끝까지 가서 "Reached the goal!"로 정상 종료).

### 진단

R-06 `downlink_node` 로그를 시간순으로 확인:

```
[INFO] Navigating to goal: 0.172... (basic_navigator)
Exception in thread Thread-2 (_run_patrol):
...
ValueError: generator already executing
[WARN] 취소할 진행 중인 주행이 없습니다   ← 사용자가 "주행 취소"를 누른 시점
```

첫 웨이포인트로 가는 `goToPose()` 호출 자체가 즉시 예외로 죽었다 — 그것도 재현할 때마다 매번, 우연한 타이밍이 아니라 항상. 로봇에 설치된 `nav2_simple_commander/robot_navigator.py::goToPose`와 rclpy 소스를 직접 열어 확인했다:

```python
# BasicNavigator.goToPose
rclpy.spin_until_future_complete(self, send_goal_future)

# rclpy.spin_until_future_complete / rclpy.spin (둘 다 실 로봇에서 소스 직접 확인)
executor = get_global_executor() if executor is None else executor
```

`rclpy.spin()`과 `rclpy.spin_until_future_complete()`는 `executor` 인자를 안 주면 **프로세스 전체가 공유하는 전역 `SingleThreadedExecutor` 하나**(`rclpy.get_global_executor()`)를 쓴다. `downlink_node.py`의 `main()`은 `rclpy.spin(node)`로 메인 스레드에서 이 전역 executor를 계속 돌리고 있었는데, `_run_patrol`(백그라운드 스레드)이 `self._navigator.goToPose()`를 부르면 그 안에서 똑같이 이 전역 executor를 또 돌리려 한다 — `SingleThreadedExecutor`는 여러 스레드가 동시에 돌리는 걸 지원하지 않아서 내부 콜백 제너레이터(`_cb_iter`)가 두 스레드에서 동시에 진행되며 `ValueError: generator already executing`로 즉시 깨진 것이다.

### "취소"가 왜 안 먹혔는지

크래시한 스레드는 `_run_patrol`의 `finally: self._nav_active = False`로 곧바로 "지금 아무것도 안 하고 있다"는 상태가 됐지만, **Nav2(`bt_navigator`)는 그 전에 이미 목표를 접수해서 실제로는 계속 주행 중**이었다. `_stop_patrol`은 `self._cancel_requested`만 세팅하고 실제 취소(`cancelTask()`)는 살아있는 주행 스레드가 그 플래그를 보고 직접 호출하는 구조인데, 그 스레드가 이미 죽어서 아무도 `cancelTask()`를 부를 사람이 없었다 — 그래서 "취소할 진행 중인 주행이 없습니다"만 남기고 로봇은 Nav2 레벨에서 계속 그 목표를 향해 갔다.

### 해결

`main()`이 `rclpy.spin(node)` 대신 이 노드 전용 `SingleThreadedExecutor`를 직접 만들어 쓰도록 바꿨다:

```python
executor = SingleThreadedExecutor()
executor.add_node(node)
try:
    executor.spin()
finally:
    executor.remove_node(node)
    node.destroy_node()
    ...
```

이러면 전역 executor는 `self._navigator`(BasicNavigator)의 `spin_until_future_complete` 전용으로 남고, 메인 스레드의 `node` spin과 더 이상 같은 executor를 공유하지 않는다.

### 검증

R-06에 배포·재시작 후 정상 기동(연결) 로그까지 확인했다 — **실제 순찰 재테스트(로봇을 다시 움직여 취소가 정말 먹히는지)는 아직 안 했다.** 다음에 반드시 재검증 필요.

**교훈**: `rclpy.spin()`/`rclpy.spin_until_future_complete()`를 executor 없이 여러 곳(메인 스레드 + 백그라운드 스레드)에서 섞어 쓰면, 겉으로는 서로 무관해 보여도 내부적으로 프로세스 전역 executor 하나를 공유해서 충돌한다. `nav2_simple_commander.BasicNavigator`처럼 내부에서 `rclpy.spin_until_future_complete(self, ...)`를 executor 없이 부르는 라이브러리를 멀티스레드 노드에서 쓸 땐, 노드 자신의 메인 spin 루프에 반드시 명시적 executor를 따로 줘야 한다.

---

## 부록: 충전 웨이포인트가 테스트 중 실수로 삭제됨

"경로 주행" 버튼이 "주행 취소"로 안 바뀐다"는 리포트를 조사하는 과정에서, `map_id=31`(R-06 활성 맵)의 충전 스테이션 웨이포인트(WP_01, 초기위치)가 어느 시점엔가 삭제되어 있는 것을 발견했다(테스트 중 실수로 지워진 것으로 추정). `runTestRun()`은 충전 웨이포인트가 없으면 메시지 박스만 띄우고 조용히 리턴하도록 돼 있어서, 7번에서 추가한 "주행 취소" 토글 코드까지 아예 도달하지 못했던 것 — 버튼 로직 버그가 아니라 데이터 문제였다. 원래 좌표(R-07에서 복사해올 때 썼던 값)로 동일하게 재생성해 복구했다:

```
POST /waypoints {"map_id":31,"code":"WP_01","name":"초기위치","kind":"charge",
  "pos_x":0.013981579479185768,"pos_y":-2.054039602596146,"yaw":-1.5137155443886319}
```
