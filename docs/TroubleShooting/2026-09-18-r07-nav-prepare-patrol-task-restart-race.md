# 2026-09-18 트러블슈팅 — R-07 P6 순찰 페이지 카메라/라이다/로봇상태 배너 동시 불능

> 작성자: 도희정 (세션 진단)
> 요약: P6 순찰 페이지에서 카메라 연결 안 됨 + 라이다 스캔 안 나옴 + 로봇 상태 배너 안 뜸이 동시에 발생. R-07(현재 실사용 중인 유일한 로봇)에 SSH로 직접 진단한 결과, 카메라·라이다 자체의 하드웨어/드라이버 문제가 아니라 **`nav_prepare`(P6 페이지 진입·새로고침마다 발사)가 매번 `patrol_task`를 무조건 재시작시키는 기존 설계** 때문에, 짧은 간격으로 겹친 두 번의 `nav_prepare`가 방금 뜨기 시작한 `patrol_task`를 기동 도중 죽였고, 그 여파로 로봇의 ROS2 DDS 디스커버리 전체가 오염되어 이 프로세스와 무관한 카메라·라이다·odom 토픽까지 전부 응답 불능(0Hz) 상태가 된 것으로 진단됨. **코드 수정은 아직 적용하지 않았고, 수정 방향만 합의된 상태** — 담당자가 이어서 구현 필요.
> 대상 파일: `pc_careguard/server/app/api/v1/patrol.py`(이 git 저장소, 서버), `robot_careguard_ws/src/careguard_agent/careguard_agent/downlink_node.py` (R-07 로컬 경로, 이 git 저장소에는 없음 — SSH로만 접근 가능), `robot_careguard_ws/src/careguard_nav/careguard_nav/dock_return.py` (내용 미확인, 다음 조사 필요), `Patrol.action` 정의(위치 미확인, 충전 웨이포인트 필드 추가 필요)
> 대상 로봇: R-07 (LIMO, `wego@192.168.230.42`, 서버는 `192.168.230.7:8000`)
> 관련 문서: [2026-09-09-downlink-map-nav-lifecycle.md](./2026-09-09-downlink-map-nav-lifecycle.md) (patrol_task "무조건 재시작" 설계를 **의도적으로** 도입한 문서 — 이번 사고의 전제가 그때 깨졌다), [2026-09-15-r07-patrol-schedule-priority-and-retrigger.md](./2026-09-15-r07-patrol-schedule-priority-and-retrigger.md) (`_patrol_goal_handle` 가드 도입 — 이번 사고는 이 가드로도 못 막는 케이스)

## 요약

