# 2026-09-14 트러블슈팅 — R-06(터틀봇) Nav2 미기동 진단·복구

> 작성자: 최치언
> "통합관제 상세에서 R-06 Nav2가 안 잡힌다"는 리포트로 시작해, 실제로 로봇에 SSH로
> 들어가 Nav2를 기동해보며 원인을 하나씩 걷어낸 기록이다. 겉으로는 하나의 증상처럼
> 보였지만 실제로는 서로 무관한 4개의 문제가 겹쳐 있었다 — 맵 데이터(충전 웨이포인트
> 없음), 로봇 소프트웨어 기능 누락(nav_prepare/set_initial_pose 미구현), nav2 파라미터
> 파일의 플러그인 이름 버전 불일치, 그리고 마지막엔 코드가 아니라 웹소켓 연결
> 순서 자체의 버그(uplink가 downlink 연결을 덮어씀). 하나씩 고칠 때마다 다음 문제가
> 드러나는 전형적인 "양파 까기" 케이스라 순서대로 정리한다.
> 대상 파일: `careguard_turtlebot_agent/careguard_turtlebot_agent/downlink_node.py`,
> `careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py`,
> `careguard_turtlebot_agent/config/nav2_burger_params.yaml`(신규),
> `careguard_turtlebot_agent/setup.py`,
> `careguard_turtlebot_agent/systemd/careguard-downlink-r05.service`,
> `careguard_turtlebot_agent/systemd/careguard-downlink-r06.service`
> 대상 로봇: R-06(터틀봇, yeyu@100.110.210.121). 맵 데이터는 R-07(LIMO)에서 복사해왔다.
> R-05(aurix)는 같은 패키지를 쓰지만 이번엔 코드만 맞춰두고 실제 배포·검증은 안 했다.

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | R-06 상세보기에서 Nav2가 전혀 안 뜸 | R-06의 활성 맵(map_id=2)에 `kind=charge` 웨이포인트가 없어 `_fetch_charge_waypoint`가 실패 | 같은 물리 공간에 있는 R-07의 맵 이미지+웨이포인트(충전 포함)를 그대로 복사해 R-06의 새 활성 맵으로 등록 |
| 2 | "초기 위치 설정"을 눌러도 로봇이 반응 없음 | 터틀봇 `downlink_node.py`가 `nav_prepare`/`set_initial_pose` 명령 타입 자체를 처리하지 않고 경고만 남기고 버림(LIMO 전용 스코프로 좁게 시작된 코드) | LIMO의 로직을 그대로 옮기는 대신, 이미 있던 `_ensure_navigation_running`/`_publish_initial_pose`를 재사용해 두 핸들러를 얇게 추가 |
| 3 | Nav2를 띄워도 `planner_server`가 FATAL로 죽고 전체 bringup이 abort | `~/turtlebot3_ws`의 `burger.yaml`(소스 트리 원본)이 최신(Iron+) 플러그인 이름 규칙(`패키지::클래스`)을 쓰는데, 이 로봇에 apt로 깔린 Humble용 `nav2_navfn_planner`/`nav2_behaviors`는 구버전 규칙(`패키지/클래스`)으로만 등록돼 있음 | 리포에 수정본 파라미터 파일(`config/nav2_burger_params.yaml`)을 두고 `CAREGUARD_NAV2_PARAMS_FILE` 환경변수로 가리키게 함 |
| 4 | 위 3가지를 다 고쳤는데도 서버가 보낸 `nav_prepare`가 로봇에 전혀 안 옴(`robot_connected: true`인데 로그가 조용함) | `uplink_node.py`가 `role=uplink` 파라미터 없이 접속 → 서버가 기본값(downlink)으로 취급해 나중에 접속하는 순간 downlink_node의 명령 수신 연결을 덮어씀 | `uplink_node.py` 접속 URL에 `&role=uplink` 추가 |
| 5 | Nav2도 뜨고 명령도 도착하는데 "초기 위치 설정"을 해도 대시보드 지도 위 로봇이 안 움직임 | `uplink_node.py`가 위치를 `/odom`(로봇 부팅 시점이 원점인 상대 좌표)으로 보내고 있어서, AMCL pose를 아무리 다시 잡아도 대시보드가 보는 값 자체가 맵과 무관했음 | position 구독을 `/odom` → `/amcl_pose`로 교체(LIMO의 순찰 단계와 동일한 방식) |

