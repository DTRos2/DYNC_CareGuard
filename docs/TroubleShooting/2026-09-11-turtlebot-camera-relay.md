# 2026-09-11 트러블슈팅 — 터틀봇(R-05/R-06) 카메라가 대시보드에 안 뜨던 문제

> 작성자: 최치언
> "맵·로봇"/"순찰·경로" 화면에서 LIMO(R-07)는 카메라가 잘 뜨는데 터틀봇(R-05/R-06)만 계속 "카메라 연결 중"으로 멈춰있던 문제를 조사·해결한 기록. 원인이 세 겹으로 겹쳐 있었다 — 터틀봇 쪽 코드에 카메라 릴레이 자체가 없었던 것, 카메라 드라이버의 픽셀 포맷을 JPEG 인코더가 인식 못 해 빈 프레임만 나가던 것, 그리고 카메라가 물리적으로 뒤집혀 마운트돼 있던 것.
> 대상 파일: `careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py`, `careguard_turtlebot_agent/systemd/careguard-camera-r05.service`(신규), `careguard_turtlebot_agent/systemd/careguard-camera-r06.service`(신규), `careguard_turtlebot_agent/systemd/careguard-uplink-{r05,r06}.service`, `careguard_turtlebot_agent/systemd/install.sh`, `careguard_turtlebot_agent/package.xml`
> 대상 로봇: R-05(aurix@192.168.230.9, `medirun_bringup`), R-06(yeyu@100.110.210.121, `turtlebot3_bringup`)

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | R-05/R-06 카메라 화면이 항상 "카메라 연결 중"만 표시 | `careguard_turtlebot_agent/uplink_node.py`(터틀봇용)에 카메라 구독·릴레이 코드 자체가 없었음(1단계 범위에서 의도적으로 제외됨) — 대시보드·서버 로직은 로봇 종류와 무관하게 정상 동작 | 로봇에 실제 카메라(`camera_ros`/libcamera, imx219)가 있는지 SSH로 직접 확인 후, LIMO(`careguard_agent/uplink_node.py`)와 동일한 패턴으로 `/camera/image_raw/compressed` 구독·relay 코드 추가 |
| 2 | R-06에서 카메라 노드를 띄워도 서버에 `camera_frame`이 안 옴(구독은 정상 매칭됨) | `camera_ros`가 기본 픽셀 포맷 NV12 미지원이라 NV21로 자동 선택했는데, `compressed_image_transport`의 JPEG 인코더가 NV21을 인식 못 해("Unrecognized image encoding [nv21]") 매 프레임 헤더만 있고 `data`가 빈 `CompressedImage`만 발행 | `camera.launch.py`에 `format:=RGB888` 파라미터를 명시해 인식 가능한 포맷으로 강제 |
| 3 | 화면에는 뜨는데 화면이 위아래로 뒤집혀 나옴(R-05, R-06 둘 다) | 카메라가 물리적으로 뒤집혀 마운트돼 있는데, 이 로봇들의 libcamera 버전(0.1)은 `camera_ros`의 `orientation` 파라미터를 지원하지 않음("parameter 'orientation' not supported on libcamera 0.1") — 드라이버 단에서 회전 불가 | `uplink_node.py`에서 cv2로 JPEG 디코딩 → `cv2.flip(img, 0)`(상하반전) → 재인코딩하는 소프트웨어 처리 추가. 환경변수(`CAREGUARD_CAMERA_FLIP_VERTICAL=1`)로만 켜서 LIMO/영향 없는 로봇은 완전히 패스스루 |
| 4 (부분 해결) | `camera_ros` 노드가 가끔 "Device or resource busy"/"failed to acquire camera"로 실패 | libcamera 파이프라인 핸들러 경합(근본 원인 미확정)에 더해, **`ros2 launch`가 하위 `component_container`의 크래시와 무관하게 launch 프로세스 자체는 exit code 0으로 끝나버려 systemd `Restart=on-failure`가 재시작을 안 걸어줌**(2026-09-12, R-05가 실제로 이 상태로 몇 시간 동안 멈춰있었음) — "자동 재시도되니 문제없다"던 기존 결론이 틀렸었다 | `careguard-camera-{r05,r06}.service`의 `Restart`를 `on-failure`→`always`로 변경해 두 로봇에 재배포 — 종료 코드와 무관하게 항상 재시작하도록 수정. libcamera 자체의 경합 원인은 여전히 미확정 |
| 5 (잠정 해결) | 카메라 릴레이를 켠 뒤 R-05/R-06이 대시보드에서 온라인/오프라인을 반복 | `uplink_node` 프로세스가 CPU를 33~41%(Pi 4, 4코어)씩 쓰고 있어 의심됨 — 확정은 못함 | 비압축 원본 구독 시도는 되돌리고(오히려 CPU 증가), `CAMERA_SEND_INTERVAL_SEC`을 5Hz→3Hz로 낮춰 재배포(2026-09-12). CPU는 소폭 줄었고(R-05 22.2%→16.9%, R-06 29.4%→26.7%), 이후 대시보드 `/ws/dashboard`를 30분간 직접 관찰한 결과 `robot_status` 전환(온라인↔오프라인)이 **0건** — 흔들림 재현 안 됨 |

