# 2026-09-17 트러블슈팅 — 복약 배출 hw_error/jammed 근본 원인 해결 (약통 시리얼 포트 오설정 + ack 버퍼링)

> 작성자: 최치언
> 대상 파일:
> `robot_careguard_ws/src/careguard_agent/careguard_agent/downlink_node.py`,
> `robot_careguard_ws/src/careguard_hw/careguard_hw/pillbox_controller.py`,
> `robot_careguard_ws/src/careguard_mission/careguard_mission/patrol_task.py`
> 대상 로봇: R-07 (LIMO, wego@192.168.230.42)
> 관련 문서: [2026-09-11-medication-dispense-integration.md](./2026-09-11-medication-dispense-integration.md) (`ACK_TIMEOUT_SEC` 3→6초로 올린 최초 배경 — 이번 사고와는 다른 원인)

## 요약

2026-09-16 밤 실기 테스트(run 81~88, 약 2시간)에서 복약 배출을 시도할 때마다 `jammed`(3회) 또는 `hw_error`(2회)만 나오고 단 한 번도 `delivered`가 안 나던 문제. 하루 넘게 재현되던 이 문제를 다음날(9/17) 세 커밋으로 근본 해결했다.

| # | 증상/원인 | 해결 |
|---|---|---|
| 1 | `PILLBOX_SERIAL_PORT` 기본값이 `/dev/ttyUSB0`(약통 아두이노가 아닌 별개의 CP210x 장치)로 잘못 설정돼 있어, 로봇이 명령을 완전히 엉뚱한 장치로 계속 보내고 있었음(응답이 올 리가 없으니 매번 ack 타임아웃 → `hw_error`) | `/dev/serial/by-id/`로 실제 정체 확인 후 기본값을 진짜 약통 아두이노 포트인 `/dev/ttyACM0`으로 수정 |
| 2 | 아두이노가 100ms마다 IR 디버그 줄을 계속 쏟아내는데, `readline()`(사실상 1바이트씩 폴링)이 이 트래픽에 밀려 실제로 도착한 `"OK A/B"` ack를 `ACK_TIMEOUT_SEC` 안에 못 읽는 경우가 있었음 | `in_waiting`만큼 한 번에 읽어 직접 버퍼링 후 줄 단위로 자르는 방식으로 재작성. ascii→utf-8 디코딩, 연결 직후 `reset_input_buffer()`, `[serial]` 태그 로그 추가 |
| 3 (부수 발견) | 배출 시도가 hw_error처럼 6~12초 만에 실패로 끝나면, 대상자가 반응할 시간도 없이 로봇이 바로 다음 목적지로 출발함 | 결과와 무관하게 `MIN_MEDICATION_STOP_SEC(20초)`이 지날 때까지 그 자리에 강제로 머묾(취소 요청 시 즉시 해제) |

---

## 1. 증상 — 9/16 밤 실기 테스트에서 배출 100% 실패

9/16 밤, 복약 배출 패트롤 변형을 반복 실행하는 실기 테스트 세션(run 81~88, 약 2시간)에서 인식→TTS→서보 열림까지는 매번 정상 진행됐는데, 그 이후가 한 번도 성공하지 못했다:

- `jammed` 3회 — IR 회수 감지가 5초 창 안에 한 번도 안 걸림
- `hw_error` 2회

## 2. 조사 과정 — 포트 정체 재확인

`hw_error`는 코드상 세 경로(①`pillbox_controller` 서비스 미기동 ②시리얼 쓰기 예외 ③ack `"OK A/B"`가 `ACK_TIMEOUT_SEC` 안에 안 옴)로 발생할 수 있는데, 9/16 밤엔 어느 경로인지 특정하지 못한 채 하루를 넘겼다.

