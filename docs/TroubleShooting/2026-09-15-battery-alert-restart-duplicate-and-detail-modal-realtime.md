# 2026-09-15 트러블슈팅 — 배터리 임계값 알림 재시작 시 중복 생성, 통합관제 상세 모달 실시간 미반영, 터틀봇(R-06) 위치 갱신 뜸함

> 작성자: 최치언
> 사용자가 "임계값 알림 로그에서 배터리가 떨어질 때마다(특히 1%마다) R-07 알림이 계속 온다"고 리포트해서 조사한 기록. 같은 세션에서 "통합관제 상세 모달에서 로봇 위치·웨이포인트를 바꿔도 계속 같은 값만 나온다"는 리포트도 함께 조사·수정했고, 그 수정을 실기 확인하는 과정에서 "R-07은 위치가 실시간으로 잘 오는데 R-06(터틀봇)만 안 온다"는 세 번째 문제를 추가로 발견해 같이 다뤘다.
> 대상 파일: `pc_careguard/server/app/ws/robot_ws.py`, `pc_careguard/server/app/api/v1/system_event.py`, `pc_careguard/dashboard/src/dialogs/robot_control_modal.{h,cpp}`, `pc_careguard/dashboard/src/pages/p1_overview_page.cpp`, `careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py`
> 대상 로봇: R-06(터틀봇, yeyu@100.110.210.121), R-07(LIMO, wego@192.168.230.42)
> 관련 브랜치(예정): `fix/battery-alert-restart-duplicate`, `fix/robot-detail-modal-realtime`, `fix/turtlebot-position-tf-timer`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 임계값 알림 로그에 배터리가 조금만 떨어져도(사실상 1%마다인 것처럼) R-07 알림이 반복 생성됨 | `_battery_alert_state`가 서버 프로세스 메모리에만 있는 값이라, careguard-server(uvicorn) 재시작마다 초기화됨. 재시작 직후 첫 battery_state 메시지는 무조건 "normal"과 비교돼, 이미 attention/return_needed였던 로봇도 "방금 새로 그 상태가 됐다"고 오판해 중복 로그 생성 | 서버가 그 로봇 상태를 아직 못 본 경우(재시작 직후) DB의 마지막 system_event로 실제 상태를 복원하는 `_infer_battery_state_from_history()` 추가 |
| 2 | 통합관제(P1) 로봇 상세 모달에서 로봇 위치·웨이포인트를 바꿔도 모달을 닫았다 열기 전까진 계속 같은 값만 보임 | `RobotControlModal`이 생성 시 REST로 딱 한 번만 맵·웨이포인트를 가져오고 `/ws/dashboard`를 아예 구독하지 않음. 로봇 현재 위치는 애초에 이 모달에 연결된 적이 없었음(자리만 있고 데이터 없음) | `RobotControlModal`이 `WsClient`를 받아 `robot_position`/`waypoints_changed`/`map_changed`를 구독하도록 배선, `MiniMapView`에 실시간 위치 화살표 렌더링 추가 |
| 3 | 위 수정 후 R-07은 위치가 계속 갱신되는데 R-06(터틀봇)만 거의 안 옴 | 두 로봇이 서로 다른 ROS2 에이전트 코드베이스를 쓰는데, LIMO 쪽엔 이미 0.3초 TF 타이머로 위치를 계속 쏘는 방식이 적용돼 있었고(과거 유사 문제 대응) 터틀봇 쪽은 `/amcl_pose` 구독에만 의존해 AMCL이 뜸하게 pose를 내놓으면 그만큼 위치도 뜸해짐 | 터틀봇 에이전트에도 LIMO와 동일한 TF 타이머 방식(`_on_slam_overlay_tick`)을 포팅 |

---

## 1. 배터리 임계값 알림이 서버 재시작마다 중복 생성됨

### 증상