---

## 1. 터틀봇 카메라 릴레이 코드 자체가 없었음

### 증상

대시보드 "맵·로봇 매핑"/"순찰·경로" 페이지에서 R-07(LIMO)은 카메라 미리보기가 정상인데, R-05/R-06(터틀봇)은 항상 `CameraLiveView`의 기본 placeholder("카메라 연결 중")만 표시되고 절대 실제 영상으로 안 바뀜.

### 진단 과정

1. 대시보드 쪽(`p5_map_page.cpp:623`, `p6_patrol_page.cpp:2510`)을 먼저 확인 — `camera_frame` WS 메시지 타입을 로봇 종류 구분 없이 그대로 처리하는 범용 코드였음. 대시보드 버그는 아님으로 판단.
2. 서버(`app/ws/robot_ws.py:199-208`)도 `camera_frame`을 로봇 구분 없이 그대로 relay만 하는 순수 중계였음. 서버 버그도 아님.
3. 로봇 쪽으로 좁혀서 `careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py`(터틀봇용)를 열어보니 `/odom`, `/battery_state`만 구독하고 있었고, 카메라 관련 import·구독·전송 코드가 전혀 없었음. 파일 상단 docstring에 이미 명시돼 있었음: "지도·카메라 등 다른 토픽 릴레이는 이번 범위 밖이라 구독하지 않는다 — 필요해지면 careguard_agent/uplink_node.py를 참고해 여기에 추가하면 된다."
4. `robot_careguard_ws/src/careguard_agent/careguard_agent/uplink_node.py`(LIMO용)는 `/camera/color/image_raw/compressed`를 구독해 5Hz(`CAMERA_SEND_INTERVAL_SEC = 0.2`)로 relay하고 있었음 — LIMO와 터틀봇은 애초에 완전히 분리된 별도 파일(포지션 소스도 다름: LIMO는 map->base_link TF, 터틀봇은 /odom)이라 한쪽만 기능이 빠져 있어도 서로 영향이 없다.
5. 터틀봇에 실제 카메라 하드웨어가 있는지부터 SSH로 직접 확인 필요 — `ros2 topic list`로는 R-05/R-06 둘 다 카메라 토픽이 전혀 없었으나(`turtlebot3_bringup robot.launch.py`, `medirun_bringup pi_bringup.launch.py` 둘 다 카메라 노드를 launch에 포함 안 함), `ros2 pkg list | grep -i cam`으로 두 로봇 모두 `camera_ros`(libcamera ROS2 래퍼) 패키지가 이미 설치돼 있고 `/dev/video0~31`(라즈베리파이 libcamera 특유의 다수 video 노드 패턴)이 있음을 확인 — 카메라 하드웨어(imx219, 라즈베리파이 카메라 모듈)는 있는데 그냥 켜져 있지 않았을 뿐.

