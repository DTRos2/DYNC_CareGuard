# 2026-09-15 트러블슈팅 — R-06 ArUco 도킹 추가 후 CPU 과부하로 Nav2·순찰 재시작 불안정

> 작성자: 최치언
> R-06(터틀봇)에 ArUco 충전소 도킹 정렬 기능을 추가한 직후 "경로 주행 취소 후 다시 주행이 안 된다", "계속 Nav2가 연결이 안 되는 것 같다"는 증상을 리포트받아 실 로봇에 SSH로 들어가 CPU/로그를 직접 확인하며 원인을 좁힌 기록이다.
> 대상 파일: `careguard_turtlebot_agent/careguard_turtlebot_agent/downlink_node.py`
> 대상 로봇: R-06(터틀봇, yeyu@100.110.210.121)

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 경로 주행 중 "주행 취소"를 누르면, 그 이후 다시 "경로 주행"을 눌러도 동작하지 않음 | ArUco 도킹 코드가 카메라 프레임을 노드 시작부터 상시 구독·디코딩하도록 돼 있어(도킹 중이 아닐 때도) 라즈베리파이 CPU를 계속 갉아먹음 — 그 부하로 Nav2 behavior tree가 밀리고, 취소 직후 다운링크 웹소켓 연결이 한 번 끊겼다 재연결되는 사고가 실제로 확인됨. 그 짧은 재연결 틈에 재시작 명령이 오면 유실될 수 있음 | 카메라 구독을 도킹 실행 중에만 켜고(`create_subscription`) 끝나면 바로 끄도록(`destroy_subscription`) 변경 |
| 2 | 계속 "Nav2가 연결이 안 되는 것 같다"는 느낌 | 같은 CPU 과부하로 AMCL의 초기 위치 설정이 매번 `Failed to transform initial pose in time`(타이밍 실패)로 반복 실패 — Nav2 자체는 떠 있지만 위치 추정이 좀처럼 안정되지 않음 | 위 1번 해결로 CPU 여유가 생기면서 완화(근본 원인이 같음) |
| 3 | 위 1·2번을 고쳐도 일반 웨이포인트마다 불필요하게 부하가 남을 소지 | 사용자 요청으로 "일반 웨이포인트에서도 ArUco 정렬·정차" 기능을 한 번 추가했었는데, 실제로는 충전소(id=17) 외에는 물리 마커가 하나도 없어 매 웨이포인트마다 최대 6초씩(`ARUCO_LOST_MARKER_TIMEOUT_SEC`) 카메라 구독+인식 루프만 돌고 아무 효과 없이 CPU만 태움 | 일반 웨이포인트 도킹 시도를 되돌리고, 충전소(kind=charge) 도착 시점에만 다시 동작하도록 원복 |

---

## 1. 카메라 상시 구독으로 인한 CPU 과부하

### 증상

R-06에 ArUco 도킹(충전소 도착 후 마커로 정밀 정렬) 기능을 추가·배포한 뒤, 사용자가 "경로 주행하고 나서 주행 취소하면 다시 경로 주행이 안 된다", 이어서 "경로주행을 하다가 취소하면 계속 nav2가 연결이 안되는거 같다"고 리포트.

### 진단 과정

1. R-06에 SSH로 직접 들어가 `uptime`/`top` 확인 — **load average 5.06**(코어 수를 훌쩍 넘는 값), `downlink_node`가 지속적으로 CPU 29~39%를 차지하고 있었다.
2. `journalctl -u careguard-downlink-r06`에서 Nav2 로그 확인 — `[BehaviorTreeEngine]: Behavior Tree tick rate 100.00 was exceeded!` 경고가 끊임없이 찍힘(Nav2가 자기 목표 주기를 못 맞추고 있다는 뜻).
3. 같은 로그에서 취소 직후 시점을 정확히 짚어보니:
   ```
   [downlink_node]: WP_02 이동 중 취소됨
   [downlink_node]: 다운링크 연결 실패, 3.0s 후 재시도: no close frame received or sent
   [downlink_node]: 서버 다운링크 연결됨: ...  (3초 뒤)
   ```
   취소 직후 다운링크 웹소켓 연결 자체가 비정상 종료(`no close frame`)됐다가 3초 뒤 재연결되는 게 실제로 관측됨 — 이 재연결 틈에 사용자가 다시 "경로 주행"을 눌렀다면 명령이 도착할 연결이 없어 조용히 사라졌을 수 있다.
4. 코드를 다시 보니 `_on_camera_frame_for_dock` 구독(`/camera/image_raw/compressed`)이 `__init__`에서 노드 생성 시점에 한 번 걸리고 끝까지 유지되는 구조였다 — **도킹 중이 아닐 때도 카메라가 프레임을 보낼 때마다(최대 30Hz) `cv2.imdecode`로 풀 디코딩**을 계속 하고 있었다.