---

## 1. R-06 활성 맵에 충전 스테이션 웨이포인트가 없음

### 증상

통합관제(P1) 상세보기에서 R-06의 "순찰 취소 → 5초 후 충전소 복귀" 카운트다운이 끝나도 실제로는 아무 반응이 없었고, `downlink_node` 로그에는 다음이 3초 간격으로 반복 출력됐다.

```
[WARN] Nav2 자동 기동 실패, 3.0s 후 재시도: map_id=2에 충전 스테이션 웨이포인트가 없습니다
```

### 진단

`GET /waypoints?map_id=2`로 직접 조회한 결과 `kind=patrol` 4개만 있고 `kind=charge`는 0개였다. `downlink_node.py::_fetch_charge_waypoint`는 충전 웨이포인트가 없으면 예외를 던지고, 이게 있어야만 `_ensure_navigation_running`이 초기 위치를 잡고 Nav2를 띄울 수 있는 구조다.

### 해결

R-06과 R-07이 같은 물리 공간(공유기만 다름)에 있다는 걸 사용자에게 확인받은 뒤, R-07의 활성 맵(image_file_id=6, 충전 웨이포인트 WP_01 포함 4개)을 그대로 복사해 R-06의 새 맵으로 등록했다.

```bash
# 1. R-07 맵 이미지 다운로드 후 R-06 이름으로 재업로드
curl -s http://192.168.230.7:8000/media/files/6 -o r07_map.pgm
curl -s -F "robot_code=R-06" -F "file=@r07_map.pgm;filename=r07_map_shared.pgm" \
  http://192.168.230.7:8000/media/upload-map   # -> file_id=60

# 2. R-06 새 맵으로 확정 (R-07과 같은 resolution/origin/width/height)
curl -s -X POST http://192.168.230.7:8000/maps -H "Content-Type: application/json" -d '{
  "robot_code": "R-06", "map_name": "이말순님 자택", "file_id": 60,
  "resolution": 0.05, "origin_x": -8.47, "origin_y": -9.45, "origin_yaw": 0.0,
  "width_px": 371, "height_px": 353,
  "raw_yaml": {"image": "r07_map_shared.pgm", "mode": "trinary", "resolution": 0.05,
    "origin": [-8.47, -9.45, 0.0], "negate": 0, "occupied_thresh": 0.65, "free_thresh": 0.25}
}'   # -> map_id=31

# 3. R-07의 웨이포인트 4개(충전 1 + 순찰 3)를 map_id=31로 복사
# (POST /waypoints 4회, kind=charge 1개 포함)
```

### 검증

`nav2_bringup bringup_launch.py`를 이 맵으로 수동 기동해 `/amcl_pose` 퍼블리셔가 뜨는 것까지 확인 — 이 시점엔 아래 3번 문제(플러그인 이름)가 아직 남아 있어서 `planner_server`는 죽어 있었다(첫 수동 테스트는 우연히 성공했다가, 재현 테스트에서는 실패해 3번 문제로 이어짐).

---

## 2. 터틀봇 `downlink_node`가 `nav_prepare`/`set_initial_pose`를 처리하지 않음

### 증상

대시보드 P6 "초기 위치 설정"을 눌러도 로봇이 반응하지 않음. `journalctl`로 확인하니 명령은 로봇까지 왔지만:

```
[WARN] 터틀봇 downlink가 아직 처리하지 않는 명령: 'nav_prepare'
[WARN] 터틀봇 downlink가 아직 처리하지 않는 명령: 'set_initial_pose'
```

### 원인