### 원인

터틀봇 uplink_node는 1단계 개발 범위에서 카메라를 의도적으로 제외한 상태 그대로였고, 로봇 bringup에도 카메라 노드가 launch되지 않고 있었다 — 즉 "버그"가 아니라 "아직 구현/설정 안 된 기능"이었다.

### 해결

1. `careguard-camera-r05.service`, `careguard-camera-r06.service`(신규 systemd 유닛) — `ros2 launch camera_ros camera.launch.py`를 각 로봇 bringup에 `BindsTo`로 묶어 자동 기동.
2. `uplink_node.py`에 LIMO와 동일한 패턴으로 카메라 구독·relay 코드 추가:
   ```python
   self.create_subscription(CompressedImage, '/camera/image_raw/compressed',
                             self._on_camera_frame, qos_profile_sensor_data)
   ```
   토픽 이름은 LIMO(orbbec_camera, `/camera/color/image_raw/compressed`)와 다르다 — `camera_ros`가 image_transport로 발행하면 `compressed_image_transport` 플러그인이 `/camera/image_raw/compressed`를 별도 republish 노드 없이 자동으로 같이 발행해줘서, 재인코딩 없이 그대로 relay 가능.
3. `careguard_turtlebot_agent/systemd/install.sh`를 확장 — 카메라 유닛은 그 로봇에 실제로 있을 때만(파일 존재 여부로 판단) 선택적으로 설치하도록 배열 기반으로 재작성.

**검증**: 서버에 직접 websocket 클라이언트로 붙어 10초간 수신 메시지를 집계 — R-05/R-06 둘 다 `camera_frame`이 초당 4.6개(LIMO와 동일한 쓰로틀링 속도)로 안정적으로 들어옴을 확인.

**교훈**: "왜 한쪽만 안 되지?"를 조사할 때, 코드가 완전히 없는 경우("아직 안 만들어짐")와 코드는 있는데 오동작하는 경우("버그")를 먼저 구분해야 한다. 이번엔 docstring에 스코프 제외 사유가 이미 친절하게 적혀 있어서 빨리 좁혀졌지만, 그런 문서화가 없었다면 "대시보드가 로봇 종류를 구분 못 하나?" 같은 엉뚱한 방향으로 먼저 파고들었을 수 있다.

---

## 2. 카메라를 띄워도 서버에 프레임이 안 옴 — NV21을 JPEG 인코더가 인식 못 함

### 증상

1절 코드를 배포하고 R-06에서 `camera_ros`를 실제로 띄웠는데도, 서버에 붙어 직접 확인해보면 `camera_frame(R-06)`이 전혀 안 옴(같은 시간대 R-07 카메라는 정상 수신). ROS 그래프상으로는 `uplink_node`가 `/camera/image_raw/compressed`에 정상 구독돼 있었고(`ros2 topic info --verbose`로 publisher/subscriber 매칭 확인), `ros2 topic hz`로는 30Hz 발행도 확인됨 — "연결은 됐는데 데이터가 안 온다"는 이상한 상태.

### 진단 과정

