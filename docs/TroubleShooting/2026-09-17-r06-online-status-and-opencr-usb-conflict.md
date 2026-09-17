# 2026-09-17 트러블슈팅 — P5 로봇 온라인 상태 표시 버그 + R-06 배터리/오도메트리 유실(OpenCR USB 포트 충돌)

> 작성자: 최치언
> 대상 파일:
> `pc_careguard/dashboard/src/pages/p5_map_page.{h,cpp}`,
> `careguard_turtlebot_agent/systemd/careguard-bringup-r06.service`
> 대상 로봇: R-06(TurtleBot3, yeyu@100.110.210.121)

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | P5 맵 페이지 로봇 셀렉트 드롭다운에서, 시스템 설정의 "로봇 관리" 탭에는 온라인으로 뜨는 로봇이 P5에서는 오프라인으로 남아있음. 또한 실제로 온라인인 로봇을 클릭해서 선택하는 순간 즉시 "오프라인"으로 표시가 바뀜 | ① WS 재연결 시 로봇 목록/온라인 상태를 다시 조회하는 절차가 없어 재연결 중 놓친 `robot_status` push를 못 따라잡음. ② `switchToRobot()`이 로봇 전환 직후 안전 기본값으로 `robotOnline_=false`를 강제하는데, 이후 실제 상태를 다시 확인하는 절차가 없어 실제로는 온라인이어도 다음 상태 전환 이벤트가 오기 전까지 계속 오프라인으로 표시됨 | 가벼운 재조회 경로 `refreshRobotOnlineStatus()`를 추가해 WS 재연결 시·로봇 전환 직후 둘 다 `GET /robots`로 실제 온라인 상태를 즉시 다시 조회해 반영 |
| 2 | P1 통합관제에서 LIMO 로봇들은 배터리 값이 정상인데 R-06(터틀봇)만 계속 배터리 값을 못 가져옴. 같은 시점에 pose(위치)도 안 잡힘 | 로봇 PC(yeyu@100.110.210.121)에 팀원 개인 센서 프로젝트용 아두이노 Uno가 별도로 연결돼 있었는데, 이 아두이노가 OpenCR보다 먼저 USB로 인식되면서 `/dev/ttyACM0`을 선점함. `turtlebot3_ros`는 `usb_port` 기본값(`/dev/ttyACM0`)으로 고정 연결을 시도하는 구조라, 포트 자체는 정상적으로 열리지만 그 뒤에 있는 게 OpenCR이 아니라 아두이노라 Dynamixel 프로토콜 핸드셰이크가 매번 실패("Failed connection with Devices")하고, 그 실패 처리 중 `rclcpp::shutdown()`이 호출된 뒤에도 코드가 계속 진행돼 배터리 등 센서 퍼블리셔를 만들다 죽음. 그 결과 `/battery_state`·`/odom`·`/joint_states` 전부 발행자가 0인 상태였음 | `careguard-bringup-r06.service`의 `usb_port`를 `/dev/ttyACM0` 대신 `/dev/serial/by-id/usb-ROBOTIS_OpenCR_Virtual_ComPort_in_FS_Mode_FFFFFFFEFFFF-if00`(장치 고유 시리얼 기준 고정 경로)로 지정 — 이제 USB 인식 순서와 무관하게 항상 OpenCR만 가리킴 |

---

## 1. P5 로봇 셀렉트 드롭다운 온라인/오프라인 표시 불일치

### 현상

P7 "시스템 설정 > 로봇 관리" 탭에는 R-06이 온라인으로 표시되는데, 같은 순간 P5 맵 페이지의 로봇 셀렉트 드롭다운에는 R-06이 오프라인으로 남아있었다. 또한 P5에서 (콤보 목록엔 온라인으로 보이는) 다른 로봇을 클릭해서 선택하면, 선택하는 그 순간 바로 "오프라인"으로 텍스트가 바뀌는 현상도 있었다.

### 원인

두 가지가 겹쳐 있었다.

**① WS 재연결 시 따라잡기 절차 부재.** `P7SettingsPage::setWsClient`는 WS가 재연결될 때마다 `fetchRobots()`로 로봇 목록을 다시 받아, 재연결 중 놓쳤을 수 있는 `robot_status` push를 따라잡는다. 반면 `P5MapPage::setWsClient`(`p5_map_page.cpp:86`)는 재연결 시 `connectionBanner_` 표시만 갱신할 뿐, 로봇 온라인 상태를 다시 조회하는 절차가 아예 없었다. 실시간 push 메시지를 하나라도 놓치면(대시보드 PC 쪽 네트워크가 잠깐 끊겼다 재연결됐거나, 그 순간 페이지가 아직 WS에 안 붙어있었던 경우 등) 따라잡을 방법이 없어, 실제로는 온라인인 로봇이 셀렉트 창엔 옛 상태 그대로 남았다.

