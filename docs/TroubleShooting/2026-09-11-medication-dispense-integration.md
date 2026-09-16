# 2026-09-11 트러블슈팅 — 복약 배출(순찰↔로봇↔펌웨어) 통합

> `feature/pillbox-slot-rotation` 브랜치에서 "P6 복약 전달 경로가 실제로 로봇에
> 배출 명령을 내리는" 다리(서버→로봇→ROS 액션→시리얼→펌웨어)를 새로 놓고 실
> 하드웨어까지 검증하는 과정에서 겪은 문제들의 증상·원인·해결을 정리한 기록.
> 대상 파일: `pc_careguard/server/app/ws/robot_ws.py`,
> `robot_careguard_ws/src/careguard_mission/careguard_mission/patrol_task.py`,
> `robot_careguard_ws/src/careguard_mission/careguard_mission/medication_task.py`,
> `robot_careguard_ws/src/careguard_hw/careguard_hw/pillbox_controller.py`,
> `robot_careguard_ws/src/careguard_hw/firmware/pillbox_firmware/pillbox_firmware.ino`,
> `deploy/sync_to_limo.sh`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | `medication_done` WS 메시지를 보내도 `medication_log`에 아무것도 안 남음 | `uvicorn` 서버 프로세스가 코드 수정 "전"에 이미 떠 있었음 — Python은 핫리로드 안 됨 | 대시보드(서버를 자동 기동하는 앱) 재시작 |
| 2 | `person_detected`를 아무리 pub해도 액션이 계속 자기 타임아웃으로만 끝남 | `rclpy.spin(node)`(SingleThreadedExecutor)에서, ActionServer 콜백이 콜백 그룹을 점유한 채 블로킹하는 동안 같은 노드의 구독 콜백이 전혀 실행 안 됨 | `main()`에서 `MultiThreadedExecutor`로 교체 |
| 3 | 로봇 PC에서 `pillbox_controller` 실행 시 `ModuleNotFoundError: No module named 'careguard_msgs.srv'` | `.srv` 파일을 `CMakeLists.txt`에 추가한 뒤 증분 빌드만 돌려서 rosidl 코드 생성이 실제로 재실행되지 않음 | `build/install/log`의 `careguard_msgs`만 지우고 클린 빌드 |
| 4 | 클린 빌드로 고쳤는데 똑같은 에러가 다시 남 | 팀원이 자기 변경사항을 같은 로봇으로 `rsync --delete` 동기화하고 있어서, 서로 다른(패치 미적용) 워크스페이스가 번갈아 덮어씀 | 동기화 타이밍을 조율하고 재시도(근본 해결은 보류) |
| 5 | 시리얼 연결 자체가 실패(`pyserial이 설치돼 있지 않습니다`) | 로봇 PC에 `python3-serial` 시스템 패키지 미설치 (`package.xml`엔 명시돼 있었지만 `rosdep install` 안 함) | `sudo apt install python3-serial` |
| 6 | 시리얼 포트 열기 실패(`[Errno 13] Permission denied: '/dev/ttyACM0'`) | 로봇 PC 사용자 계정이 `dialout` 그룹에 없음 | `sudo usermod -aG dialout $USER`(재로그인) 또는 임시로 `chmod 666` |
| 7 | 서보가 실제로 열렸는데도 액션 결과가 `hw_error`로 끝남 | 펌웨어 `moveServoTo()`가 0→150도를 1도당 30ms로 움직여 최악 ≈4.5초 걸리는데, ack 대기시간을 3초로 잡아둬서 서보가 다 움직이기 전에 타임아웃 | `pillbox_controller.py`의 `ACK_TIMEOUT_SEC`(3→6초), `medication_task.py`의 `HW_SERVICE_TIMEOUT_SEC`(5→8초)로 상향 |
| 8 | (보류) 위 타이밍을 고친 뒤에도 `medication_task`↔실 `pillbox_controller` 조합에서 이유 없이(로그도 없이) `hw_error` | 미확정 — 고아 프로세스 중복 실행 의심(같은 액션/서비스 이름으로 여러 노드가 동시에 뜨면 어느 쪽이 응답할지 꼬일 수 있음), 다음 세션에서 `ps aux`로 중복 프로세스부터 확인 예정 | 미해결 |

---

## 1. medication_done을 보내도 DB에 안 남음

**증상**

가짜 로봇 WS 클라이언트로 `{"type": "medication_done", ...}`을 실제 서버(`ws://127.0.0.1:8000/ws/robot`)에 보냈는데, `medication_log` 테이블에 새 행이 전혀 안 생겼다.

**진단 과정**