1. 독립적인 python 테스트 스크립트로 같은 토픽·QoS를 구독해보니 콜백은 초당 30번씩 정상 호출됨 — ROS 통신 자체는 문제 없음.
2. `uplink_node.py`의 `_on_camera_frame`에 임시 디버그 로그(`get_logger().info(f'... len={len(msg.data)} ...')`)를 추가해 재배포 후 확인 — 콜백은 매번 호출되는데 **`len(msg.data)`가 항상 0**이었다. 코드의 `if not msg.data: return`에 걸려 애초에 `_send()`까지 도달하지 못하고 있었음.
3. `ros2 topic echo /camera/image_raw/compressed --no-arr`로 다른 방식(우리 코드와 무관한 순수 CLI 도구)으로 직접 확인해도 동일하게 `data: length 0`, `format: ''`, `stamp: 0` — 우리 코드 문제가 아니라 애초에 발행되는 메시지 자체가 빈 껍데기였다.
4. `camera_ros`의 `component_container` 로그를 보니 `[ERROR] [camera]: Unrecognized image encoding [nv21]`가 프레임마다 반복되고 있었다. 앞서 카메라 시작 로그에 `default pixel format (NV12) not supported`, `no pixel format selected, auto-selecting: "NV21"`이라는 WARN이 있었는데, 이렇게 자동 선택된 NV21을 `compressed_image_transport`의 JPEG 인코더가 지원하지 않아 압축에 매번 실패 → 빈 메시지만 발행되고 있었던 것.

### 원인

`camera.launch.py`가 `format` 파라미터를 기본값(빈 문자열)으로 두면 카메라가 지원 포맷 중 하나를 자동 선택하는데, 이 센서(imx219)에서 자동 선택된 NV21이 하필 JPEG 압축 플러그인과 호환되지 않았다.

### 해결

`format:=RGB888`을 명시(`camera.launch.py camera:=0 format:=RGB888`). 재시작 후 `ros2 topic echo`로 `format: jpg`, `data: length 61706` 같은 실제 데이터가 나오는 것을 확인.

**교훈**: `ros2 topic hz`는 메시지가 "발행되고 있는지"(빈도)만 보여줄 뿐 "내용이 채워져 있는지"는 보장하지 않는다. "연결은 되는데 내용이 없다"는 의심이 들면 `ros2 topic echo`로 실제 필드 값을 봐야 한다 — 이번처럼 인코딩 실패가 예외를 던지지 않고 조용히 빈 메시지만 내보내는 경우, hz/구독 매칭만 봐서는 절대 못 잡는다.

---

## 3. 카메라가 물리적으로 뒤집혀 마운트됨 — 드라이버가 회전을 지원하지 않음

### 증상

2절까지 고치고 나니 서버에 정상적으로 프레임이 도착했지만, 실제 이미지를 받아 확인해보니 R-05/R-06 둘 다 화면이 위아래로 뒤집혀 나왔다.

### 진단 과정

1. `camera_ros`의 `orientation` 파라미터(0/90/180/270, libcamera의 `Transform` 매핑)를 발견 — `ros2 param list /camera`로 존재를 확인, `ros2 param describe`로 "Read only: true"(런타임 변경 불가, 실행 시점에만 지정 가능)임을 확인.
2. `ros2 launch camera_ros camera.launch.py orientation:=180`처럼 붙여봤는데, `camera.launch.py`가 `camera`/`format`만 `DeclareLaunchArgument`로 선언해뒀지 `orientation`은 launch 인자로 안 뚫려 있어서 `ros2: error: unrecognized arguments`로 거부됨.
3. `ros2 pkg executables camera_ros`로 찾은 단독 실행형 `camera_node`(컴포저블이 아닌 일반 노드)에 `--ros-args -p orientation:=180`으로 직접 시도 — 이번엔 실행은 됐지만 `[WARN] [camera]: parameter 'orientation' not supported on libcamera 0.1`로 무시되고, 곧이어 다른 이유(파이프라인 핸들러 경합, 4번 항목 참고)로 카메라 획득 자체에 실패해 종료됨.
4. 즉 이 로봇들에 설치된 libcamera 버전(0.1)에서는 `camera_ros`를 통한 방식으로는 회전/반전을 걸 수 없다는 게 확인됨 — 드라이버·라이브러리 버전을 올리는 건 이번 범위 밖(다른 기능에 영향 줄 수 있는 시스템 전역 변경).

### 원인

드라이버 레벨에서 안 되면 값을 못 바꾸는 게 아니라, 소프트웨어(수신 측)에서 이미지 자체를 처리해서 바꾸면 된다 — `orientation` 파라미터가 막힌 것이지 "이미지를 뒤집을 방법이 없다"는 뜻은 아니었다.