사용자가 "지금 임계값 알림 로그 쪽에서 배터리가 떨어질때마다 로그가 온다, 1%마다 R-07 알림이 온다"고 리포트.

### 진단 과정

1. 먼저 `_handle_battery_state`(`robot_ws.py`) 자체를 확인 — 상태 전이 시점에만 로그를 남기는 로직과 히스테리시스(±5%p 여유분)가 이미 정상적으로 들어있었고, 관련 테스트 7개(`test_robot_ws_battery.py`)도 전부 통과. 즉 상태 판정 로직 자체는 문제가 없었다.
2. 실제 DB(`system_event`)를 직접 조회해 R-07(robot_id=328)의 최근 기록을 확인:
   ```
   (241, 328, 'battery_attention', '배터리 44% — 주의 임계값(45%) 이하', 2026-09-15 11:42:35)
   (240, 328, 'battery_attention', '배터리 45% — 주의 임계값(45%) 이하', 2026-09-15 11:36:11)
   ```
   같은 `battery_attention`이 `battery_recovered` 없이 6분 24초 간격으로 두 번 연속 찍혀 있었다 — 정상적인 히스테리시스 로직대로라면 발생할 수 없는 패턴(상태가 그대로면 두 번째 로그는 안 남아야 함).
3. `/ws/dashboard`에 직접 붙어 실시간으로 `system_event_created`를 90초간 관찰 — 그 사이엔 새 이벤트가 전혀 생성되지 않음. 즉 "계속 반복 중"인 게 아니라 특정 순간에만 중복이 발생한다는 뜻.
4. 타임스탬프를 다시 보니, 두 번째 로그(11:42:35)가 이 세션에서 사용자 요청으로 `careguard-server`를 재시작한 시각(11:42:32, `systemctl status`로 확인)의 정확히 3초 뒤였다 — 재시작과의 상관관계를 확인.

### 원인

`_battery_alert_state: Dict[str, str] = {}`(robot_ws.py 상단)는 **순수 인메모리 dict**다. 서버(uvicorn) 프로세스가 재시작되면 이 dict도 빈 상태로 초기화된다.

`_handle_battery_state`는 `previous_state = _battery_alert_state.get(robot_code, "normal")`로 이전 상태를 가져왔는데, 재시작 직후 이 dict가 비어 있으면 **로봇이 실제로는 이미 attention/return_needed 상태였어도 무조건 "normal"과 비교**하게 된다. 그러면 재시작 후 들어오는 첫 battery_state 메시지가 (아무 변화도 없었는데) "normal → attention"으로 오판돼 중복 로그가 생긴다. 로봇 배터리가 이미 return_needed 임계값 아래인 상태에서 서버가 재시작되면, 이미 진행 중인 "충전소 복귀" 명령까지 재시작마다 다시 나갈 수 있는 더 심각한 부작용도 이론상 가능하다(이번엔 return_needed 구간에서 재현되진 않았음).

일상적으로 서버가 자주 재시작되는 환경(배포·디버깅 세션 등)에서는 이 증상이 마치 "배터리가 조금만 떨어져도 계속 알림이 온다"처럼 체감된다.

### 해결

`robot_ws.py`에 `_infer_battery_state_from_history(cur, robot_id)` 헬퍼를 추가 — 이 프로세스가 그 로봇의 상태를 아직 한 번도 못 봤을 때(= `_battery_alert_state`에 키가 없을 때)만, DB에서 그 로봇의 가장 최근 배터리 관련 `system_event`(`battery_attention`/`battery_return_needed`/`battery_recovered`)를 조회해 실제 상태를 복원한다:

```python
def _infer_battery_state_from_history(cur, robot_id: int) -> str:
    cur.execute(
        """
        SELECT event_type FROM system_event
        WHERE robot_id = %(robot_id)s
          AND event_type IN ('battery_attention', 'battery_return_needed', 'battery_recovered')
        ORDER BY id DESC LIMIT 1
        """,
        {"robot_id": robot_id},
    )
    row = cur.fetchone()
    if row is None:
        return "normal"
    if row["event_type"] == "battery_return_needed":
        return "return_needed"
    if row["event_type"] == "battery_attention":
        return "attention"
    return "normal"
```