`_handle_medication_done()` 함수를 스크립트에서 직접 호출해보니 정상적으로 DB에 삽입됐다 — 즉 함수 로직 자체는 문제가 없었다. `ps -p <서버 PID> -o lstart`로 서버 프로세스 시작 시각을 확인하고 `robot_ws.py`의 파일 수정 시각(`stat`)과 비교하니, 서버가 뜬 시각(14:47:21)이 파일을 고친 시각(15:49:13)보다 **한 시간 이상 앞섰다**.

**원인**

이 서버(`uvicorn app.main:app`)는 대시보드 앱(`careguard_dashboard`)이 자동으로 띄운 자식 프로세스다. `--reload` 옵션 없이 떠 있어서 코드를 아무리 고쳐도 그 프로세스는 계속 옛날 코드로 동작한다 — Python은 소스 파일을 고쳤다고 실행 중인 프로세스가 자동으로 다시 읽지 않는다.

**해결**

대시보드 앱을 재시작해 서버 프로세스를 새로 띄웠다. 재시작 후 같은 테스트를 다시 돌리니 `medication_log`에 정상적으로 행이 생기고, `/ws/dashboard`로도 `medication_done` broadcast가 정상 수신됐다.

**교훈**

서버/로봇 코드를 대시보드가 자동 기동하는 구조에서는, 코드를 고친 뒤 "지금 떠 있는 프로세스가 그 코드를 실제로 로드한 시점 이후에 떴는지"를 먼저 확인해야 한다 — 그렇지 않으면 "코드는 맞는데 왜 안 되지"를 반복하게 된다.

---

## 2. person_detected를 pub해도 액션이 못 받음 (rclpy 콜백 그룹 데드락)

**증상**

`patrol_task.py`/`medication_task.py`가 `/careguard/perception/person_detected`를 구독해 `threading.Event`로 대기하는 구조인데, `ros2 topic pub`으로 아무리 반복 발행해도 액션이 늘 자기 `person_timeout_sec`이 다 지나서야 `timeout`으로 끝났다.

**진단 과정**

`ros2 topic echo`로 별도 구독을 걸어보니 메시지 자체는 정상적으로 발행·수신되고 있었다 — 즉 토픽 계층은 멀쩡했다. `example_interfaces/action/Fibonacci`로 최소 재현을 만들어(액션 실행 중 `threading.Event.wait()`로 블로킹 + 별도 구독 콜백이 그 이벤트를 set) `rclpy.spin(node)`(기본 `SingleThreadedExecutor`)로 돌려보니 증상이 그대로 재현됐다 — 로그를 보면 구독 콜백(`on_msg`)이 액션의 `wait()`가 **자기 타임아웃으로 끝난 직후**에야 한꺼번에(큐에 쌓여있던 것들이) 처리됐다.

**원인**

ActionServer와 구독이 같은(기본) 콜백 그룹에 묶여 있고, `SingleThreadedExecutor`는 그 콜백 그룹에서 하나가 실행 중이면 같은 그룹의 다른 콜백을 절대 동시에 못 돌린다 — 액션 실행 스레드가 블로킹돼 있는 내내 구독 콜백은 큐에만 쌓이고 실행되지 못했다.

**해결**

같은 최소 재현에서 `rclpy.executors.MultiThreadedExecutor()`로 바꿔보니 즉시 해결됐다(발행 2.5초 만에 반응). `patrol_task.py`·`medication_task.py`의 `main()`을 둘 다 `executor.add_node(node); executor.spin()`(MultiThreadedExecutor) 패턴으로 교체했다.

**교훈**

액션 서버 안에서 블로킹 대기를 하면서 같은 노드의 구독 콜백이 그 대기를 풀어주길 기대하는 구조는, 기본 `rclpy.spin(node)`(SingleThreadedExecutor)로는 구조적으로 절대 동작하지 않는다 — 코드 리뷰만으로는 안 드러나고 실제로 goal을 보내고 관찰해야만 발견되는 종류의 버그였다. 이런 패턴을 쓸 땐 처음부터 `MultiThreadedExecutor`를 기본값으로 삼는 게 안전하다.

---

## 3. PillboxSlot.srv를 추가했는데 ModuleNotFoundError

**증상**

`careguard_msgs`에 새 `.srv` 파일을 추가하고 `CMakeLists.txt`에도 등록한 뒤 `colcon build`를 돌렸는데(`Finished`로 성공 표시), `pillbox_controller` 실행 시 `ModuleNotFoundError: No module named 'careguard_msgs.srv'`가 났다.

**진단 과정**

`.srv` 파일 존재 여부, `CMakeLists.txt` 등록 여부를 각각 확인했더니 둘 다 정상이었다. 설치된 파이썬 패키지 디렉터리(`install/careguard_msgs/local/lib/python3.10/dist-packages/careguard_msgs/`)를 직접 열어보니 `action`·`msg` 폴더는 있는데 **`srv` 폴더 자체가 없었다** — 즉 rosidl 코드 생성이 그 파일에 대해서는 한 번도 실행되지 않은 것.