### 해결

`uplink_node.py`의 `_on_camera_frame`에서, 환경변수 `CAREGUARD_CAMERA_FLIP_VERTICAL=1`이 켜진 로봇만 cv2로 JPEG 디코딩 → `cv2.flip(img, 0)` → 재인코딩 후 전송하도록 분기 추가:

```python
CAMERA_FLIP_VERTICAL = os.environ.get('CAREGUARD_CAMERA_FLIP_VERTICAL', '0') == '1'
...
jpeg_bytes = bytes(msg.data)
if CAMERA_FLIP_VERTICAL:
    image = cv2.imdecode(np.frombuffer(jpeg_bytes, dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        return
    flipped = cv2.flip(image, 0)
    ok, encoded = cv2.imencode('.jpg', flipped)
    if not ok:
        return
    jpeg_bytes = encoded.tobytes()
```

`careguard-uplink-r05.service`, `careguard-uplink-r06.service` 둘 다에 `Environment=CAREGUARD_CAMERA_FLIP_VERTICAL=1`을 추가했다(둘 다 같은 방식으로 뒤집혀 마운트돼 있었음). 이 플래그를 안 켜는 로봇(LIMO)은 `if CAMERA_FLIP_VERTICAL` 분기 자체를 안 타서 기존과 완전히 동일한 패스스루 경로로 동작 — 디코딩·재인코딩 비용이 전혀 안 붙는다. `package.xml`에 `python3-opencv`/`python3-numpy` 의존성을 추가했다(둘 다 로봇에 이미 설치돼 있었지만 패키지 매니페스트에는 없었음).

**검증**: 서버에서 받은 실제 JPEG 프레임을 파일로 저장해 육안 확인 — R-05는 의자에 앉은 사람, R-06은 책상 밑 PC가 둘 다 똑바로(뒤집히지 않게) 보임.

**교훈**: 하드웨어/드라이버가 어떤 기능(이번엔 회전)을 지원하지 않는다고 그 문제를 못 고치는 건 아니다. 파이프라인의 다른 지점(이번엔 릴레이 코드 쪽, 즉 수신 측 소프트웨어)에서 같은 효과를 낼 수 있는지 먼저 찾아보는 게, 드라이버 업그레이드처럼 범위가 넓고 위험한 변경보다 안전하다. 단, 사용자가 요청한 "상하반전"이 엄밀히는 카메라가 180도 돌아간 것(상하+좌우 모두 뒤집힘)일 수도 있어 `cv2.flip(img, -1)`이 정답일 수도 있었는데, 실제 캡처 이미지로 확인해보니 이 로봇들은 순수 상하 방향으로만 뒤집혀 있어 `cv2.flip(img, 0)`(세로축 기준)만으로 정확히 맞았다 — 이런 종류의 보정은 짐작하지 말고 반드시 실제 이미지로 검증해야 한다.

---

## 4. (부분 해결) camera_ros가 가끔 카메라 획득에 실패함

### 증상

`camera_ros`(라즈베리파이 카메라)를 연속으로 껐다 켰다 하면, 가끔 아래처럼 실패한다:
```
[ERROR] [V4L2] v4l2_device.cpp:353 'imx219 10-0010': Unable to set controls: Device or resource busy
[INFO] [Camera] camera.cpp:840 Pipeline handler in use by another process
[ERROR] [launch_ros.actions.load_composable_nodes]: Failed to load node '' ...: failed to acquire camera
```
`fuser`로 확인해도 그 시점엔 다른 프로세스가 디바이스를 잡고 있지 않다(다른 진짜 프로세스와의 충돌은 아님).

### 원인 (추정, 미확정)

libcamera의 파이프라인 핸들러가 이전 프로세스 종료 직후 아주 짧게 "정리 중" 상태로 남아있다가, 그 사이에 새 프로세스가 획득을 시도하면 경합이 나는 것으로 보인다 — 정확한 내부 메커니즘까지는 이번에 확인하지 않았다.

