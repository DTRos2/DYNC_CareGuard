# 2026-09-07 트러블슈팅 — P5 SLAM 매핑 / P6 순찰 실 로봇 연동

> `feature/p5-p6-map-patrol-pages` 브랜치, 실제 LIMO 로봇으로 SLAM 저장·매핑 취소·웨이포인트 이동·순찰 위치 표시를 검증하던 중 나온 문제들의 증상·원인·해결을 정리한 기록.
> 대상 파일: `robot_careguard_ws/src/careguard_agent/careguard_agent/{downlink_node.py, uplink_node.py}`, `robot_careguard_ws/src/careguard_mission/careguard_mission/{slam_progress_publisher.py, patrol_task.py}`, `robot_careguard_ws/src/careguard_bringup/launch/navigation_diff_launch.py`, `pc_careguard/server/app/ws/{robot_ws.py, dashboard_ws.py}`, `pc_careguard/server/app/api/v1/{map.py, patrol.py}`, `pc_careguard/dashboard/src/{widgets/waypoint_canvas.*, pages/p6_patrol_page.*}`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | SLAM 맵을 같은 이름으로 다시 저장하면 `.pgm`만 저장소에 남고 `.yaml`은 안 생김 | `map(robot_id, name)` UNIQUE 제약에 걸려 재저장 INSERT가 `UniqueViolation`으로 통째로 실패, `.yaml`을 쓰는 코드까지 못 감 | `INSERT ... ON CONFLICT (robot_id, name) DO UPDATE`로 변경 |
| 2 | "매핑 취소"를 눌러도 지도·라이다 화면이 멈춘 채 안 꺼짐 | 새로 만든 `slam_progress_publisher`가 SIGINT 종료 중 rclpy의 이중 `shutdown()` 호출로 비정상 종료(exit 1), 그룹 종료 확인이 꼬임 | `if rclpy.ok(): rclpy.shutdown()`으로 방어(같은 패턴 4개 노드 전부 적용) |
| 3 | cartographer가 떠있는 터미널에서 Ctrl+C를 눌러도 안 꺼짐 | 버그 아님 — `start_new_session=True`로 일부러 터미널 job control에서 분리해둔 것(앱이 `os.killpg`로만 정리하게 하려는 설계) | 대시보드 "매핑 취소" 버튼 사용, 급하면 `pkill -f cartographer_launch.py` |
| 4 | 와이파이가 끊겼다 붙은 뒤로 카메라는 잘 보이는데 화살표(teleop)·저장 버튼이 안 먹힘 | `uplink_node`(상태 송신 전용)와 `downlink_node`(명령 수신)가 서버의 같은 연결 딕셔너리를 공유해서, uplink가 나중에 재연결되면 downlink의 명령 수신 자리를 가로챔 | WS 접속에 `role=uplink/downlink` 쿼리 추가, 서버가 딕셔너리를 분리 |
| 5 | 지도를 확대하면 웨이포인트 원(마커)이 화면을 다 덮을 만큼 커짐 | 마커를 지도 확대/축소(`p.scale(zoomScale_)`)와 같은 좌표 공간에 고정 크기로 그림 | 마커마다 `1/zoomScale_`로 되돌리는 로컬 변환을 씌워 화면상 크기 고정 |
| 6 | 지도 아래쪽을 클릭하면 웨이포인트 입력 팝업이 창 밖으로 잘려 저장 버튼이 안 보임 | 팝업 위치 계산이 오른쪽·위쪽 경계는 clamp하면서 아래쪽 경계는 안 막음 | 아래쪽도 동일하게 clamp |
| 7 | "현재 위치" 라벨이 실제 다이얼 마커와 멀리 떨어진 곳에 보임 | 라벨이 다이얼과 같은 확대 좌표 공간에 고정 오프셋(-60,-32)으로 그려져, 확대율만큼 오프셋도 같이 커짐 | 라벨 제거, 다이얼(halo+점+방향바늘)만 현재 위치 표시로 남김 |
| 8 | 웨이포인트를 클릭해 "이동"을 눌러도 실제 로봇은 안 움직임 | `driveToSingleWaypoint()`가 서버 호출 없이 캔버스 위에서 다이얼 마커만 가짜로 애니메이션하는 로컬 시뮬레이션이었음 | 실제 서버 API(`POST /patrol-runs/drive-to-point`) 호출로 교체 |
| 9 | 순찰 페이지에 로봇의 실제 위치가 전혀 안 뜸 | amcl이 초기 위치를 못 받아 위치를 추정 못 함 + `navigation_diff_launch.py`가 애초에 `map:=` 인자를 안 받고 있었음(선언 자체가 없어 조용히 무시됨) | 맵 다운로드+Nav2 자동 실행+초기 위치 설정 파이프라인 추가, launch 파일에 `DeclareLaunchArgument('map', ...)` 추가 |