`careguard_turtlebot_agent/downlink_node.py`는 애초에 "배터리 낮으면 충전소로 복귀"만 처리하도록 좁게 시작된 코드(모듈 docstring 참고)라 `_handle_command`가 `patrol_start`/`patrol_stop`만 분기하고 나머지는 경고만 남기고 버린다. LIMO(`robot_careguard_ws/src/careguard_agent/downlink_node.py`)에는 두 핸들러가 이미 있지만, `careguard_nav`/`careguard_mission` 같은 LIMO 전용 워크스페이스 패키지에 의존해서 그대로 옮겨올 수 없다.

### 해결

LIMO 코드를 복사하는 대신, 터틀봇 `downlink_node.py`에 이미 있던 자체 구현(`_ensure_navigation_running`, `_publish_initial_pose` — 둘 다 외부 패키지 없이 순수 자체 구현)을 그대로 재사용해 얇은 핸들러 두 개만 추가했다.

```python
elif msg_type == 'nav_prepare':
    self._prepare_navigation(message)
elif msg_type == 'set_initial_pose':
    self._handle_set_initial_pose(message)
```

`_prepare_navigation`은 `map_id`로 충전 웨이포인트를 조회해 `_ensure_navigation_running`을 부르고, `_handle_set_initial_pose`는 Nav2가 이미 떠 있을 때만 `_publish_initial_pose`로 AMCL 위치를 즉석에서 다시 잡는다(LIMO와 동일한 전제 — Nav2 자체는 여기서 새로 켜지 않음).

---

## 3. `planner_server`가 플러그인 클래스를 못 찾아 FATAL

### 증상

Nav2를 띄우면 `amcl`은 뜨는데 `planner_server`가 죽고 `lifecycle_manager_navigation`이 전체 bringup을 abort한다.

```
[FATAL] [planner_server]: Failed to create global planner. Exception: According to the
loaded plugin descriptions the class nav2_navfn_planner::NavfnPlanner with base class
type nav2_core::GlobalPlanner does not exist. Declared types are
nav2_navfn_planner/NavfnPlanner nav2_smac_planner/SmacPlanner2D ...
```

같은 맵으로 수동 재현 테스트를 여러 번 반복했고(컴포지션 켜고/끄고, 워크스페이스 소싱 조합 바꿔가며), 매번 100% 같은 에러로 재현됐다 — 환경 차이나 경합(race)이 아니라 구조적인 버전 불일치였다.

### 원인

`~/turtlebot3_ws/src/turtlebot3/turtlebot3_navigation2/param/burger.yaml`(소스만 있고 빌드는 안 된 트리, 2026-09-13에 이미 확인된 상태)은 최신(Iron 이후) nav2 컨벤션인 `"패키지::클래스"` 형식으로 플러그인을 지정한다. 그런데 이 로봇에 apt로 깔린 Humble용 `ros-humble-nav2-navfn-planner`/`ros-humble-nav2-behaviors` 패키지는 아직 구버전 컨벤션(`"패키지/클래스"`)으로 플러그인을 등록하고 있었다 — 실제로 설치된 XML을 직접 열어 확인:

```xml
<!-- /opt/ros/humble/share/nav2_navfn_planner/global_planner_plugin.xml -->
<class name="nav2_navfn_planner/NavfnPlanner" type="nav2_navfn_planner::NavfnPlanner" .../>
```

`burger.yaml` 자체에도 "In Iron and older versions, '/' was used instead of '::'"라는 주석이 있는데, Humble은 Iron보다 오래된 배포판이라 실제로는 "/"를 썼어야 했다 — 소스 트리가 최신 브랜치 기준으로 이미 앞서가 있어서 생긴 불일치였다. 같은 방식으로 대조해보니 `nav2_navfn_planner`와 `nav2_behaviors`(recovery behaviors: spin/backup/drive_on_heading/wait/assisted_teleop) 두 패키지만 영향을 받고, 나머지(controller_server가 쓰는 `dwb_core` 등)는 이미 `::` 형식으로 정상 등록돼 있었다.

### 해결