### 현재 상태 (2026-09-11 시점)

재시도(프로세스를 다시 시작)하면 예외 없이 잡힌다. 운영 중에는 `Restart=on-failure`(`RestartSec=5`)로 이미 자동 재시도되므로 실사용에 지장은 없을 것으로 보이나, 재부팅 직후 등 타이밍이 안 좋으면 첫 기동에 한 번 실패할 수 있다는 점은 알아둬야 한다. 더 확실한 해결(예: `ExecStartPre`에 재시도 로직 추가, 또는 원인 자체를 libcamera 이슈 트래커에서 확인)은 이번 세션에서는 진행하지 않았다.

### 추가 발견 및 조치 (2026-09-12) — "자동 재시도되니 문제없다"는 결론이 틀렸었다

사용자가 "R-05 카메라가 안 들어온다"고 리포트해서 확인해보니, `careguard-camera-r05.service`가 **15:49:54부터 `inactive (dead)` 상태로 몇 시간째 멈춰있었다** — 위에서 "운영 중엔 지장 없다"고 판단했던 `Restart=on-failure` 자동 재시도가 실제로는 전혀 작동하지 않고 있었다.

**원인**: 저널을 보니 `component_container`가 `exit code -6`(SIGABRT)으로 죽었는데도, 그걸 감싸고 있는 `ros2 launch` 프로세스 자체는 곧이어 "Deactivated successfully"로 **exit code 0(성공)**으로 끝났다. systemd 입장에서는 `ExecStart`가 지정한 프로세스(`ros2 launch`)가 정상 종료한 것으로 보이므로, `Restart=on-failure`(비정상 종료에만 재시작)는 아예 발동하지 않는다 — "재시도가 자동으로 된다"는 4번 항목의 기존 결론은 이 실패 모드(하위 노드는 죽었는데 launch는 exit 0으로 끝나는 경우)를 놓친 것이었다.

**해결**: R-05를 즉시 수동으로 `systemctl restart`해 복구한 뒤, `careguard-camera-r05.service`·`careguard-camera-r06.service` 둘 다 `Restart=on-failure` → `Restart=always`로 변경해 재배포했다 — 종료 코드가 무엇이든(0이든 비정상이든) 항상 재시작하도록 바꿔서, 이번처럼 launch가 exit 0으로 조용히 끝나버리는 경우에도 자동 복구되게 했다.

**현재 상태**: 자동 재시도가 실제로 걸리는지는 이번 재발이 다시 일어나야 확인 가능하다(의도적으로 재현시키지는 않음). libcamera 파이프라인 핸들러 경합이라는 근본 원인 자체는 여전히 미확정이라 "부분 해결"로 남긴다 — 이번에 고친 건 "실패했을 때 실제로 복구되는가"이지, "왜 가끔 실패하는가"가 아니다.

---

## 5. (잠정 해결) 카메라 릴레이를 켠 뒤 로봇이 온라인/오프라인을 반복

### 증상

1~3절을 다 고쳐 카메라가 정상적으로 보이기 시작한 뒤, 대시보드에서 R-05/R-06이 번갈아가며 온라인 → 오프라인 → 온라인으로 상태가 계속 바뀐다는 실사용 리포트.

### 진단 과정