---

## 1. SLAM 맵 재저장 시 `.pgm`만 남고 `.yaml`이 안 생김

**증상**
"SLAM 맵 저장·배포" 버튼을 눌러 실제로 로봇이 지도를 만들었는데, `server/storage/maps/`에 `.pgm` 파일만 있고 `.yaml`은 하나도 없음. 사용자가 재매핑 후 다시 저장했을 때 특히 자주 발생.

**진단 과정**
1. `.yaml` 사이드카를 만드는 `write_map_yaml_sidecar()` 코드 자체는 정상으로 보여, 처음엔 이 함수가 아예 안 불렸을 거라 의심.
2. DB의 `map`/`slam_job`/`files` 행 생성 시각을 커밋 시각과 대조해보니, 지금 저장소에 있는 `.pgm`들은 전부 `.yaml` 사이드카 기능이 커밋되기 **이전**에 만들어진 것들이었음 — 즉 코드 자체는 실행된 적이 없었던 시점의 산물.
3. 로봇 없이 fake WS 클라이언트로 실제 `slam_saved` 메시지를 재현해 최신 코드로 다시 테스트 → **똑같이 실패**, 서버 로그에 `psycopg2.errors.UniqueViolation: duplicate key value violates unique constraint "map_robot_id_name_key"`.

**원인**
`map(robot_id, name)`에 UNIQUE 제약이 있는데, `_save_slam_result`(SLAM 저장)와 `create_map`(파일로 등록) 둘 다 그냥 `INSERT`만 하고 있었음. 같은 로봇에 같은 이름으로 재저장하면(실제로 흔한 시나리오 — "새 맵", "동산주택 2층" 등 같은 이름이 DB에 이미 여러 번 나타남) `UniqueViolation`으로 예외가 나 트랜잭션 전체가 롤백. 문제는 순서: 로봇이 이미지는 `POST /media/upload-map`으로 먼저 올려버린 뒤라, `.pgm`은 남고 그 뒤(DB insert, `.yaml` sidecar)만 통째로 실패.

**해결**
```sql
INSERT INTO map (...) VALUES (..., TRUE)
ON CONFLICT (robot_id, name) DO UPDATE SET
    raw_yaml = EXCLUDED.raw_yaml, resolution = EXCLUDED.resolution, ...,
    is_active = TRUE, created_at = NOW()
RETURNING id
```
새 행을 또 만드는 대신 기존 행을 덮어쓰게 변경(`ws/robot_ws.py::_save_slam_result`, `api/v1/map.py::create_map` 둘 다) — `map.id`가 안 바뀌어야 그 맵에 이미 등록된 waypoint들이 재저장 후에도 유효하기 때문. 같은 map_id에 이미 연결돼 있던 예전 이미지 파일은 soft delete만 해서(`deleted_at`, 실제 파일은 안 지움) `GET /maps/active`의 LEFT JOIN이 중복 행을 내는 것도 같이 방지.

fake WS 클라이언트로 같은 충돌 시나리오를 재현해 수정 전엔 실패, 수정 후엔 `.yaml`이 정상 생성되는 것까지 확인.

**교훈**: "저장은 되는데 부산물 파일이 없다"는 증상은 파이프라인 중간의 예외로 뒷부분이 통째로 스킵됐을 가능성부터 의심할 것. 파일 생성 시각을 커밋 시각과 대조해보는 게 "옛날 코드의 산물인지 최신 코드의 버그인지"를 가르는 데 유용했다.

---

## 2. "매핑 취소"를 눌러도 화면이 멈춘 채 안 꺼짐

**증상**
대시보드에서 "매핑 취소"를 눌러도 SLAM 라이브 뷰(지도·라이다)가 그대로 멈춰있고 반응이 없음. `downlink_node` 로그에:
```
[slam_progress_publisher-4] rclpy._rclpy_pybind11.RCLError: failed to shutdown: rcl_shutdown already called on the given context
[INFO] [cartographer_node-1]: process has finished cleanly
[INFO] [rviz2-3]: process has finished cleanly
[ERROR] [slam_progress_publisher-4]: process has died [pid ..., exit code 1, ...]
```