`_handle_battery_state`에서는 이 값을 조회 즉시 `_battery_alert_state`에 캐시해두므로, 프로세스당 로봇 하나에 DB 조회가 한 번만 더 든다.

같은 김에 사용자 요청으로 `system_event.py`의 `EVENT_TYPE_LABELS["battery_return_needed"]` 라벨도 `"배터리 복귀 필요"` → `"충전소 복귀 필요"`로 변경(실제로 로봇이 복귀하는 대상은 "충전소"이지 "배터리"가 아니라는 지적).

### 검증

- `pytest tests/test_robot_ws_battery.py` 기존 7개 전부 통과(회귀 없음 확인).
- 수정한 코드로 `careguard-server`를 다시 재시작해 직접 재현 — 재시작 시점에 R-07이 여전히 attention 상태(44%)였는데도, 재시작 후 `system_event` 최대 id가 그대로 241로 유지됨을 확인(수정 전이었다면 242가 새로 생겼을 상황).
- `GET /system-events` 응답에서 `event_type`이 `"충전소 복귀 필요"`로 바뀐 것도 확인.

**교훈**: 상태 판정에 쓰는 값을 프로세스 메모리에만 두면, "그 상태 판정 로직 자체는 맞다"는 게 "재시작에도 안전하다"를 보장하지 않는다. 재시작(배포·크래시·수동 재기동) 직후의 "첫 메시지" 경로는 별도로 취급해야 하고, 이미 DB에 영속화된 이력이 있다면 그걸로 인메모리 상태를 복원하는 쪽이 "일단 normal로 가정"보다 안전하다.

---

## 2. 통합관제 상세 모달 — 맵/위치/웨이포인트 실시간 미반영

### 증상

사용자가 "통합관제에서 상세 모달에 있는 맵을 실시간으로 적용시켜줘, 로봇 위치하고 웨이포인트하고 변경했는데 계속 똑같은 값만 가지고 온다"고 리포트.

### 진단 과정

`pc_careguard/dashboard/src/dialogs/robot_control_modal.{h,cpp}`와 호출부(`p1_overview_page.cpp`)를 확인:

- `RobotControlModal` 생성자가 `loadChargeWaypoint()`를 딱 한 번 호출해 `GET /maps/active` + `GET /waypoints`로 맵 이미지·웨이포인트를 REST로 받아온다. 이걸 다시 불러오는 타이머·이벤트 핸들러가 전혀 없었다.
- `RobotControlModal`은 `WsClient` 인스턴스를 아예 모르는 클래스였다 — `messageReceived`를 구독하는 코드가 없으니 `/ws/dashboard`가 실시간으로 내려주는 `robot_position`/`waypoints_changed`/`map_changed`를 받을 방법 자체가 없었다.
- "로봇 현재 위치"는 더 근본적으로 연결이 빠져 있었다 — `Data::positionLabel`을 채우는 코드가 호출부에 아예 없고(주석에 "서버에 그 자체를 재는 경로가 없어 못 채운다"고 명시돼 있었으나, 실제로는 `/ws/dashboard`의 `robot_position` 브로드캐스트로 채울 수 있었음), `MiniMapView`에는 웨이포인트 점을 그리는 기능만 있고 로봇의 실시간 위치를 그리는 기능 자체가 없었다.

### 원인

모달을 열어둔 채로는 물론이고, 모달을 껐다 다시 열어도(REST 재요청 자체는 나가지만) "로봇 현재 위치"는 애초에 파이프라인이 없어 절대 반영될 수 없는 구조였다. 웨이포인트/맵은 "모달을 새로 열 때만" 최신화되고 열려있는 동안은 절대 안 바뀌는 구조였다.