**② 로봇 전환 시 강제 오프라인 후 재확인 누락.** `switchToRobot()`(`p5_map_page.cpp:506`)은 로봇을 바꿀 때마다 실제 상태를 확인하기 전에 안전을 위해 `robotOnline_ = false`로 강제하고, 바로 `updateRobotChipText()`로 콤보 항목 텍스트까지 그 값으로 덮어쓴다. 문제는 이후 실제 상태를 다시 확인하는 절차가 없었다는 것 — 이미 온라인인 로봇으로 전환한 경우 서버 입장에서는 "연결 상태 전환"이 없었으므로 새 `robot_status` push도 안 오고, 결국 다음 실제 연결/해제 전환이 발생하기 전까지 화면엔 계속 오프라인으로 남았다.

### 해결

`P5MapPage`에 가벼운 재조회 경로 `refreshRobotOnlineStatus()`를 추가했다 — `loadRobotsFromServer()`처럼 `rebuildRobotMenu()`·`switchToRobot()`을 다시 타지 않고(그러면 `currentSlamJobId_`·`registeredMaps_` 등 진행 중이던 화면 상태가 리셋될 위험이 있다), `GET /robots` 응답으로 `robots_[i].isOnline`과 콤보 항목 텍스트만 갱신한다.

```cpp
void P5MapPage::refreshRobotOnlineStatus() {
    apiClient_->fetchRobots([this](bool ok, const QVector<ApiClient::RobotInfo>& robots) {
        if (!ok) return;
        for (const ApiClient::RobotInfo& r : robots) {
            for (int i = 0; i < robots_.size(); ++i) {
                if (robots_[i].code != r.code) continue;
                robots_[i].isOnline = r.isOnline;
                robotCombo_->setItemText(i, formatRobotChipText(robots_[i], r.isOnline));
                if (r.code == currentRobotCode_) {
                    robotOnline_ = r.isOnline;
                    robotOfflineBanner_->setVisible(!robotOnline_);
                }
                break;
            }
        }
    });
}
```

이 경로를 두 지점에서 호출하도록 연결했다:

- `setWsClient`의 `connectionStateChanged` 핸들러 — 재연결된 순간 호출(P7SettingsPage와 동일한 이유).
- `switchToRobot()` 마지막 — 안전 기본값(오프라인)으로 강제해둔 직후 즉시 실제 상태로 재확인.

---

## 2. R-06 배터리·오도메트리·pose 유실 — OpenCR USB 포트 충돌

### 현상

P1 통합관제에서 LIMO 로봇들은 배터리 %가 정상적으로 올라오는데, R-06(터틀봇)만 배터리 값을 못 가져왔다. 같은 로봇의 pose(위치)도 지도 위에 안 잡혔다.

### 조사 과정

로봇 PC(`yeyu@100.110.210.121`)에 SSH로 접속해 `careguard-bringup-r06.service`(TurtleBot3 bringup) 로그를 확인했다. 서비스 자체는 "active running"이었지만, 실제로 OpenCR 보드와 통신하며 배터리·오도메트리·조인트 상태를 발행하는 `turtlebot3_ros` 서브프로세스가 반복적으로 죽어 있었다.

```
[turtlebot3_ros-3] [INFO] [DynamixelSDKWrapper]: Succeeded to open the port(/dev/ttyACM0)!
[turtlebot3_ros-3] [INFO] [DynamixelSDKWrapper]: Succeeded to change the baudrate!
[turtlebot3_ros-3] [ERROR] [turtlebot3_node]: Failed connection with Devices
[turtlebot3_ros-3] [INFO] [turtlebot3_node]: Add Motors
[turtlebot3_ros-3] [INFO] [turtlebot3_node]: Add Wheels
[turtlebot3_ros-3] [INFO] [turtlebot3_node]: Add Sensors
[turtlebot3_ros-3] terminate called after throwing an instance of 'rclcpp::exceptions::RCLError'
[turtlebot3_ros-3]   what():  could not create publisher: rcl node's context is invalid, at ./src/rcl/node.c:428
[ERROR] [turtlebot3_ros-3]: process has died [pid ..., exit code -6, ...]
```

`turtlebot3_node/src/turtlebot3.cpp` 소스를 확인해 정확한 크래시 흐름을 특정했다:

1. `check_device_status()`가 가장 먼저 실행되어 OpenCR에 Dynamixel 프로토콜로 ping을 보낸다.
2. `dxl_sdk_wrapper_->is_connected_to_device()`가 `false`를 반환하면 `"Failed connection with Devices"`를 로그로 남기고 `rclcpp::shutdown()`을 호출한다.
3. 그런데 이 실패를 호출부가 체크하지 않고 그대로 `add_motors()` → `add_wheels()` → `add_sensors()`를 계속 진행한다. `add_sensors()`가 `sensors::BatteryState` 퍼블리셔를 만들려는 순간, 이미 shutdown된 컨텍스트라 `RCLError`로 크래시한다.