**진단 과정**
- cartographer_node·rviz2(C++ 노드)는 깔끔히 종료됐는데, 이번에 새로 추가한 `slam_progress_publisher`(유일한 Python/rclpy 노드)만 종료 중 에러로 죽음.
- `_stop_slam()`은 `os.killpg(...)`로 그룹 전체(cartographer_node+rviz2+slam_progress_publisher+ros2 launch 자체)를 죽인 뒤 다 죽을 때까지 `.wait(timeout=5.0)`으로 확인하는 구조 — 그 확인 절차가 이 노드의 비정상 종료로 꼬임.

**원인**
SIGINT를 받으면 rclpy가 내부적으로 먼저 `shutdown()`을 호출해버릴 수 있는데, `slam_progress_publisher.py`의 `finally` 블록이 또 한 번 `rclpy.shutdown()`을 무조건 호출해서 "이미 종료된 컨텍스트를 또 종료하려 함" 에러로 exit code 1이 남. cartographer_node/rviz2는 C++(rclcpp) 노드라 이 rclpy 특유의 경쟁 상태가 없어 문제없이 끝났던 것.

**해결**
```python
finally:
    node.destroy_node()
    if rclpy.ok():
        rclpy.shutdown()
```
같은 보일러플레이트(`finally: node.destroy_node(); rclpy.shutdown()`)가 `patrol_task.py`, `mission_scheduler.py`, `downlink_node.py`, `uplink_node.py`에도 전부 있어서 동일하게 방어 코드 추가. 추가로 `downlink_node._stop_slam()`의 SIGKILL 후 두 번째 `.wait()`도 예외 처리가 없어 혹시 거기서도 타임아웃 나면 예외가 그대로 새 나가 WS 수신 루프 전체가 깨질 수 있었음 — `finally`로 감싸서 어떤 경우든 프로세스 참조가 항상 정리되게 함.

**교훈**: `ros2 launch`로 여러 노드를 한 그룹으로 묶어 관리할 때, Python(rclpy) 노드의 종료 경로가 C++(rclcpp) 노드보다 더 깨지기 쉽다(SIGINT 이중 처리 등). 그룹 종료를 기다리는 코드가 있다면 각 노드의 종료 exit code까지 신경 써야 한다.

---

## 3. 터미널에서 Ctrl+C를 눌러도 cartographer가 안 꺼짐 (버그 아님)

**증상**
`downlink_node`가 cartographer를 띄운 터미널에서 Ctrl+C를 눌러도 cartographer_node·rviz2 등이 안 죽고 로그가 계속 찍힘.

**진단 과정**
`downlink_node._start_slam()`이 `subprocess.Popen(..., start_new_session=True)`로 cartographer를 띄우고 있는 것을 확인.

**원인**
버그가 아니라 의도된 설계. `start_new_session=True`는 cartographer 프로세스 그룹을 터미널의 job control에서 **일부러** 분리시킨다 — 그래야 터미널에서 실수로 Ctrl+C를 눌러도 cartographer_node/occupancy_grid_node/rviz2가 반쯤 죽다 만 고아 프로세스로 안 남고, 앱이 `_stop_slam()`의 `os.killpg`로만 그룹 전체를 예측 가능하게 종료시킬 수 있다(그 반대로 하면, 앱이 그룹 리더 하나에만 신호를 보내는 것과 셸이 포그라운드 그룹 전체에 신호를 보내는 것의 차이 때문에 자식들이 고아로 남는 문제가 실제로 있었다).

**해결**
정상 경로는 대시보드 "매핑 취소" 버튼. 터미널에서 직접 끄고 싶으면:
```bash
pkill -f "ros2 launch careguard_bringup cartographer_launch.py"
```
그래도 안 죽으면 `pkill -9 -f cartographer_node` 등으로 개별 강제 종료. 이렇게 외부에서 죽이면 `downlink_node`가 그 사실을 모르니, 다음 "매핑 시작"이 막히면 `downlink_node` 자체도 재시작하는 게 제일 확실하다.

---

## 4. 와이파이 재연결 후 teleop·저장 명령이 조용히 안 먹힘

**증상**
로봇을 원격지로 옮겨 와이파이가 끊겼다 다시 붙은 뒤, 카메라 라이브 영상은 정상인데 화살표(teleop)를 눌러도 로봇이 안 움직이고 "SLAM 맵 저장" 버튼도 반응이 없음. 에러 팝업도 안 뜸.