### 해결

- `RobotControlModal`이 `WsClient*`를 생성자로 받도록 변경(`P1OverviewPage`가 이미 갖고 있는 공유 `wsClient_`를 그대로 넘김 — 모달마다 소켓을 새로 열지 않음).
- `RobotControlModal::handleWsMessage()` 추가:
  - `robot_position`(해당 `robot_code`) → `MiniMapView::setLivePose(x, y, yaw)`로 실시간 위치 화살표 갱신 + "현재 위치" 라벨 텍스트 갱신.
  - `waypoints_changed`(해당 `map_id`) / `map_changed`(해당 `robot_code`) → 기존 `loadChargeWaypoint()`를 재호출해 맵·웨이포인트를 다시 받아옴.
- `MiniMapView`에 `setLivePose()`와 화살표 렌더링 추가 — `SlamMapView::drawLivePose`(P5)와 같은 좌표 변환 규칙(맵 프레임 y 위쪽+ → 화면 y 아래쪽+이라 y 부호 반전)을 재사용.
- `P1OverviewPage`의 모달 생성 코드에서 `new RobotControlModal(data, this)` → `new RobotControlModal(data, wsClient_, this)`로 변경.

### 검증

- `cmake --build`로 빌드 성공.
- `careguard_dashboard`를 새 바이너리로 재시작(기존 프로세스 종료 후 재기동, DISPLAY/XAUTHORITY 유지).
- `/ws/dashboard`에 직접 붙어 실제로 `robot_position` 메시지가 오는지 확인 — R-07(Nav2 가동 중)은 10초에 25~34건(약 0.3초 간격)으로 잘 들어옴. **UI 화면 자체(모달 안의 화살표가 실제로 움직이는지)는 이 세션에서 눈으로 직접 확인하지 못했음 — 사용자가 대시보드에서 직접 열어 확인 필요.**
- 확인 도중 "R-07은 잘 오는데 R-06은 안 온다"는 게 발견돼 3번 항목으로 이어짐.

**교훈**: "모달/카드를 새로 열 때만 데이터를 받아온다"는 구조는 소규모 정적 정보엔 괜찮지만, 원래 실시간이어야 하는 값(로봇 위치처럼 초 단위로 바뀌는 값)을 REST 스냅샷으로 다루면 "열 때마다 오래된 값이 반복해서 보이는" 것처럼 느껴진다 — 이런 값은 처음부터 WS 구독이 있는지를 먼저 확인해야 한다.

---

## 3. R-06(터틀봇) 위치 실시간 미반영 (R-07은 정상)

### 증상

2번 수정 후 `/ws/dashboard`로 직접 확인한 결과, R-07은 위치가 계속 들어오는데 R-06(터틀봇)만 10초에 1건 수준으로 거의 안 들어옴.

### 진단 과정

1. `/ws/dashboard`에 10초간 붙어 타입별 카운트 확인: `('robot_position', 'R-07')` 34건, `('robot_position', 'R-06')` 1건.
2. 두 로봇의 에이전트 코드가 서로 다른 패키지라는 걸 확인 — R-07은 `robot_careguard_ws/src/careguard_agent`(LIMO), R-06은 `careguard_turtlebot_agent`(터틀봇).
3. LIMO 쪽(`uplink_node.py`)엔 `SLAM_OVERLAY_INTERVAL_SEC=0.3`초 타이머로 `map->base_link` TF를 직접 읽어 위치를 쏘는 `_on_slam_overlay_tick`이 이미 있었다(2026-09-14 유사 문제 대응으로 추정). 터틀봇 쪽은 `/amcl_pose`(`PoseWithCovarianceStamped`) 토픽 구독(`_on_pose`)에만 의존 — AMCL이 새 추정치를 뜸하게 내놓으면(정지 중이거나 스캔 매칭 갱신이 뜸할 때) 위치 전송도 그만큼 뜸해진다.
4. 터틀봇 에이전트엔 이미 `tf2_ros` 버퍼/리스너와 라이다 스캔 오버레이용 0.3초 타이머(`SCAN_OVERLAY_INTERVAL_SEC`)가 있어서, LIMO와 같은 방식을 포팅하기 위한 인프라는 이미 갖춰져 있었다.

