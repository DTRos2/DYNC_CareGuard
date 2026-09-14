# 2026-09-14 트러블슈팅 — LIMO 배터리 % 오표시, 배터리 주의 알림 반복 생성, 로봇 전환 시 카메라 화면 잔상

> 작성자: 최치언
> 사용자가 "LIMO 배터리가 물리 게이지로는 4칸 중 1칸 반(~35%) 남았는데 대시보드엔 1%로 뜬다"고 리포트해서 조사한 기록. 원인을 찾는 과정에서 SSH로 로봇에 직접 들어가 실측 전압을 확인했고, 같은 세션에서 "배터리 주의/정상 회복 알림이 번갈아 반복해서 뜬다", "순찰·지도 화면에서 카메라 연결 안 된 로봇으로 전환해도 이전 로봇의 화면이 그대로 남는다"는 두 가지 증상도 함께 조사·수정했다.
> 대상 파일: `robot_careguard_ws/src/careguard_agent/careguard_agent/uplink_node.py`, `pc_careguard/server/app/ws/robot_ws.py`, `pc_careguard/server/tests/test_robot_ws_battery.py`, `pc_careguard/dashboard/src/pages/p5_map_page.cpp`, `pc_careguard/dashboard/src/pages/p6_patrol_page.cpp`, `pc_careguard/dashboard/src/widgets/waypoint_canvas.{h,cpp}`, `pc_careguard/dashboard/src/widgets/slam_mapping_card.{h,cpp}`
> 대상 로봇: R-07(LIMO, wego@192.168.230.42)
> 관련 브랜치: `fix/limo-battery-voltage-curve`, `fix/battery-threshold-hysteresis`, `fix/camera-view-reset-on-robot-switch`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | LIMO 물리 배터리 게이지는 4칸 중 1.5칸(~35~37%)인데 대시보드엔 1%로 표시됨 | 전압→% 변환이 2점 직선(`MAX=12.6V`/`MIN=10.0V`)이었는데, 실측 전압(9.8V)이 `MIN`보다 낮아 계산값이 음수 → 0~1%로 클램프. `MIN=10.0V` 자체가 하루 전 실측 지점 1개만으로 역산한 추정치였고, 리튬이온 방전곡선이 비선형이라 2점 직선으로는 저잔량 구간에서 크게 어긋남 | 실측 전압 3개 지점(12.6V=100%, 11.3V=50%, 9.8V=37.5%)과 컷오프 추정치(9.0V=0%)를 잇는 구간별 직선(피스와이즈)으로 교체 |
| 2 | "배터리 주의"와 "정상 회복" 알림이 번갈아 반복해서 계속 생성됨 | 배터리 복귀(return_needed) 임계값에만 히스테리시스(+5%p 여유분)가 있었고, 주의(attention) 임계값에는 없어서 attention_pct 근처에서 값이 오르내릴 때마다 상태가 매번 뒤집힘 | attention 탈출에도 같은 방식(+5%p 여유분)의 히스테리시스 적용 |
| 3 | 순찰(P6)·지도(P5) 화면에서 카메라 연결 안 된 로봇으로 전환해도 "카메라 연결 중"이 안 뜨고 이전 로봇의 마지막 프레임이 정지 화면으로 남음 | 카메라 위젯(`CameraLiveView`)이 페이지당 하나만 생성돼 로봇 전환 시에도 재사용되는데, 로봇 전환 로직이 온라인·주행 상태는 초기화하면서 카메라 화면은 전혀 건드리지 않음. `placeholder`로 되돌리는 `clearFrame()`은 이미 있었지만 호출하는 곳이 없어 죽은 코드였음 | `WaypointCanvas`/`SlamMapView`에 `clearCameraFrame()` 위임 메서드를 추가하고, 로봇/맵 전환 시점에 호출 |

---

## 1. LIMO 배터리 % 오표시

### 증상

물리 배터리 게이지는 4칸 중 1.5칸(대략 35~37%)인데, 대시보드에는 1%로 표시됨.

### 진단 과정

1. `robot_careguard_ws/src/careguard_agent/careguard_agent/uplink_node.py`의 전압→% 변환 로직 확인:
   ```python
   LIMO_BATTERY_VOLTAGE_MAX = 12.6
   LIMO_BATTERY_VOLTAGE_MIN = 10.0
   percentage = max(0.0, min(100.0, (voltage - MIN) / (MAX - MIN) * 100.0))
   ```
   `MIN=10.0V`는 2026-09-13(전날) 실측 지점 1개(게이지 3번째 칸 깜빡임 시점 11.3V)만으로 `MAX` 고정 하에 역산한 추정치였다(`2026-09-13-limo-camera-outage-and-battery-gap.md` 3번 항목 참고) — 코드 주석에도 "다른 지점으로 재확인하면 더 정확해질 수 있다"고 스스로 명시돼 있었음.