**진단 과정**
1. `downlink_node`를 재시작하면 일시적으로 다시 정상 동작 — 재현 가능한 패턴임을 확인.
2. 로봇의 `uplink_node` 로그를 보니 `서버 업링크 연결됨` 메시지가 40~150초 간격으로 계속 반복 — 연결이 끊겼다 재연결되기를 계속 반복 중이었음(와이파이 불안정의 직접 증거).
3. 서버 `ws/robot_ws.py` 코드 확인 → `_connections: Dict[str, WebSocket]` 딕셔너리 **하나**에 robot_code당 소켓을 하나만 저장하고, `downlink_node`와 `uplink_node`가 **서로 다른 WS 연결을 독립적으로** 맺고 있었음(`downlink_node.py` 자체 주석에 "실 운영에서는 두 노드가 연결 하나를 공유하는 편이 낫다"고 이미 알려진 리스크로 적혀 있었음).

**원인**
`send_command()`는 `_connections[robot_code]`에 들어있는 소켓으로만 명령을 보내는데, 와이파이 재연결 타이밍에 따라 **uplink_node의 연결이 나중에** 다시 붙으면 그 자리를 uplink 소켓이 차지해버림. `uplink_node.py`는 들어오는 메시지를 아예 안 읽는 구조(`await ws.wait_closed()`)라, teleop/저장 명령이 그 소켓으로 가서 조용히 사라짐 — 소켓 자체는 살아있어 서버 입장에선 "전송 성공"으로 보여 에러도 안 남.

**해결**
- 서버: `/ws/robot` 엔드포인트에 `role` 쿼리 파라미터 추가, `_connections`(downlink 전용)와 `_telemetry_connections`(uplink 전용)로 분리. `send_command`/`is_connected`는 `_connections`만 본다.
- 로봇: `uplink_node.py`/`downlink_node.py` 접속 URL에 각각 `&role=uplink`/`&role=downlink` 추가.
- fake WS 클라이언트 2개(uplink 역할이 downlink보다 나중에 접속하는 순서)로 재현 → 수정 전엔 downlink가 명령을 못 받음, 수정 후엔 uplink가 나중에 붙어도 downlink가 명령을 계속 받는 것을 확인.

**교훈**: "카메라는 되는데 명령만 안 먹힌다"처럼 일부 기능만 죽는 증상은, 여러 연결/채널이 있는 구조에서 **어느 연결이 문제인지**부터 좁혀야 한다. 두 개의 독립된 소켓이 같은 서버 쪽 자료구조를 공유하면, 겉보기엔 "연결은 돼있다"는 상태인데 실제로는 엉뚱한 소켓과 얘기하고 있는 상태가 나올 수 있다.

---

## 5–7. P6 지도 UI 3종 (마커 확대, 팝업 잘림, 현재위치 라벨)

실 로봇 테스트 스크린샷 3장으로 한 번에 리포트된 문제. 원인은 각각 다르지만 전부 `WaypointCanvas`의 좌표/변환 처리에서 비롯됨.

### 5. 지도 확대 시 웨이포인트 마커가 너무 커짐

**증상**
지도를 휠로 확대하면 웨이포인트 원(마커)과 글자가 화면을 다 덮을 만큼 커짐.

**원인**
`paintEvent()`가 `p.scale(zoomScale_, zoomScale_)`를 건 뒤 마커를 고정 반지름(`kMarkerRadius=15`)으로 그려서, 확대율 4배면 마커도 그대로 4배(화면상 60px)로 커짐. "지도 위 핀은 화면 크기가 항상 일정해야 한다"는 일반적인 지도 UX 기대와 어긋남.

**해결**
마커 하나하나를 `p.save() → p.translate(위치) → p.scale(1/zoomScale_) → 그리기 → p.restore()`로 감싸, 위치는 확대를 따라가되 크기는 화면상 고정되게 함. 클릭 판정 반경(`hitTest`)도 `kHitRadius / zoomScale_`로 같이 맞춰서, "보이는 크기"와 "눌리는 범위"가 어느 배율에서나 일치하게 함.

### 6. 지도 아래쪽 클릭 시 팝업이 창 밖으로 잘림

**증상**
지도 하단 근처를 클릭해 웨이포인트를 추가/편집하려 하면, 입력 팝업이 화면 아래로 넘어가 저장·취소 버튼이 안 보임.

