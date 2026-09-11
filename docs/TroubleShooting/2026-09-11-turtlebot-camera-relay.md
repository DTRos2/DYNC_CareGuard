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
| 4 (참고) | `camera_ros` 노드가 가끔 "Device or resource busy"/"failed to acquire camera"로 첫 실행에 실패 | libcamera 파이프라인 핸들러가 이전 프로세스 종료 직후 아주 짧게 점유 상태로 남아있어 곧바로 재시작하면 경합이 생김(재현은 됐지만 근본 원인까지는 못 팜) | 재시도(재시작)하면 정상적으로 잡힘 — systemd `Restart=on-failure`로 이미 자동 재시도되므로 운영 중엔 문제 없음 |

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

## 4. (참고, 미해결) camera_ros가 가끔 카메라 획득에 실패함

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

### 현재 상태

재시도(프로세스를 다시 시작)하면 예외 없이 잡힌다. 운영 중에는 `Restart=on-failure`(`RestartSec=5`)로 이미 자동 재시도되므로 실사용에 지장은 없을 것으로 보이나, 재부팅 직후 등 타이밍이 안 좋으면 첫 기동에 한 번 실패할 수 있다는 점은 알아둬야 한다. 더 확실한 해결(예: `ExecStartPre`에 재시도 로직 추가, 또는 원인 자체를 libcamera 이슈 트래커에서 확인)은 이번 세션에서는 진행하지 않았다.