`turtlebot3_ws` 소스 트리(별도 저장소, 우리가 관리하지 않음)를 직접 고치면 나중에 그 워크스페이스를 다시 체크아웃/업데이트할 때 조용히 되돌아갈 수 있어서, 이 리포 안에 관리 사본을 새로 뒀다.

- `careguard_turtlebot_agent/config/nav2_burger_params.yaml` — 원본 `burger.yaml`을 복사해 문제의 6곳(`planner_server.GridBased.plugin` 1곳 + `behavior_server`의 spin/backup/drive_on_heading/wait/assisted_teleop 5곳)만 `/` 형식으로 수정, 나머지는 원본과 동일. 원인과 근거는 파일 헤더 주석에 남겨뒀다.
- `careguard_turtlebot_agent/setup.py` — 이 config 파일을 `share/careguard_turtlebot_agent/config/`로 설치하도록 `data_files`에 등록.
- `careguard_turtlebot_agent/systemd/careguard-downlink-r06.service`(+ 같은 문제 소지가 있는 r05도 함께) — `CAREGUARD_NAV2_PARAMS_FILE` 환경변수로 이 파일의 설치 경로를 가리키도록 추가. `downlink_node.py`는 이미 이 환경변수를 읽도록 돼 있었어서 코드 변경은 필요 없었다.

### 검증

R-06에 배포(파일 전송 → `colcon build` → systemd 유닛 갱신 → 재시작) 후 `nav_prepare`를 다시 보내 로그에 `lifecycle_manager_navigation: Managed nodes are active`가 찍히는 것과, `ros2 node list`에 `amcl`/`map_server`/`planner_server`/`controller_server`/`bt_navigator`/`behavior_server`/`waypoint_follower`/`velocity_smoother`가 전부 뜨는 것을 확인했다.

---

## 4. `uplink_node`가 downlink 연결을 덮어써서 명령이 로봇에 안 옴

### 증상

위 1~3번을 다 고친 뒤에도 서버 API(`/patrol-runs/prepare-navigation`)는 `{"robot_connected": true}`를 정상 응답하는데, R-06의 `downlink_node` 로그에는 아무 것도 안 찍힘 — 명령이 허공으로 사라지는 것처럼 보였다.

### 진단

서버 access 로그(`/tmp/careguard-server.log`)를 직접 열어 R-06의 `/ws/robot` 연결 두 건을 시간순으로 비교했다.

```
100.110.210.121:43040 - WebSocket /ws/robot?robot_code=R-06&role=downlink [accepted]   ← downlink_node, 먼저 접속
100.110.210.121:44276 - WebSocket /ws/robot?robot_code=R-06 [accepted]                  ← role 파라미터 없음, 나중 접속
```

`careguard_turtlebot_agent/uplink_node.py`의 접속 URL에 `&role=uplink`가 빠져 있었다. 서버(`robot_ws.py`)는 role이 없는 연결을 기본값인 downlink로 취급하는데, 이건 "role을 안 보내는 클라이언트를 안전하게(명령을 받을 수 있는 연결로) 다루자"는 의도였지만 — downlink_node가 먼저 붙고 uplink_node가 그 다음에 role 없이 붙으면, `_connections[robot_code]`(명령을 실제로 내보내는 딕셔너리)가 downlink_node의 연결에서 uplink_node의 연결로 통째로 덮어써진다. 이후 서버가 보내는 모든 명령은 uplink_node로 가서 조용히 무시된다(`send_command`는 `ws.send_json`이 예외 없이 성공하면 `True`를 반환하므로, 호출부에서는 "연결돼 있다"는 응답만 받고 실제로는 엉뚱한 곳에 전달된 것을 알 방법이 없다).

### 해결

`downlink_node.py`가 이미 하고 있던 것과 동일하게 `uplink_node.py`의 접속 URL에도 `&role=uplink`를 명시했다.

```python
url = f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}&role=uplink'
```

### 검증

`uplink_node`/`downlink_node` 재시작 후 서버 로그에 두 연결 모두 role이 정확히 찍히는 것을 확인:

```
100.110.210.121:34008 - WebSocket /ws/robot?robot_code=R-06&role=uplink [accepted]
100.110.210.121:34014 - WebSocket /ws/robot?robot_code=R-06&role=downlink [accepted]
```

이후 `nav_prepare`를 다시 보내자 즉시 `downlink_node` 로그에 `Nav2 시작: map_id=31 (...)`이 찍혔다.

**교훈**: 같은 로봇의 uplink/downlink처럼 서버가 role로 구분하는 다중 연결 구조에서는, "role을 안 보내면 안전한 기본값으로 처리"가 오히려 먼저 맺어진 올바른 연결을 조용히 덮어쓰는 함정이 될 수 있다. `send_command`가 `True`를 반환한다고 해서 "의도한 연결로 갔다"는 보장이 아니라는 점도 이번에 명확해졌다 — 연결이 여러 개일 수 있는 구조라면 각 연결이 실제로 어떤 role로 등록됐는지 서버 로그로 직접 대조하는 게 가장 확실하다.

---

## 5. `uplink_node`가 `/odom`을 보내 "초기 위치 설정"이 화면에 반영 안 됨

### 증상

1~4번을 전부 고쳐서 Nav2도 뜨고 명령도 정상 도달하는데, 대시보드에서 "초기 위치 설정"으로 위치를 다시 찍어도 지도 위 로봇 마커가 전혀 움직이지 않음.

### 원인

`careguard_turtlebot_agent/uplink_node.py`는 2026-09-07에 "이 로봇은 AMCL/SLAM이 없어서 `/amcl_pose`도, `map->base_link` TF도 없다"는 전제로 위치 소스를 `/odom`(로봇이 부팅한 시점을 원점으로 하는 상대 좌표)으로 정한 상태였다. 코드 주석에도 "AMCL/맵 정렬 위치가 필요해지면 교체하면 된다"고 이미 적혀 있었는데, 오늘 1~4번을 고쳐서 실제로 Nav2/AMCL이 뜨게 되면서 그 전제가 깨졌다 — `set_initial_pose`로 AMCL의 pose를 아무리 다시 잡아도, 대시보드가 보고 있는 값 자체가 맵과 무관한 `/odom` 좌표라 반영될 리가 없었다.

### 해결

LIMO(`careguard_agent/uplink_node.py`)가 순찰(P6) 단계에서 쓰는 방식과 동일하게, position 구독을 `/odom`(`nav_msgs/Odometry`)에서 `/amcl_pose`(`geometry_msgs/PoseWithCovarianceStamped`)로 교체했다. Nav2가 아직 안 떠서 `/amcl_pose`가 없는 동안은 position 자체를 안 보내는 것도 LIMO와 동일한 동작이다 — 맵과 무관한 좌표를 보내느니 안 보내는 편이 낫다는 판단.

### 검증

`POST /patrol-runs/set-initial-pose`로 `(x=3.0, y=-1.5, yaw=0.7)`을 지정한 뒤, 대시보드 P6 화면을 스크린샷으로 직접 확인 — 로봇 위치 마커가 정확히 그 좌표로 이동한 것을 확인했다.

---

## (참고) 부팅 시 Nav2 자동 기동을 시도했다가 롤백한 이력

이번 조사 초반에 "터틀봇 전원이 켜지면 Nav2도 같이 자동으로 뜨게 해달라"는 요청이 있어 `downlink_node` 시작 시 서버의 활성 맵을 조회해 자동으로 `_ensure_navigation_running`을 호출하는 `_bootstrap_navigation` 스레드를 한 번 추가·배포했었다. 이후 사용자가 방향을 바꿔(직접 요청해서) 해당 변경을 `git checkout`으로 롤백하고 로봇에도 원본을 재배포했다 — 현재 코드에는 남아있지 않다. 부팅 자동 기동이 다시 필요해지면 이 트러블슈팅 문서의 1~5번 수정사항(맵/웨이포인트, nav2 파라미터, role 파라미터, position 소스)이 전제조건이라는 점만 참고하면 된다.