**원인**
```cpp
popover_->move(qMin(anchor.x() + 12, width() - popover_->width() - 8),
               qMax(anchor.y() - 20, 0));
```
x(오른쪽 경계)는 `qMin`, y 위쪽 경계는 `qMax`로 막아놨는데, **y의 아래쪽 경계는 아예 안 막음**.

**해결**
```cpp
const int y = qMin(qMax(anchor.y() - 20, 0), height() - popover_->height() - 8);
```
`handleCanvasAddRequested`/`handleCanvasEditRequested` 양쪽에서 중복되던 위치 계산을 `movePopoverNear()`로 뽑아내며 같이 정리.

### 7. "현재 위치" 라벨이 실제 위치와 다른 곳에 있는 것처럼 보임

**증상**
지도를 확대한 상태에서 "● 현재 위치" 텍스트와, 실제 위치를 나타내는 다이얼 모양 마커(halo+점+방향바늘)가 서로 멀리 떨어져 나타나 마치 별개의 두 마커처럼 보임.

**원인**
사실 하나의 마커였음 — 라벨이 다이얼 중심에서 `(-60, -32)` 고정 오프셋으로 그려지는데, 이 좌표도 마커와 같은 확대 변환(`p.scale(zoomScale_)`) 안에서 그려져서 확대율이 커질수록 라벨과 다이얼 사이 간격도 화면상 비례해서 벌어짐.

**해결**
사용자 요청대로 "● 현재 위치" 텍스트 라벨을 아예 제거하고, halo+점+방향바늘로 구성된 다이얼 마커만 남김(이 마커도 5번과 같은 이유로 화면 고정 크기 처리). 로봇의 실제 위치 데이터(`robot_position` WS 메시지)를 이 마커에 직접 반영하도록 되어 있어, 실제 로봇이 움직이면 이 다이얼도 따라 움직인다.

---

## 8. "이동" 버튼을 눌러도 실제 로봇이 안 움직임

**증상**
P6에서 웨이포인트를 클릭해 "이동"을 실행해도 로봇이 그 자리에서 꼼짝 안 함.

**진단 과정**
`P6PatrolPage::driveToSingleWaypoint()` 코드를 확인:
```cpp
void P6PatrolPage::driveToSingleWaypoint(const QString& id) {
    ...
    canvas_->startTestRun({id}, chargeId, /*returnToCharging=*/false);
}
```
서버 API 호출이 **하나도 없음** — `startTestRun()`은 화면 위에서 다이얼 마커를 가짜 경로 따라 애니메이션만 시키는 로컬 시뮬레이션(원래 "테스트 실행" 버튼을 위해 만든 기능)이었음.

**원인**
"이동" 클릭 핸들러가 실제로 구현된 적이 없고, 데모용 로컬 애니메이션 함수를 그대로 재사용하고 있었음. 이 로컬 애니메이션이 실제 로봇 위치와 같은 상태 변수(`currentRobotPos_`)를 공유해서, 7번 항목의 "현재 위치가 엉뚱한 곳에 있다"는 증상의 원인 중 하나이기도 했음.

**해결**
- `Patrol.action`에 `bool return_to_charging` 필드 추가 — 전체 순찰(true, 다 돌고 충전소 복귀)과 단일 이동(false, 도착한 자리에 머묾)을 같은 액션 서버로 구분.
- 서버에 `POST /patrol-runs/drive-to-point` 신설 — `patrol_run` 기록 없이(일회성 명령) `run_id=0`으로 기존 `patrol_start` 파이프라인 재사용.
- GUI가 로컬 애니메이션 대신 이 API를 실제로 호출하도록 교체. 다이얼 마커는 이제 이 클릭이 아니라 로봇이 실제로 보내는 `robot_position` WS 메시지로만 움직임(로컬 시뮬레이션과 다시 섞이지 않게).
- 로컬 fake WS 클라이언트로 서버가 실제로 `{"type":"patrol_start","run_id":0,"return_to_charging":false,...}`를 로봇에 전달하는 것까지 확인(로봇 쪽 Nav2 동작 자체는 실 로봇 검증 필요로 남음).

**교훈**: "버튼을 눌렀는데 화면에 뭔가 반응은 있다"는 게 "실제로 서버/로봇까지 명령이 갔다"는 뜻은 아니다. 로컬 데모/프리뷰용으로 만든 애니메이션 함수가 나중에 실제 기능의 자리를 대신 차지하고 있는 경우, 코드를 직접 읽어보지 않으면 알아채기 어렵다.