서비스 재시작, OpenCR 리셋 버튼, 라즈베리파이 전체 재부팅(오늘 10:14 실제로 재부팅됨)까지 시도했지만 전부 **완전히 동일한 지점에서 동일한 에러**로 재현됐다 — 즉 일시적 상태 문제가 아니라 구조적인 문제라는 뜻이었다.

`/dev/ttyACM0`이 열리는 것 자체는 매번 성공한다는 점에 착안해 `udevadm info`로 실제 어떤 장치가 어떤 노드에 연결돼 있는지 확인했다:

```
$ udevadm info -q property -n /dev/ttyACM0
ID_VENDOR_ID=2341
ID_MODEL_ID=0043
ID_VENDOR_FROM_DATABASE=Arduino SA
ID_MODEL_FROM_DATABASE=Uno R3 (CDC ACM)

$ udevadm info -q property -n /dev/ttyACM1
ID_VENDOR_ID=0483
ID_MODEL_ID=5740
ID_VENDOR=ROBOTIS
ID_MODEL=OpenCR_Virtual_ComPort_in_FS_Mode
```

**`/dev/ttyACM0`은 OpenCR이 아니라 아두이노 Uno였다.** 이 로봇 PC에는 팀원(yeyu) 개인 센서 프로젝트용(`arduino_dht11_bridge`, `tb3_arduino_sensor_bridge`, `yeyu_sensor_bridge_project` 등 워크스페이스가 홈 디렉터리에 같이 있었다)으로 연결해둔 아두이노 Uno가 있었는데, 어제 이 아두이노가 먼저 USB로 인식되면서 `/dev/ttyACM0`을 차지했고, OpenCR은 그 뒤로 밀려나 `/dev/ttyACM1`로 잡혀 있었다. `turtlebot3_bringup`의 `robot.launch.py`(`launch/robot.launch.py:40`)는 `usb_port` 기본값이 `/dev/ttyACM0`으로 고정돼 있어서, `turtlebot3_ros`는 시리얼 포트를 여는 것까지는(아두이노도 진짜 시리얼 장치이므로) 매번 성공하지만, 그 뒤에 있는 게 Dynamixel 프로토콜을 모르는 아두이노라 핸드셰이크가 항상 실패했던 것이다.

### 해결

아두이노를 뽑고 OpenCR의 USB 케이블을 재연결해 `/dev/ttyACM0`을 다시 차지하게 한 뒤 정상 동작을 1차 확인했다. 다만 이 상태는 다음에 또 다른 USB 장치가 먼저 꽂히면 언제든 재발할 수 있는 구조적 문제라, `usb_port`를 장치 고유 시리얼 기준의 안정적인 경로로 고정했다.

`careguard_turtlebot_agent/systemd/careguard-bringup-r06.service`:

```diff
-ExecStart=/bin/bash -lc 'source /opt/ros/humble/setup.bash && source ~/turtlebot3_ws/install/setup.bash && exec ros2 launch turtlebot3_bringup robot.launch.py'
+ExecStart=/bin/bash -lc 'source /opt/ros/humble/setup.bash && source ~/turtlebot3_ws/install/setup.bash && exec ros2 launch turtlebot3_bringup robot.launch.py usb_port:=/dev/serial/by-id/usb-ROBOTIS_OpenCR_Virtual_ComPort_in_FS_Mode_FFFFFFFEFFFF-if00'
```

`/dev/serial/by-id/usb-ROBOTIS_OpenCR_...`는 udev가 장치의 벤더/모델/시리얼 넘버를 기준으로 만들어주는 심볼릭 링크라, USB 인식 순서와 무관하게 항상 이 특정 OpenCR 보드만 가리킨다. 수정 후 로봇에 배포·재시작해 `turtlebot3_node`가 "Succeeded to create battery state publisher"까지 정상적으로 통과하고 `/battery_state`·`/odom`이 정상 값을 내는 것을 확인했다.

### 남은 과제 / 참고

- R-05도 같은 로봇 PC 구조(터틀봇 + OpenCR)를 쓴다면 같은 종류의 USB 순서 충돌에 노출돼 있을 수 있다 — 아직 재현되지 않아 이번엔 손대지 않았지만, 같은 증상이 보이면 같은 방식(`usb_port`를 by-id 경로로 고정)으로 고치면 된다.
- 근본적으로는 로봇 전용 PC에 팀원 개인 프로젝트(아두이노 센서 브리지 등)를 같이 연결해 쓰는 것 자체가 이런 충돌의 원인이므로, 가능하면 분리하는 것이 좋다.