9/17, `/dev/serial/by-id/`로 R-07에 연결된 시리얼 장치를 다시 확인했다. **직전(9/16)에 "이 보드는 CP210x라 `/dev/ttyUSB0`으로 잡힌다"고 결론 내렸던 것 자체가 틀렸다** — `lsusb`/`by-id` 심볼릭 링크에 `"Arduino (www.arduino.cc)"`로 정확히 찍히는 진짜 약통 아두이노는 `/dev/ttyACM0`이었고(일반적인 Arduino Uno/Nano 호환보드의 16U2/32U4 USB 컨트롤러는 `cdc_acm` 드라이버를 쓰므로 `ttyACM*`가 정상), `/dev/ttyUSB0`은 R-07에 물려있는 **별개의 CP210x 장치**(용도 미확인)였다. 즉 최초 추정(`ttyACM0`)이 맞았는데 9/16에 잘못 정정해서 하루 넘게 엉뚱한 포트로 명령을 보내고 있었던 것이다.

## 3. 원인

### 3-1. 잘못된 시리얼 포트 (근본 원인)

`downlink_node.py`의 `PILLBOX_SERIAL_PORT` 기본값이 `/dev/ttyUSB0`으로 설정돼 있어, `pillbox_controller.py`가 매번 그 CP210x 장치에 `"OPEN A/B"`를 보내고 있었다. 그 장치는 약통 아두이노가 아니므로 응답(`"OK A/B"`)이 올 수가 없고, 매번 `ACK_TIMEOUT_SEC` 뒤 `hw_error`로 실패한다 — 서보가 실제로 열렸는지 여부와 무관하게 **애초에 명령 자체가 안 갔던 것**.

```diff
-# R-07의 이 보드는 CP210x USB-시리얼 브리지라 /dev/ttyACM0이 아니라 /dev/ttyUSB0으로
-# 잡힌다(lsusb로 "Silicon Labs CP210x UART Bridge" 확인, 2026-09-16)
-PILLBOX_SERIAL_PORT = os.environ.get('CAREGUARD_PILLBOX_SERIAL_PORT', '/dev/ttyUSB0')
+# 2026-09-16에 한 번 /dev/ttyUSB0으로 고쳤었는데 이게 틀렸다 — 그 포트는 R-07에
+# 물려있는 "CP210x USB-시리얼 브리지"라는 별개 장치였고, 약통 아두이노가 아니었다.
+# 약통 아두이노는 USB 디스크립터에 "Arduino(www.arduino.cc)"라고 정확히 찍혀 있고
+# /dev/ttyACM0으로 잡힌다 — 애초에 처음 추정이 맞았었다. /dev/ttyUSB0은 절대 이거로
+# 되돌리지 말 것.
+PILLBOX_SERIAL_PORT = os.environ.get('CAREGUARD_PILLBOX_SERIAL_PORT', '/dev/ttyACM0')
```

`jammed` 3건도 상당 부분 이 문제의 파생일 가능성이 크다 — 서보가 안 열렸으니 IR이 알약 낙하를 감지할 수도 없었을 것.

### 3-2. ack 읽기 로직이 IR 디버그 트래픽에 밀림

포트를 바로잡아도 잠재적으로 남아있던 문제. 기존 `SerialPillboxBackend._read_loop`는 `pyserial`의 `readline()`(내부적으로 1바이트씩 읽어 `\n`을 찾는 방식)에 의존했는데, 아두이노가 100ms 주기로 IR 상태 디버그 줄을 계속 쏟아내는 스트림에서는 리더가 이 트래픽에 밀려 실제로 도착한 `"OK A/B"` ack를 `ACK_TIMEOUT_SEC`(6초, [2026-09-11 문서](./2026-09-11-medication-dispense-integration.md) 참고) 안에 못 읽는 경우가 있었다. ascii 전용 디코딩도 한글이 섞인 디버그 줄(`"IR-A(9번): ..."`)에서 깨질 여지가 있었다.

## 4. 해결

### 4-1. 포트 수정 — 위 3-1 diff 그대로 적용

### 4-2. ack 읽기를 버퍼 기반으로 재작성