2. LIMO(`wego@192.168.230.42`)에 SSH 접속(사용자가 계정/비밀번호 제공). `.bashrc`의 "환경 선택(1/2/3)" 메뉴는 실제 인터랙티브 tty에서만 뜨고 비-tty SSH 명령 실행에서는 건너뛰므로, 필요한 ROS 오버레이(`/opt/ros/humble`, `~/wego_ws/install`, `~/robot_careguard_ws/install`)를 직접 source해서 우회:
   ```bash
   source /opt/ros/humble/setup.bash && source ~/wego_ws/install/setup.bash
   source ~/robot_careguard_ws/install/setup.bash
   export ROS_LOCALHOST_ONLY=1
   ros2 topic echo /limo_status --field battery_voltage
   ```
3. 8초간 연속 샘플링 결과 `battery_voltage`가 **9.8V로 고정**(로봇 정지 상태, `vehicle_state: 0`) — 모터 부하로 인한 순간 처짐이 아니라 안정적인 값이었다. 9.8V는 당시 `MIN=10.0V`보다 낮아 계산식이 음수가 되고 0~1%로 클램프되는 걸 그대로 재현.
4. `ros2 interface show limo_msgs/msg/LimoStatus`로 확인한 결과 메시지에는 `battery_voltage` 필드만 있고, LIMO 자체 게이지(%)를 ROS로 직접 가져올 방법은 없음 — 전압만으로 % 재보정을 해야 한다는 뜻.

### 원인

리튬이온 배터리의 방전 곡선은 비선형(중간은 완만, 저잔량에서 급격히 꺾임)인데, 기존 코드는 2점(MAX/MIN) 직선 보간만 썼다. `MIN` 값이 단 하나의 근사 실측 지점으로 정해진 상태였기 때문에, 그 지점에서 멀어질수록(특히 저잔량 쪽) 오차가 컸다.

표준 3S 리튬이온 방전곡선을 그대로 적용하는 방법도 검토했으나, 표준 곡선상 9.8V는 거의 0%에 해당해(0%↔9.81V) 실측된 게이지(~37.5%)와 여전히 크게 어긋났다 — 이 배터리팩의 실제 특성이 일반적인 표준 곡선과 다르다는 뜻으로 판단, 표준 곡선을 그대로 쓰는 대신 실측 지점을 직접 잇는 방식을 택함(사용자 확인 후 결정).

### 해결

`uplink_node.py`에서 2점 직선을 실측 기반 3구간 피스와이즈 선형 보간으로 교체(`fix/limo-battery-voltage-curve` 브랜치):

```python
LIMO_BATTERY_VOLTAGE_PERCENT_CURVE = (
    (12.6, 100.0),  # 3S 리튬이온 화학적 완충 전압 — 실측 없이도 신뢰 가능
    (11.3, 50.0),   # 2026-09-13 실측, 게이지 3번째 칸 깜빡임 시점
    (9.8, 37.5),    # 2026-09-14 실측, 게이지 1.5/4칸 상태
    (9.0, 0.0),     # 실측치 없음 — 3S 팩 일반적 완전방전 컷오프(3.0V/cell) 추정치
)
```

### 검증

변환 함수 단독 실행으로 9.8V → 37.5%, 11.3V → 50%, 12.6V → 100% 반환 확인(단위 테스트 수준). 로봇에는 아직 배포하지 않음 — 배포 후 실제 대시보드 표시값이 물리 게이지와 맞는지 재확인 필요.

### 한계 / 후속 작업

중간 두 지점(11.3V, 9.8V)은 물리 게이지 칸을 눈대중으로 읽은 값이라 여전히 근사치다. 이후 게이지 칸이 바뀌는 시점마다 `ros2 topic echo /limo_status --field battery_voltage`로 전압을 추가로 기록해두면 이 표를 더 정교하게 다듬을 수 있다.

**교훈**: 전압→% 같은 물리량 보정 상수는 실측 지점이 1개뿐이면 신뢰하지 말고, 반드시 "이 지점 하나로 역산했다"는 걸 코드 주석에 남겨야 한다(이번에도 그 주석 덕분에 다음 사람이 바로 의심하고 재검증할 수 있었다). 표준 스펙값을 대입하기 전에, 실제 관측값이 그 표준과 맞는지부터 확인해야 한다.

---

## 2. 배터리 주의(attention) 알림이 정상 회복과 번갈아 반복 생성됨

### 증상

배터리 % 표시는 안정적인데(대시보드 카드), 시스템 로그에는 "배터리 주의"와 "정상 범위로 회복"이 짧은 간격으로 계속 번갈아 뜸.

### 원인