1. 서버 `/ws/dashboard`에 직접 붙어 30~90초씩 여러 번 `robot_status` 이벤트를 관찰 — 그 짧은 관찰 창에서는 흔들림이 안 잡혔다(1회성이거나 더 뜸한 간격일 가능성).
2. 로봇 쪽 `careguard-uplink-*.service` 저널을 좀 더 넓게(15분) 살펴보니, R-06에서 실제로 재연결이 한 번 있었다(13분 만에 1회) — 활발하게 반복되는 흔들림이라기보다는 가끔 발생하는 끊김에 가까웠다.
3. 그 사이 `top`으로 확인한 CPU 사용률이 눈에 띄게 높았다 — `uplink_node` 33~41%, 카메라 `component_container` 44~53%(둘 다 Pi 4, 4코어). 로봇은 이미 Nav2 스택(controller_server, smoother_server, lifecycle_manager 등) 전체를 같이 돌리고 있어 코어 여유가 넉넉하지 않은 상태였다.
4. 카메라 릴레이(3절에서 추가한 "JPEG 디코딩 → 뒤집기 → 재인코딩")가 CPU 증가의 주 원인으로 의심되어, 압축 해제 단계를 없애면 가벼워질 거라 보고 **비압축 원본 `/camera/image_raw`(sensor_msgs/Image)를 직접 구독해 뒤집고 인코딩만 한 번 하는 방식**으로 바꿔봤다(`cv_bridge` 사용).
5. 실측 결과는 예상과 반대였다 — `uplink_node` CPU가 오히려 44~50%로 **늘었다**. 원인은 대역폭·역직렬화 비용이었다: 640x480 RGB888 비압축 프레임은 30Hz로 초당 약 27MB인데, 우리 콜백은 그 중 5개(5Hz 쓰로틀링)만 실제로 처리하고 나머지는 버려도, rclpy가 구독한 이상 30Hz 전부를 매번 역직렬화(수신·메모리 할당)해야 한다 — 이 비용이 JPEG 디코딩(압축된, 훨씬 작은 데이터를 대상으로 5Hz만 수행) 비용보다 더 컸다.

### 원인

(미확정) CPU 사용률이 높은 것 자체는 확인했지만, 그게 온라인/오프라인 흔들림의 직접 원인인지(예: asyncio 웹소켓 이벤트 루프가 GIL 경합·스케줄링 지연으로 서버와의 핑퐁/재연결 타임아웃을 놓치는 식)까지는 이번 세션에서 인과관계를 확정하지 못했다. "압축 해제를 없애면 가벼워질 것"이라는 직관도 이번엔 틀렸다 — 압축 전송 자체가 대역폭·역직렬화 비용을 줄여주는 몫이 디코딩 비용보다 컸다.

### 해결 (부분적)

CPU를 늘리기만 한 raw 구독 방식은 되돌리고, 원래의 "압축 토픽 구독 + 필요할 때만 디코딩·뒤집기·재인코딩" 방식으로 복귀했다. 되돌린 뒤 재측정한 CPU는 `uplink_node` 기준 R-06 29.4%, R-05 22.2%로, 실험 전과 비슷하거나 조금 더 낮은 수준으로 확인됐다.

**교훈**: "무거워 보이는 처리 단계(디코딩)를 없애면 가벼워질 것"이라는 직관만으로 최적화하지 말고, 반드시 실측해야 한다. ROS2/DDS에서는 "얼마나 자주 쓰는가"뿐 아니라 "구독한 토픽이 초당 얼마나 큰 데이터를 실어 나르는가"가 처리 비용에 못지않게 큰 비중을 차지할 수 있다 — 이번처럼 압축 여부가 다른 두 토픽을 놓고 고를 때는 압축된 쪽이 "덜 자주 압축을 풀더라도" 더 유리할 수 있다.

### 추가 조치 (2026-09-12)

`CAMERA_SEND_INTERVAL_SEC`을 5Hz(0.2s)에서 3Hz(약 0.33s)로 낮췄다 — 2Hz까지 낮추는 것도 검토했으나, 대시보드 우측 상단 미리보기 박스 용도로는 2Hz는 체감상 뚝뚝 끊겨 보일 수 있는 반면 3Hz는 충분히 부드러우면서 디코딩·재인코딩·base64 인코딩·전송 처리 횟수를 40% 줄일 수 있어 절충안으로 택했다.

두 로봇에 재배포(`careguard_turtlebot_agent/careguard_turtlebot_agent/uplink_node.py` 소스를 로봇 워크스페이스로 복사 → `colcon build --packages-select careguard_turtlebot_agent` → `careguard-uplink-{r05,r06}.service` 재시작)한 뒤 약 3분간 안정화 후 재측정한 `uplink_node` CPU:

| 로봇 | 5Hz(기존) | 3Hz(변경 후) |
|---|---|---|
| R-05 | 22.2% | 16.9% |
| R-06 | 29.4% | 26.7% |

R-05는 CPU가 뚜렷이 줄었지만(약 24% 감소), R-06은 감소 폭이 크지 않았다(약 9% 감소) — 구독 자체(`/camera/image_raw/compressed`)는 여전히 30Hz로 들어오고 있어 콜백 진입·DDS 역직렬화 비용은 Hz를 낮춰도 그대로이고, 줄어드는 건 스로틀을 통과한 프레임에 대한 디코딩·플립·재인코딩·base64·전송 비용뿐이라 로봇마다(다른 프로세스 부하, 스케줄링 상황 등) 체감 절감 폭이 다를 수 있다.

**기대 효과**

1. CPU 절감: 프레임당 디코딩→뒤집기→재인코딩→base64 인코딩→전송 처리 횟수가 초당 5회→3회로 40% 줄어든다.
2. 다른 프로세스와의 자원 경합 완화: 이 로봇들은 Nav2 스택(주행 제어)도 같이 돌리고 있어, `uplink_node`가 CPU를 덜 쓰면 주행 관련 노드들이 겪는 스케줄링 지연이 줄어들 가능성이 있다.
3. 네트워크 전송량 감소: 초당 서버로 나가는 JPEG 프레임 수가 줄어드는 만큼 대역폭도 그만큼 줄어든다.
4. (가설, 미검증) 온라인/오프라인 흔들림 완화: CPU 과부하가 웹소켓 이벤트 루프의 핑퐁 타이밍을 놓치는 원인이었다면 개선될 수 있으나, 이번 세션에서는 실측 검증 전이다 — R-06은 CPU가 여전히 26%대로 크게 줄지 않아, 다음에도 흔들리면 CPU가 근본 원인이 아닐 가능성이 높다.

### 검증 (2026-09-12)

3Hz 재배포 후, 서버 `/ws/dashboard`에 파이썬 스크립트(`websockets`)로 직접 붙어 `robot_status` 이벤트를 30분간(16:17:30~16:47:30) 수집했다. 그 사이 두 로봇 다 실제로 사용(대시보드 접속) 중이었다.

- 관찰된 `robot_status` 전환: 최초 접속 시 hydrate된 `R-05 online=True`, `R-06 online=True` 2건뿐 — 이후 30분 내내 **추가 전환 0건**.
- 로봇 쪽 `careguard-uplink-{r05,r06}.service` 저널도 같은 구간에서 "연결됨"/"연결 실패" 로그가 0건 — 재연결 자체가 한 번도 없었다.
- 관찰 시작 직전에 서버(FastAPI+Postgres)가 이 세션에서 한동안 내려가 있었던 걸 발견해 다시 띄운 뒤 진행했다 — 로봇은 서버가 없는 동안엔 `Connect call failed` 재시도만 반복했는데, 이건 "서버가 꺼져있어 당연히 안 되는" 상황이라 이번에 조사하는 "서버가 떠 있는데도 흔들리는" 문제와는 별개다.

**현재 상태**: 3Hz로 낮추고 30분 관찰한 결과 흔들림이 재현되지 않아 **잠정 해결로 본다**. 다만 관찰 시간이 30분으로 길지 않고, 과거 R-06에서도 "13분 만에 1회"처럼 뜸하게 발생한 전례가 있어(위 2번 항목) 완전히 해소됐다고 단정하기는 이르다 — 앞으로 실사용 중 다시 흔들리면 이 문서를 갱신하고, CPU보다 Tailscale/WiFi 연결 불안정이나 서버 쪽 웹소켓 핑퐁·타임아웃 설정 쪽을 봐야 한다.