`_read_loop`가 `in_waiting`만큼 한 번에 읽어 직접 버퍼에 쌓다가 완전한 줄(`\n` 기준)이 모였을 때만 잘라내도록 바꿨다 — 줄 경계가 깨질 여지를 없앤다.

```python
def _read_loop(self):
    while self._running:
        try:
            n = self._serial.in_waiting or 1
            chunk = self._serial.read(n)
        except Exception as exc:
            self._log(f'읽기 오류로 리더 스레드 종료: {exc!r}')
            break
        if not chunk:
            continue
        self._buf += chunk
        while b'\n' in self._buf:
            raw_line, self._buf = self._buf.split(b'\n', 1)
            line = raw_line.decode('utf-8', errors='ignore').strip()
            if not line:
                continue
            if line in ('OK A', 'OK B'):
                self._log(f'ack 수신: "{line}"')
                self._ack_events[line[-1]].set()
                continue
            if self._on_line:
                self._on_line(line)
```

곁들여 수정한 것들:
- `readline()`의 내부 타임아웃(1.0초) 대신 `read()` 타임아웃을 0.2초로 낮춰 반응성 개선.
- 연결 직후 `reset_input_buffer()` 추가 — 아두이노 리셋 직후 부팅 배너 등 쌓여있던 잔여 바이트를 비움.
- ascii → utf-8 디코딩으로 변경.
- 명령 전송/ack 수신/타임아웃마다 `[serial]` 태그로 로그를 남겨, 이후 재현 시 `pillbox_controller` 서비스 미기동/쓰기 예외/ack 타임아웃 세 경로 중 정확히 어디서 실패했는지 로그만으로 바로 알 수 있게 함(타임아웃 로그에는 그 시점 버퍼 잔여 데이터도 같이 남김).

### 4-3. (부수 발견) 배출 실패 시 최소 정지 시간 보장

같은 실기 테스트에서 별도로 확인된 문제: 배출 시도가 `hw_error`처럼 6~12초 만에 끝나버리면, 대상자가 반응할 시간도 없이 로봇이 바로 다음 목적지로 출발했다. `patrol_task.py`에 `MIN_MEDICATION_STOP_SEC=20.0`(정상 흐름이었다면 어차피 기다렸을 `RETRIEVAL_TIMEOUT_SEC`과 동일한 값)을 추가해, 결과(`delivered`/`timeout`/`jammed`/`hw_error`)와 무관하게 이 시간이 지날 때까지 그 자리에 머물게 했다. 취소 요청이 오면 이 강제 대기도 즉시 풀린다.

```python
if status != 'cancelled':
    remaining = MIN_MEDICATION_STOP_SEC - (time.monotonic() - started_at)
    while remaining > 0 and not goal_handle.is_cancel_requested:
        step = min(0.5, remaining)
        time.sleep(step)
        remaining -= step
```

## 5. 남은 과제 / 참고

- `Slot 1`과 `Slot 3`이 같은 물리 IR/서보 채널(`'A'`)을 공유한다(`_SLOT_TO_CHANNEL = {1:'A', 2:'B', 3:'A'}`, `pillbox_controller.py`/`downlink_node.py` 양쪽에 독립적으로 중복 정의돼 있어 공유 상수가 없음) — 배선을 바꾸면 두 파일 다 고쳐야 하고, 안 그러면 슬롯→물리 매핑이 조용히 깨진다. 아직 손 안 댐.
- `pillbox_controller.py`는 시리얼 연결 실패 시 항상 ack에 성공하는 `FakePillboxBackend`로 조용히 대체된다 — 앞으로 비슷한 증상(배출은 되는데 결과가 이상함)이 재현되면, "시리얼 백엔드 연결: ..." 로그로 진짜 백엔드가 붙었는지부터 확인할 것.
- 이번 수정 이후의 실기 재검증(연속 배출 성공 여부)은 아직 별도로 기록되지 않음 — 다음 실기 테스트에서 `delivered`가 정상적으로 나오는지 확인 필요.