**원인**

이미 한 번 빌드된 상태에서 `.srv` 파일 하나를 추가한 뒤 증분(incremental) `colcon build`만 돌리면, 빌드 캐시가 새 인터페이스 파일을 감지 못하고 넘어가는 경우가 있다(빌드 자체는 "성공"으로 표시되므로 알아채기 어렵다).

**해결**

```bash
rm -rf build/careguard_msgs install/careguard_msgs log/careguard_msgs
colcon build --packages-select careguard_msgs
```
클린 빌드 후 `srv` 폴더가 정상적으로 생성됐고, import 에러도 사라졌다.

**교훈**

메시지/서비스/액션 정의 파일을 기존 패키지에 새로 추가했을 때 `colcon build`가 "성공"이라고 해도, 실제로 새 인터페이스가 코드 생성됐는지(설치 경로에 해당 서브폴더가 생겼는지)까지 확인해야 한다 — 증분 빌드를 못 믿을 이유가 하나 더 생겼다는 뜻이라, 인터페이스 파일을 추가할 땐 습관적으로 해당 패키지만 클린 빌드하는 게 안전하다.

---

## 4. 클린 빌드로 고쳤는데 로봇 PC에서 똑같은 에러가 또 남

**증상**

3번을 고치고 `deploy/sync_to_limo.sh --build`로 동기화+빌드까지 마쳤는데, 얼마 뒤 로봇 PC에서 `pillbox_controller`를 실행하니 정확히 같은 `ModuleNotFoundError`가 다시 났다.

**진단 과정**

`CMakeLists.txt`를 로봇에서 직접 `grep`해보니 `PillboxSlot.srv` 등록 줄이 있었다 — 즉 파일은 다시 살아있었다. 사용자가 "팀원이 자기 파일들로 계속 로봇에 전송하고 있다"고 확인해줬다.

**원인**

`deploy/sync_to_limo.sh`는 `rsync -avz --delete`를 쓴다 — 소스 쪽에 없는 파일은 원격에서도 지워진다는 뜻이다. 팀원이 이 패치가 반영 안 된 자신의 워크스페이스에서 같은 로봇으로 동기화를 돌리면, 그 시점에 이 세션에서 추가한 파일들(또는 `CMakeLists.txt`의 새 줄)이 통째로 사라지거나 되돌아간다 — 두 사람이 같은 배포 대상에 번갈아 `--delete` 동기화를 거는 셈이라 매번 상대방 작업이 지워졌다.

**해결**

근본 해결(동기화 락, 브랜치 병합 등)은 하지 않고, 이번엔 서로 겹치지 않는 타이밍에 다시 동기화+빌드하는 것으로 우회했다.

**교훈**

여러 사람이 `rsync --delete`로 같은 배포 대상(빌드 아티팩트가 아니라 소스 자체)에 동기화를 거는 구조는 원천적으로 경쟁 상태(race condition)를 만든다 — 이 프로젝트 규모에서는 당장 급한 문제는 아니지만, 로봇 쪽에도 git을 붙이거나(현재 CONVENTIONS.md는 의도적으로 안 붙이는 쪽을 택함) 동기화 전 담당자 간 확인 절차가 필요해 보인다.

---

## 5. pyserial 미설치

**증상**

클린 빌드·동기화가 다 끝난 뒤 `pillbox_controller`를 실행하니 `시리얼 연결 실패(pyserial이 설치돼 있지 않습니다 (python3-serial)) — Fake 백엔드로 대체합니다` 에러(노드 자체는 안 죽고 Fake로 폴백)가 났다.

**원인**

`careguard_hw/package.xml`에 `<depend>python3-serial</depend>`로 명시해뒀지만, 로봇 PC에서 `rosdep install`을 따로 돌리지 않아 실제로는 설치가 안 돼 있었다.

**해결**

```bash
sudo apt install python3-serial
```

**교훈**

새 패키지의 `package.xml`에 시스템 의존성을 적어두는 것만으로는 실제 설치를 보장하지 못한다 — `rosdep install --from-paths src -y` 같은 단계를 배포 스크립트에 포함하거나, 최소한 새 패키지를 처음 배포할 땐 수동으로라도 의존성 설치 여부를 확인해야 한다.

---

## 6. 시리얼 포트 Permission denied

**증상**

pyserial 설치 후에도 `[Errno 13] could not open port /dev/ttyACM0: Permission denied`로 여전히 Fake 백엔드로 폴백됐다.

**원인**

로봇 PC의 실행 계정이 `/dev/ttyACM0`을 소유한 `dialout` 그룹에 속해 있지 않았다 — Linux에서 USB 시리얼 장치는 기본적으로 `dialout` 그룹에만 읽기/쓰기 권한이 있다.