### 원인

두 로봇 플랫폼의 에이전트 코드가 애초에 다르게 짜여 있었고, LIMO 쪽에만 "AMCL pose 갱신이 뜸해도 위치를 계속 보완 전송"하는 TF 타이머 방식이 들어가 있었다 — 터틀봇 쪽엔 이 수정이 포팅되지 않은 상태였다.

### 해결

`careguard_turtlebot_agent/uplink_node.py`:

- 기존 `create_timer(SCAN_OVERLAY_INTERVAL_SEC, self._send_scan_overlay)`를 `self._on_slam_overlay_tick`으로 교체.
- `_on_slam_overlay_tick()` 신설 — `map->base_link` TF를 조회해 성공하면 `{'type': 'position', ...}`을 전송하고, 이어서 기존 `_send_scan_overlay()`도 그대로 호출(LIMO의 `_on_slam_overlay_tick`과 동일한 구조).
- `_on_pose`(`/amcl_pose` 구독)는 그대로 유지 — 두 경로가 같이 있어도 더 자주 갱신될 뿐 문제없다(LIMO도 같은 패턴).

R-06(`yeyu@100.110.210.121`)에 `rsync` + `colcon build --packages-select careguard_turtlebot_agent` + `systemctl restart careguard-uplink-r06`으로 배포.

### 검증 및 후속 이슈

배포 직후 다시 `/ws/dashboard`로 확인했는데도 R-06 위치가 여전히 거의 안 들어와서 추가로 파봄:

- `ros2 run tf2_ros tf2_echo map base_link`가 "Invalid frame ID map — frame does not exist"를 계속 반환 — **그 시점에 R-06은 Nav2/AMCL 자체가 켜져 있지 않았다**(`ros2 node list`에 AMCL/bt_navigator 관련 노드가 없음).
- 서버 `GET /robots`가 이 로봇을 `is_nav_running: true`로 보여주고 있었는데, 실제 ROS 노드 목록과는 달랐다 — Nav2는 로봇 부팅 시 자동으로 뜨지 않고, 대시보드에서 "위치 보정"/순찰 시작 등을 눌러야 뜨는 구조라는 걸 이번에 확인함(별도로 조사가 더 필요한 부분 — `is_nav_running`이 실제 노드 생존이 아니라 서버가 마지막으로 보낸 `nav_status` 메시지 값만 들고 있어서 괴리가 생길 수 있음).
- 세션 도중 R-06 전원을 껐다(사용자 요청) 다시 켠 뒤 서비스(`bringup`/`uplink`/`downlink`) 정상 기동은 확인했으나, Nav2를 다시 띄워 "TF 타이머로 위치가 실제로 0.3초 간격으로 잘 오는지"까지는 **이번 세션에서 최종 확인하지 못했다** — 다음에 R-06에서 위치 보정/순찰을 한 번 띄운 뒤 재확인 필요.

**교훈**: 로봇 플랫폼이 여러 종류(LIMO/터틀봇)인 저장소에서는, 한쪽 플랫폼에서 이미 고친 문제가 다른 쪽 에이전트 코드베이스엔 자동으로 반영되지 않는다는 걸 항상 의심해야 한다 — 같은 증상이 "이미 고쳐진 적 있는 문제"처럼 보이면, 그 수정이 정말 모든 로봇 종류의 코드에 다 들어갔는지부터 확인해야 한다. 또한 `is_nav_running` 같은 "마지막으로 받은 상태값"은 실제 노드 생존 여부와 어긋날 수 있다는 것도 이번에 실측으로 확인됐다.