---

## 9. 순찰 페이지에 로봇의 실제 위치가 안 뜸

**증상**
P6 순찰 페이지에 진입해도(혹은 순찰을 시작해도) 로봇의 실제 현재 위치가 화면에 전혀 표시되지 않음.

**진단 과정**
1. 위치 데이터의 출처는 `uplink_node`가 구독하는 `/amcl_pose`(Nav2의 AMCL이 발행) — SLAM 중에는 별도 TF 기반 경로로 위치가 나오지만, 순찰 중에는 AMCL이 필수임을 확인.
2. AMCL은 **초기 위치를 받아야** 위치 추정을 시작한다는 점 확인 — `careguard_nav/initial_pose_setter.py`라는, 충전 스테이션 좌표를 `/initialpose`로 자동 발행해주는 노드가 이미 존재했지만, **`careguard_bringup`의 어떤 launch 파일에도 연결돼 있지 않아** 아무도 자동으로 실행해주지 않았음.
3. 자동화를 만들던 중 `navigation_diff_launch.py`를 다시 읽어보니 결정적 문제 발견:
   ```python
   map_file_name = 'mapd_origin.yaml'
   map_file_path = os.path.join(bringup_share_dir, 'maps', map_file_name)
   ```
   `map_file_path`가 **그냥 파이썬 지역 변수**였음 — `DeclareLaunchArgument`가 없어서 `ros2 launch ... map:=<경로>`로 실행해도 ros2 launch가 그 이름의 인자 자체를 몰라 조용히 무시하고, 항상 하드코딩된 기본 맵(`mapd_origin.yaml`)만 쓰이고 있었을 것으로 추정됨.

**원인**
두 가지가 겹쳐 있었음: (a) 초기 위치 설정이 자동화 안 됨, (b) 애초에 로봇/맵별로 다른 지도를 Nav2에 넘길 방법 자체가 launch 파일 버그로 막혀 있었음.

**해결**
- `navigation_diff_launch.py`에 `DeclareLaunchArgument('map', default_value=...)` + `LaunchConfiguration('map')` 추가해 실제로 오버라이드가 먹히게 수정.
- `downlink_node`에 `_ensure_navigation_running(map_id)` 추가 — `patrol_start`(순찰/이동) 또는 신설한 `nav_prepare` 명령을 받으면: 서버 `GET /maps/active`에서 맵 이미지+메타데이터 다운로드 → 로컬에 `.pgm`/`.yaml` 생성 → `navigation_diff_launch.py` 실행 → amcl이 `/amcl_pose`를 advertise할 때까지 대기(최대 30초) → `ros2 run careguard_nav initial_pose_setter`로 초기 위치 자동 설정.
- SLAM과 Nav2는 동시 실행 불가라 상호 배타 처리(매핑 중엔 순찰 자동 시작 거부, 순찰/Nav2 중 매핑 시작하면 Nav2를 먼저 자동 종료).
- "웨이포인트를 보내기 전에도 위치부터 보고 싶다"는 요청에 맞춰, 순찰 명령을 실제로 누르기 전에도 P6 페이지 진입 시(`showEvent`) 자동으로 `nav_prepare`를 보내 Nav2를 미리 준비시킴 — 단, 로봇 부팅 시 무조건 자동 실행하는 방식은 SLAM부터 시작하는 흔한 워크플로우에서 매번 낭비가 생기므로 채택하지 않고, "P6 페이지를 실제로 보러 왔을 때"로 트리거 시점을 좁힘.
- 다운로드한 맵의 `id`가 요청받은 `map_id`와 실제로 일치하는지 검증 추가 — 재매핑 직후처럼 대시보드가 들고 있는 맵 정보가 서버의 최신 활성 맵과 어긋나는 순간 조용히 엉뚱한 맵으로 초기 위치를 잡는 것을 방지.

**교훈**: launch 파일에 인자를 "받는 것처럼 보이는" 코드(그냥 지역 변수 + 주석)와 실제로 `DeclareLaunchArgument`로 선언해 받는 코드는 겉보기엔 구분이 안 간다 — CLI 오버라이드가 실제로 반영되는지는 반드시 실행해서 확인해야 한다. 자동화 기능을 새로 얹기 전에, 그 밑에 깔린 launch 파일이 애초에 파라미터화되어 있는지부터 검증할 것.