**해결**

```bash
sudo usermod -aG dialout $USER   # 재로그인 필요
# 또는 급하면 임시로:
sudo chmod 666 /dev/ttyACM0
```

**교훈**

USB 시리얼 장치를 처음 다루는 계정에서는 그룹 권한 문제가 거의 항상 먼저 걸린다 — 새 하드웨어 통합 체크리스트에 "실행 계정이 `dialout`에 속해 있는지" 항목을 넣어두면 다음엔 바로 확인 가능하다.

---

## 7. 서보가 실제로 열렸는데 hw_error로 끝남 (타이밍)

**증상**

`ros2 service call`로 직접 슬롯을 여는 테스트는 정상 동작(서보가 실제로 움직임)했는데, `medication_task`의 `DeliverMedication` 액션을 통해서 호출하면 서보가 눈으로 보기엔 정상적으로 열렸음에도 액션 결과가 `hw_error`로 끝났다.

**진단 과정**

펌웨어(`pillbox_firmware.ino`)의 `moveServoTo()`를 다시 읽어보니, 1도 이동마다 `delay(30)`을 걸고 있었다 — `ANGLE_CLOSED(0)`→`ANGLE_OPEN(150)` 전체 구간을 움직이면 최악의 경우 `150 × 30ms ≈ 4.5초`가 걸린다. 그런데 펌웨어는 `moveServoTo()`가 **끝난 뒤에야** `"OK A"` 응답을 보내고, `pillbox_controller.py`의 `ACK_TIMEOUT_SEC`는 3초로 잡혀 있었다 — 서보가 물리적으로 다 움직이기 전에 소프트웨어가 먼저 포기하고 실패로 처리한 것이었다.

**원인**

시리얼 ack 대기시간(3초)이 실제 하드웨어 동작이 걸릴 수 있는 최악의 시간(≈4.5초)보다 짧게 잡혀 있었다 — Fake 백엔드로만 테스트했을 때는 응답이 즉시 오므로 이 타이밍 문제가 전혀 드러나지 않았다.

**해결**

- `pillbox_controller.py`: `ACK_TIMEOUT_SEC` 3.0 → 6.0초
- `medication_task.py`: `HW_SERVICE_TIMEOUT_SEC`(이 ack를 감싸는 ROS 서비스 호출 자체의 대기시간)도 6초보다 커야 하므로 5.0 → 8.0초로 같이 상향(둘 사이엔 항상 `HW_SERVICE_TIMEOUT_SEC > ACK_TIMEOUT_SEC` 관계가 유지돼야 한다는 걸 주석으로 남김)

**교훈**

Fake/Mock 백엔드로 하는 통합 테스트는 "흐름이 맞는지"는 검증해주지만 "타이밍이 맞는지"는 절대 검증해주지 못한다 — 실제 지연이 있는 하드웨어(서보 이동, 모터, 네트워크 등)를 다루는 타임아웃 값은 반드시 실물로 최소 한 번은 최악의 경우(가장 먼 거리 이동 등)를 확인해야 한다.

---

## 8. (보류) 타이밍을 고친 뒤에도 이유 없이 hw_error

**증상**

7번을 고치고 재시작했는데도(소스 파일에 새 값이 반영된 것까지 확인함), `medication_task`를 통한 호출은 `opening` 단계에서 `hw_error`로 끝났다. 이번엔 3초도 안 걸려 실패해서(타임아웃이라 보기엔 너무 빠름), 순수 타이밍 문제는 아닌 것으로 보인다. `medication_task` 쪽 터미널엔 에러 로그가 전혀 안 찍혔다(예외가 났다면 `get_logger().error`로 로그가 남아야 정상).

**진단 상태**

로그가 없다는 건 `_on_done` 콜백 자체가 아예 호출 안 됐다(=서비스 응답이 타임아웃 전에도 후에도 안 옴)는 뜻으로 보이는데, 정확한 원인은 아직 못 짚었다. 가장 유력한 의심은 **같은 액션/서비스 이름으로 노드가 중복 실행 중일 가능성**(디버깅 과정에서 `ros2 run`을 여러 번 겹쳐 돌렸을 수 있음) — 이 경우 goal이 옛날(재시작 전) 프로세스로 갈 수도 있다.

**다음에 확인할 것**

```bash
ps aux | grep -E "medication_task|pillbox_controller" | grep -v grep
```
결과에 각각 정확히 하나씩만 떠 있는지 먼저 확인. 중복이 없는데도 재현되면, `pillbox_controller` 쪽 서비스 콜백(`_on_pillbox_slot`)에 임시로 진입 로그를 추가해 요청 자체가 도착하는지부터 다시 봐야 한다.