`pc_careguard/server/app/ws/robot_ws.py::_handle_battery_state`에는 이미 2026-09-14 앞선 커밋(`eb1f9fc`)에서 복귀(return_needed) 임계값 탈출에 히스테리시스(+5%p 여유분)가 적용돼 있었다(R-07이 복귀 임계값 근처에서 27%↔31%로 반복 튀던 문제 대응). 하지만 이 여유분은 `return_needed`에만 적용되고 `attention` 탈출에는 없었다 — attention_pct(예: 30%) 근처에서 같은 방식으로 값이 오르내리면, 5초 체크 주기마다 상태가 매번 뒤집히며 `battery_attention`/`battery_recovered` 이벤트가 번갈아 생성됐다.

### 해결

`_handle_battery_state`에서 `attention` 탈출에도 동일한 여유분(`_BATTERY_RECOVERY_MARGIN_PCT=5`, 기존 `_BATTERY_RETURN_RECOVERY_MARGIN_PCT`를 일반화해 이름 변경)을 적용(`fix/battery-threshold-hysteresis` 브랜치, `eb1f9fc` 이후 추가 커밋):

```python
elif previous_state == "attention" and percentage <= attention_pct + _BATTERY_RECOVERY_MARGIN_PCT:
    new_state = "attention"
```

### 검증

신규 테스트 `test_battery_attention_requires_recovery_margin` 추가 — 29%(주의 진입) → 33%(여유분 미만, 주의 유지) → 29%(유지) → 36%(여유분 초과, 정상 복귀) 시나리오로 검증. 기존 5개 테스트 포함 `pytest tests/test_robot_ws_battery.py` 전체 6개 통과, `flake8 --select=F` 통과.

**교훈**: 히스테리시스를 한 임계값 경계에만 적용하고 "이걸로 끝"이라고 생각하면 안 된다 — 같은 물리 현상(전압 처짐/회복)은 어느 경계 근처에서든 똑같이 재현될 수 있으므로, 상태 목록에 경계가 여러 개면 전부 같은 방식으로 다뤄야 한다.

---

## 3. 로봇/맵 전환 시 카메라 화면에 이전 로봇 프레임이 남음

### 증상

순찰(P6)·지도(P5) 화면에서 카메라가 연결된 로봇을 보다가 카메라가 연결되지 않은(또는 오프라인인) 다른 로봇으로 전환하면, "카메라 연결 중" placeholder가 뜨는 대신 방금 전 로봇의 마지막 프레임이 정지 화면으로 남아있다가 3초 뒤 "연결 끊김" 배지만 뜸.

### 원인

카메라 라이브 화면(`CameraLiveView`)은 P6/P5 페이지당 하나만 생성되어 로봇 탭을 바꿔도 재사용된다. 로봇 전환 로직(`P6PatrolPage::loadMapData()`, `P5MapPage::switchToRobot()`)은 온라인 상태·주행 상태(`robotOnline_`, `isPatrolling_`, `navRunning_` 등)는 "이전 로봇 것을 그대로 들고 있으면 안 된다"는 이유로 안전하게 초기화하면서도, 카메라 화면은 전혀 손대지 않았다.

`CameraLiveView`에는 placeholder로 되돌리는 `clearFrame()`이 이미 정의돼 있었지만(생성자에서 만드는 "카메라 연결 중" 문구/아이콘으로 복귀), 저장소 전체에서 이 함수를 호출하는 곳이 정의부 말고 한 군데도 없어 사실상 죽은 코드였다.

### 해결

`fix/camera-view-reset-on-robot-switch` 브랜치:

- `WaypointCanvas`(P6, `waypoint_canvas.h/cpp`)와 `SlamMapView`(P5, `slam_mapping_card.h/cpp`)에 `clearCameraFrame()` 위임 메서드 추가 — 내부적으로 기존 `CameraLiveView::clearFrame()`을 호출.
- `P6PatrolPage::loadMapData()`와 `P5MapPage::switchToRobot()`의 "이전 로봇 상태 초기화" 블록에 각각 `canvas_->clearCameraFrame()` / `slamCard_->view()->clearCameraFrame()` 호출 추가.

### 검증

`cmake --build`로 빌드 성공 확인. **실제 대시보드를 띄워 로봇 전환 시 화면을 눈으로 확인하지는 않았음** — 머지 전 카메라 미연결 로봇으로 전환해 placeholder가 정상적으로 뜨는지 직접 확인 필요.

**교훈**: 로봇/맵 전환 시 "이전 상태를 들고 있으면 안 된다"는 원칙을 적용할 때, 상태 변수(`bool`/enum)뿐 아니라 화면에 남아있는 마지막 데이터(이미지·프레임 등)도 같은 원칙의 적용 대상이라는 걸 놓치기 쉽다. `clearFrame()`처럼 만들어만 두고 호출부를 안 붙인 함수가 없는지, 새 기능을 추가할 때 `grep`으로 실제 호출 여부를 확인하는 습관이 필요하다.