### 원인

ArUco 도킹은 "충전소 도착 후 짧은 시간"에만 필요한 기능인데, 카메라 디코딩 구독을 노드 수명 전체로 걸어놔서 평소(순찰 주행 중)에도 계속 CPU를 썼다. 이게 이미 CPU를 많이 쓰는 Nav2(behavior tree)·uplink_node(카메라 relay)와 경합하면서 라즈베리파이 전체가 과부하 상태가 됐고, 그 여파로:
- Nav2 behavior tree가 tick 주기를 못 맞춤
- AMCL 초기 위치 설정이 타이밍 실패를 반복(`Failed to transform initial pose in time`) — "Nav2가 연결이 안 되는 것 같다"는 체감으로 이어짐
- 네트워크/웹소켓 처리까지 밀려 다운링크 연결이 한 번 끊어짐 — "취소 후 재주행 안 됨"으로 이어짐

### 해결

`downlink_node.py`에서 카메라 구독을 상시로 두지 않고, `_dock_with_aruco()` 실행 구간에만 켜고 끄도록 변경:

```python
def _dock_with_aruco(self) -> bool:
    ...
    self._camera_sub_for_dock = self.create_subscription(
        CompressedImage, '/camera/image_raw/compressed',
        self._on_camera_frame_for_dock, qos_profile_sensor_data)
    ...
    try:
        ...
    finally:
        self._publish_cmd_vel(0.0, 0.0)
        if self._camera_sub_for_dock is not None:
            self.destroy_subscription(self._camera_sub_for_dock)
            self._camera_sub_for_dock = None
        self._send({'type': 'dock_result', 'docked': docked})
```

### 검증

배포·재시작 직후 `ps -eo pid,pcpu,comm | grep downlink`로 확인 — 도킹이 실행되지 않는 상태에서 `downlink_node` CPU가 38.9% → **11.7%**로 감소. (남은 load average는 카메라 드라이버(`component_container`)·`uplink_node`의 상시 카메라 relay 때문으로, 이번 변경과 무관한 기존 부하.)

**교훈**: ROS2 구독을 만들 때 "이 콜백이 실제로 필요한 시간대"와 "노드가 살아있는 시간대"를 구분해야 한다 — 특정 짧은 구간(도킹, 캘리브레이션 등)에만 필요한 무거운 처리(이미지 디코딩 등)는 그 구간에만 구독을 열고 끝나면 반드시 닫아야, 평소 CPU 예산을 갉아먹어 다른 실시간 시스템(Nav2, 웹소켓)을 흔드는 연쇄 장애를 피할 수 있다.

---

## 2. 일반 웨이포인트마다 ArUco 도킹을 시도하게 했다가 되돌림

### 배경

사용자가 "일반 웨이포인트일 때도 (마커 인식·정렬·정차가) 동작하게 해달라"고 요청해, 순찰 중 각 웨이포인트 도착 시마다 `_dock_with_aruco()`를 실행하도록 한 번 확장했었다.

### 문제

실제로 물리 마커는 충전소(id=17) 한 곳에만 붙어 있어, 다른 웨이포인트에서는 마커를 찾지 못한 채 `ARUCO_LOST_MARKER_TIMEOUT_SEC`(6초) 동안 카메라 구독+인식 루프만 돌다가 포기 — 매 웨이포인트마다 불필요한 CPU 부하와 지연만 추가되는 상황이었다. 1번 문제(상시 카메라 디코딩)와 겹치면서 CPU 과부하를 더 키운 요인 중 하나였다.

### 해결

일반 웨이포인트 도착 시의 `_dock_with_aruco()` 호출을 제거하고, 원래대로 충전소(kind=charge) 도착 시점(`_drive_to_charge`, `_run_patrol`의 복귀 구간)에서만 실행되도록 되돌렸다. 다른 웨이포인트에도 실제로 마커를 붙이게 되면 그때 다시 추가하면 된다.

### 검증

코드 리뷰 + `py_compile`로 문법 확인, R-06 재배포 후 서비스 정상 기동(크래시 없음) 확인. 실제로 순찰을 끝까지 돌려 취소 후 재시작이 안정적으로 되는지는 다음 실기 테스트에서 재확인 필요.

**교훈**: "요청받은 기능을 넓게 적용"하는 게 항상 좋은 게 아니다 — 이번처럼 그 기능이 의존하는 물리적 전제(마커가 실제로 거기 있는지)가 아직 안 갖춰진 곳까지 넓히면, 효과 없이 비용(CPU·시간)만 느는 경우가 생긴다. 전제가 갖춰지는 시점에 맞춰 범위를 넓히는 게 안전하다.