| # | 증상/사실 | 원인 | 해결 방향 (미적용, 담당자 작업 필요) |
|---|---|---|---|
| 1 | P6에서 카메라 연결 안 됨 + 라이다 스캔 안 나옴 + 로봇 상태 배너 안 뜸이 동시 발생 | `ros2 node list` 완전히 비어있고 `/scan`·`/odom`·`/camera/.../compressed` 모두 publisher는 있는데 실제 메시지 0개(`_NODE_NAME_UNKNOWN_`) — DDS 디스커버리 자체가 깨진 상태. 아래 #2가 방아쇠로 추정됨 | #2, #3 수정으로 재발 방지 |
| 2 | `patrol_task`가 아무것도 안 바뀌었어도 `nav_prepare`(페이지 진입/새로고침)마다 매번 kill+재시작되고, 두 요청이 짧은 간격(예: 1초)으로 겹치면 방금 뜨기 시작한 프로세스를 기동 도중 또 죽임 | 2026-09-09에 "patrol_task는 가벼운 `ros2 run` 노드라 재시작 비용이 없다"고 판단해 map_id 재사용 로직을 **의도적으로** 제거하고 "무조건 재시작"으로 단순화했는데, 그 후 `nav_prepare`가 (patrol_start보다 훨씬 잦게) 페이지 진입·새로고침마다 이 함수를 부르게 되면서 "비용 없음" 전제가 깨짐 | `_ensure_patrol_task_running`에 **map_id 비교 재사용 로직**을 다시 도입 (단, 내용 변경 여부는 볼 필요 없음 — 이유는 #3) |
| 3 | 재매핑으로 충전 웨이포인트가 바뀌면 patrol_task가 예전 좌표로 복귀 시도하는 문제가 있어서(2026-09-09에 이미 한 번 발견) #2의 "무조건 재시작"이 그 임시 해법이었음 | patrol_task가 시작 시점에 딱 한 번 `WaypointManager.fetch()`로 충전 좌표를 캐시해두고 순찰 종료 시 그 캐시를 씀 (`dock_return.py`) — 순찰/긴급 웨이포인트처럼 매 goal마다 fresh하게 안 받음 | 서버(`patrol.py::start_patrol_run`)가 **경로 웨이포인트를 조회하는 바로 그 시점에 충전 웨이포인트도 같이 조회**해서 `patrol_start` 메시지에 실어 보낸다. patrol_task는 자체 fetch 없이 goal에 실려온 값을 그대로 씀 — 순찰/긴급 웨이포인트와 완전히 같은 패턴으로 통일 |
| 4 (별건, 원인 아님) | Nav2 최초 기동 시(오늘 10:40) rviz2가 SIGABRT로 즉시 크래시 | `navigation_diff_launch.py`가 헤드리스 로봇에서도 rviz2를 같이 띄움 | 이번 사고의 직접 원인은 아니지만 별도로 고칠 가치 있음 — rviz2를 launch에서 제거하거나 headless 조건부로 변경 |

---

## 1. 증상

사용자 리포트: "지금 순찰페이지에 카메라 연결 안됨. 레이더 스캔이 안나오는데 로봇 상태를 나타내는 상단 배너가 안나와." 현재 실사용 중인 로봇은 **R-07(LIMO) 하나뿐**.

`p6_patrol_page.cpp`의 `handleWsMessage()`를 보면 이 세 가지(카메라 프레임, 라이다 스캔 오버레이, 로봇 상태 배너)는 각각 `camera_frame`/`slam_scan_update`/`robot_status` WS 메시지에 의존하는데, 셋이 동시에 안 나온다는 건 특정 위젯 버그가 아니라 **로봇 쪽에서 이 메시지들 자체가 안 올라오고 있다**는 신호였다.

## 2. 진단 과정

1. R-07에 SSH 접속(`wego@192.168.230.42`, `wego`) — `careguard-bringup-r07`/`-uplink-r07`/`-downlink-r07` 세 서비스 모두 `active (running)`. `ps aux`에도 `limo_base`, `ydlidar_ros2_driver_node`, `camera_container`, `robot_localization`(ekf), `nav2_container`가 전부 정상적으로 (중복 없이) 떠있음 — 프로세스 자체는 죽지 않았다.
2. 그런데 `ros2 daemon stop; ros2 daemon start`로 데몬을 새로 띄운 뒤에도 `ros2 node list`가 완전히 빈 상태. `ros2 topic info -v /scan`, `/odom`, `/camera/color/image_raw/compressed` 모두 publisher/subscriber count는 정상(1개/1~2개)인데 **노드 이름이 `_NODE_NAME_UNKNOWN_`**, `ros2 topic hz`는 10초 넘게 기다려도 메시지 0개.
   - 이건 이전에 관측된 적 있는 라이다 전용 고스트 DDS 증상(publisher/subscriber는 정상인데 실제 메시지 0개, 노드 이름 `_NODE_NAME_UNKNOWN_`)과 같은 패턴인데, 이번엔 **라이다뿐 아니라 카메라·odom까지** 도메인 전체가 영향을 받았다는 점이 다르다.
3. `journalctl -u careguard-downlink-r07.service`에서 오늘(9/18) 부팅(10:38:37) 이후 로그를 훑어봄:
   - 10:40:51경 첫 `nav_prepare`로 Nav2가 기동되는 과정에서 `[ERROR] [rviz2-2]: process has died [pid 3127, exit code -6 (SIGABRT)]` — `navigation_diff_launch.py`가 이 헤드리스 로봇에서도 rviz2를 같이 띄우는데 바로 죽음. (→ 요약 표 #4, 별건으로 분리)
   - 팀원이 실제 `nav_prepare` 호출 시각을 로그에서 뽑아보니: `10:55:49, 10:55:54, 10:55:56`(페이지 조작 구간, 불규칙), `11:06:51`(단발), `11:17:09`(단발), `11:18:04`(단발), **`11:18:43`, `11:18:44`(1초 간격으로 2번)**, `11:22:57`(단발) — 일정 주기 타이머가 아니라 페이지 진입/새로고침/맵 전환 때마다 나가는 트리거이고, 어쩌다 두 번이 짧은 간격으로 겹치는 경우가 있음을 확인.
   - 그 `11:18:43`/`11:18:44` 구간을 다시 `journalctl --since '11:18:30' --until '11:19:15'`로 열어보니, `patrol_task` 프로세스(pid 4393/4394)가 `rclpy` spin 도중 `ExternalShutdownException`/`KeyboardInterrupt`로 죽는 트레이스가 정확히 `11:18:44`에 찍혀있었다 — 즉 **기동한 지 1초 만에 SIGINT로 죽었다**는 물증.

## 3. 원인

코드(`downlink_node.py`)를 읽어서 확인한 호출 경로:

```python
def _prepare_navigation(self, message: dict):
    # P6 페이지가 열리거나(showEvent) 맵 전환될 때마다 호출됨
    map_id = int(message.get('map_id', 0) or 0)
    ...
    self._ensure_navigation_running(map_id)      # Nav2 — map_id+내용 안 바뀌면 재사용(가드 있음)
    self._ensure_video_recorder_running()
    self._ensure_voice_announcer_running()
    self._ensure_patrol_task_running(map_id)     # patrol_task — 가드 없음!
```

`_ensure_patrol_task_running`은 [2026-09-09 문서](./2026-09-09-downlink-map-nav-lifecycle.md)에서 **의도적으로** map_id 재사용 로직을 제거하고 "호출될 때마다 무조건 kill+재시작"으로 단순화됐다. 당시 근거는 "patrol_task는 Nav2와 달리 런치 트리·AMCL·costmap이 없는 가벼운 `ros2 run` 노드 하나라 재시작 비용이 거의 없다"였고, **그 시점엔 이 함수가 `patrol_start` 직전에만 호출된다고 가정**했다.

이후 [2026-09-15 문서](./2026-09-15-r07-patrol-schedule-priority-and-retrigger.md)에서 "사실 `nav_prepare`(P6 페이지 새로고침만 해도 발사됨)도 이 함수를 부른다"는 게 드러났고, 그때는 "진행 중인 goal을 죽이는 것"만 문제 삼아 `_patrol_goal_handle is not None` 가드를 추가했다. **하지만 "기동 중이지만 아직 goal이 없는" 상태는 이 가드로 안 걸린다.**

그 결과, `nav_prepare` 두 번이 짧은 간격(관측된 사례: 1초)으로 들어오면:
1. 1번째 `nav_prepare` → `patrol_task` 없음/죽어있음 → 새로 `Popen`으로 기동 시작
2. (기동 완료·액션 서버 등록 전, 약 1초 이내) 2번째 `nav_prepare` 도착 → `_patrol_goal_handle`은 여전히 `None`(아직 goal 안 받음) → 가드 안 걸림 → `self._patrol_task_process.poll() is None`(살아있음)인데도 **map_id 비교 로직이 아예 없어서** 그대로 `_terminate_process_group`으로 SIGINT → 1번째가 띄운, 아직 DDS 등록도 안 끝난 프로세스가 죽음

**아직 초기화가 안 끝난 rclpy 프로세스를 SIGINT로 죽이면 그 노드의 DDS 참가자가 깨끗하게 등록 해제되지 않고 고스트 상태로 남을 수 있다** — 이게 patrol_task와 완전히 무관한 프로세스(카메라, 라이다, ekf)의 토픽까지 도메인 전체 디스커버리를 오염시킨 것으로 추정된다. (이 마지막 인과관계 — "patrol_task 킬 → 도메인 전체 DDS 오염" — 는 로그 타임스탬프와 증상이 정확히 들어맞는 정황 증거이지, 재현 실험으로 확정한 건 아니다. 담당자가 의심되면 재현 테스트로 한 번 더 검증 권장.)

## 4. 왜 "매번 재시작"이 필요했는지 (충전 웨이포인트)

순찰/긴급 웨이포인트는 `_start_patrol`이 `patrol_start` 메시지를 만들 때 **서버가 그 순간 DB에서 새로 조회한 좌표를 goal에 그대로 실어 보낸다** — patrol_task 프로세스 내부 캐시를 안 쓴다. 그래서 이것들은 patrol_task가 며칠간 안 죽어도 항상 최신 값으로 동작한다.

**충전 웨이포인트만 다르다.** `patrol_task`는 시작 시점에 딱 한 번 `WaypointManager.fetch(CAREGUARD_MAP_ID)`로 충전 스테이션 좌표를 캐시해두고, 순찰이 끝나 복귀할 때(`dock_return.py`) 그 캐시를 쓴다. 이게 2026-09-09에 "재매핑해도 예전 충전소로 복귀하는" 버그의 원인이었고, 그때 "map_id 비교 대신 무조건 재시작"으로 땜질했던 것 — 그 땜질이 이번 사고의 근본 전제다.

**왜 순찰/긴급 웨이포인트만 그렇게 돼있고 충전은 안 그런지 — 특별한 이유는 없다.** 그냥 `patrol.py::start_patrol_run`이 경로(`patrol_route.steps`)에 든 웨이포인트만 조회해서 `patrol_start` 메시지에 실었을 뿐, 충전 웨이포인트(`kind='charge'`)는 애초에 이 조회에 낄 일이 없었다(경로 자체에 못 들어감 — `_reject_charge_in_steps` 참고). 그래서 로봇 쪽이 별도로 알아서 캐시해두는 방식이 된 것 — 설계 의도라기보다 "경로 웨이포인트 조회 로직을 그대로 재사용하기 애매해서" 생긴 비대칭에 가깝다.

## 5. 해결 방향 (합의됨, 코드 미적용)

**Nav2 쪽은 건드리지 않는다** — 이미 map_id+내용변경 여부로 "필요할 때만" 재시작하는 가드가 있고, 실제로 잦은 재시작 문제도 없다. `nav2_msgs/srv/LoadMap`으로 무중단 맵 교체가 기술적으로 가능하긴 하지만, 맵이 바뀌면 AMCL이 새로 수렴해야 하는 건 어차피 똑같아서 얻는 이득에 비해 검증 리스크가 크다 — 우선순위 낮음, 이번 작업 범위 아님.

**patrol_task 쪽은 "재시작 필요성 자체를 없앤다":**

1. **서버(`patrol.py::start_patrol_run`)가 경로 웨이포인트를 조회하는 바로 그 시점에 충전 웨이포인트도 같이 조회**해서 `patrol_start` 메시지에 실어 보낸다. `waypoint` 테이블은 맵당 `kind='charge'` 행이 정확히 0~1개이도록 UNIQUE 인덱스(`waypoint_one_charge_per_map_idx`)로 이미 보장돼있어 조회가 단순하다:
   ```python
   # start_patrol_run, waypoints 조회 바로 다음에 추가
   cur.execute(
       "SELECT code, pos_x, pos_y, yaw FROM waypoint "
       "WHERE map_id = %s AND kind = 'charge' AND deregistered_at IS NULL",
       (route["map_id"],),
   )
   charge_wp = cur.fetchone()
   ```
   그리고 `patrol_start` 페이로드에 `"charge_waypoint": {"code": ..., "x": ..., "y": ..., "yaw": ...} if charge_wp else None` 필드를 추가한다.
   로봇 쪽(`downlink_node.py::_start_patrol`)은 이 필드를 그대로 `Patrol.Goal()`에 실어(필드 추가 필요 — `Patrol.action` 정의 확인 필요) `patrol_task`에 넘기고, `dock_return.py`는 자체 `WaypointManager.fetch()` 호출 없이 **goal에 실려온 값**을 그대로 쓰도록 바꾼다. 순찰/긴급 웨이포인트와 완전히 같은 패턴으로 통일되는 것 — 별도의 "복귀 시점 라이브 fetch" 같은 새 메커니즘을 만들 필요가 없다.
2. 1번이 적용되면 patrol_task는 "충전 좌표 stale" 문제에서 자유로워지므로, `_ensure_patrol_task_running`에 **map_id 비교 재사용 로직을 다시 넣는다** — Nav2의 `changed` 비교까지는 필요 없고, 단순히:
   ```python
   if (not override) and self._patrol_task_process is not None \
           and self._patrol_task_process.poll() is None \
           and self._patrol_task_map_id == map_id:
       return True  # 이미 같은 map_id로 떠있음 — 재사용
   ```
   `_patrol_goal_handle` 가드(2026-09-15 도입)는 그대로 유지 — 이번 것과 서로 보완 관계(goal 진행 중 보호 / idle 중 불필요한 재시작 방지).
3. (선택, 별건) `navigation_diff_launch.py`에서 헤드리스 로봇에 불필요한 rviz2를 제거하거나 조건부로 변경.

## 6. 검증 상태

- [x] R-07 서비스/프로세스 생존 확인 (systemctl, ps aux)
- [x] DDS 디스커버리 깨짐 재현 관측 (`ros2 node list` 빈 목록, `_NODE_NAME_UNKNOWN_`, 0Hz)
- [x] `journalctl`로 `patrol_task`가 11:18:44에 SIGINT로 죽는 트레이스 확인
- [x] `_prepare_navigation`/`_ensure_patrol_task_running` 코드 경로 확인 (map_id 가드 없음)
- [ ] **코드 수정 미적용** — 위 5번 방향으로 `patrol.py`, `downlink_node.py`, `Patrol.action`, `dock_return.py` 실제 수정 필요
- [ ] `Patrol.action`(careguard_msgs 또는 careguard_mission 쪽 정의로 추정, 파일 위치 미확인) 정의 확인 — `charge_waypoint`를 실을 필드 추가 필요
- [ ] `dock_return.py` 실제 구현 미확인 — goal에 실려온 충전 웨이포인트를 쓰도록 바꾸는 게 기존 구조에서 자연스러운지 확인 필요 (지금 `WaypointManager.fetch()`를 어디서 얼마나 자주/어떤 시점에 참조하는지부터 확인)
- [ ] `dispatch_drive_to_point`(이동, `patrol.py:473`)는 애초에 도착 후 충전소 복귀를 안 하므로(`return_to_charging: False`) 충전 웨이포인트를 안 실어도 됨 — 손댈 필요 없음, 혼동 주의
- [ ] 수정 후 재현 테스트: P6 페이지를 빠르게 두 번 이상 열거나 새로고침을 연타해서 patrol_task가 안 죽는지, DDS가 안 깨지는지 확인
- [ ] 수정 후 회귀 테스트: 재매핑 후 순찰 → 충전 웨이포인트가 새 좌표로 정상 복귀하는지 재확인 (2026-09-09에 고쳤던 그 시나리오)
- [ ] rviz2 SIGABRT는 원인 후보에서는 제외했지만 별도 이슈로 트래킹 권장 (요약 표 #4)

## 다음 작업 (담당자 TODO)

1. **서버**: `pc_careguard/server/app/api/v1/patrol.py::start_patrol_run`에서 `waypoints` 조회 직후, `route["map_id"]`로 `kind='charge'` 웨이포인트를 같이 조회해 `patrol_start` 페이로드에 `charge_waypoint` 필드로 추가 (본문 5-1번 쿼리 참고). `dispatch_drive_to_point`는 건드리지 않음.
2. **로봇 (메시지 정의)**: `Patrol.action` 정의를 찾아 `charge_waypoint`(x/y/yaw)를 실을 필드 추가. `robot_careguard_ws` 안에서 `grep -rn "Patrol.action\|PatrolGoal\|action/Patrol"`로 위치부터 찾기 (SSH: `wego@192.168.230.42`, `wego`).
3. **로봇 (downlink_node)**: `_start_patrol`이 `message["charge_waypoint"]`를 받아 `goal`에 채워 넣도록 수정.
4. **로봇 (patrol_task/dock_return)**: `dock_return.py`를 열어 지금 `WaypointManager.fetch()`를 어디서/언제 참조하는지 확인하고, 그 자리를 goal로 받은 `charge_waypoint` 값을 쓰도록 교체 — 자체 fetch 호출 제거.
5. **로봇 (downlink_node)**: `_ensure_patrol_task_running`에 5-2번 map_id 비교 가드 추가.
6. R-07에서 재현 테스트: P6 페이지 빠르게 재진입/새로고침 연타 → `ros2 node list`/`ros2 topic hz /scan,/odom,/camera/...` 로 DDS 정상 여부, `journalctl`로 patrol_task가 더 이상 기동 중에 안 죽는지 확인.
7. 회귀 테스트: 재매핑 후 순찰 시작 → 새 충전 좌표로 정상 복귀하는지 확인 (2026-09-09 시나리오 재확인).
8. (선택) rviz2 크래시는 별도 트러블슈팅 문서로 분리해 처리.

---

**교훈**: 2026-09-09에 "이 함수는 patrol_start 직전에만 불린다"는 전제로 최적화(무조건 재시작)를 했는데, 6일 뒤(9/15) 그 전제가 깨진 걸 알고도 "goal 진행 중 보호"만 고치고 "idle 상태에서 겹쳐 불리는 경우"는 놓쳤다. **호출 빈도/타이밍에 대한 가정이 있는 최적화는, 그 가정이 나중에 깨졌을 때 원래 최적화 근거 자체를 다시 검토해야 한다** — 이번처럼 "재시작 비용이 없다"는 전제 하나가 무너지면(=재시작이 훨씬 잦아지면) 다른 보호장치를 덧대는 것보다 애초에 "왜 재시작이 필요했는가"(충전 좌표 stale 방지)로 돌아가 그 필요성 자체를 없애는 게 더 근본적인 해법이었다.
