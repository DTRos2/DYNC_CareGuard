# 2026-09-10 트러블슈팅 — "주행 취소" 기능, 코드 리뷰로 드러난 3가지 크래시·레이스 버그

> 작성자: 도희정
> P6 "주행 취소"를 실제로 동작하게 만드는 파이프라인(patrol_task.py의 cancel_callback, downlink_node.py의 goal 취소, 서버 cancel_patrol_run)을 막 완성한 직후, 코드 리뷰로 검증하다가 발견한 문제들. 전부 "기능은 새로 만들었지만 정작 그 안에서 동작이 깨져 있는" 패턴 — 순찰마다 크래시, 취소해도 안 지워지는 상태, 그리고 docstring이 약속한 동작과 실제 코드가 다른 문제까지 세 겹. 같은 코드 리뷰에서 함께 나온 로봇 상태 재동기화 버그는 [2026-09-10-robot-state-resync.md](./2026-09-10-robot-state-resync.md) 참고.
> 대상 파일: `robot_careguard_ws/src/careguard_mission/careguard_mission/patrol_task.py`, `robot_careguard_ws/src/careguard_nav/careguard_nav/waypoint_manager.py`, `robot_careguard_ws/src/careguard_agent/careguard_agent/downlink_node.py`, `pc_careguard/server/app/api/v1/patrol.py`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 순찰을 시작만 하면(취소 여부와 무관하게) 로봇 쪽이 매번 크래시 | `patrol_task.py`가 `cancel_check=goal_handle.is_cancel_requested`로 **프로퍼티 값**(bool)을 넘겼는데, `waypoint_manager.py::_go_through`는 이걸 `cancel_check()`로 **호출**함 → `'bool' object is not callable` | `lambda: goal_handle.is_cancel_requested`로 감싸서 넘김 |
| 2 | "주행 취소"를 눌러도 goal_handle 정리 로직이 사실상 항상 스킵됨(불필요한 GetResult 요청이 계속 나감) | `downlink_node.py`가 `_on_patrol_result`에서 매번 `goal_handle.get_result_async()`를 **다시 호출**해 `is` 비교 → `get_result_async()`는 호출할 때마다 새 Future 객체를 만들어 항상 False | goal 응답 시점(`_on_patrol_goal_response`)에 만든 Future를 `_patrol_result_future`에 저장해두고, 그 저장된 객체와만 비교 |
| 3 | "로봇 명령과 DB 정리는 서로 무관하게 둘 다 한다"는 docstring과 달리, 로봇이 응답 없으면 DB 정리(고아 `running` 기록 방지용)까지 같이 지연됨 | `cancel_patrol_run`이 `await robot_ws.send_command(...)`를 끝까지 기다린 뒤에야 DB 정리를 시작 — 순차 실행 | `asyncio.gather`로 두 작업을 병행 실행 |

---

## 1. `cancel_check` 프로퍼티 값 vs lambda — 순찰 시작하자마자 크래시

**증상**

"주행 취소" 파이프라인을 신설한 직후 코드 리뷰를 돌렸더니, 취소 여부와 무관하게 순찰을 시작하기만 하면 로봇 쪽에서 크래시가 나는 치명도 최상 이슈로 잡혔다.

**진단 과정**

1. `patrol_task.py`의 `navigate_poses`/`return_to_charging` 호출부에서 `cancel_check=goal_handle.is_cancel_requested`를 넘기고 있었다.
2. `waypoint_manager.py::_go_through`를 보니 `cancel_check()`로 **호출**하고 있었다 — 즉 넘겨받은 게 함수가 아니라 bool 값이면 그 자리에서 `TypeError: 'bool' object is not callable`.
3. `goal_handle.is_cancel_requested`는 rclpy의 **프로퍼티**(호출이 아니라 값 자체)라, 그대로 넘기면 그 시점의 bool 값이 그대로 담긴다 — "취소된 적 없음"에도 크래시가 나는 이유.

**원인**

취소 콜백 체계를 신설하며 "지금 이 시점에 취소됐는지"를 매 반복마다 다시 물어봐야 하는데(콜러블 필요), 호출 시점의 값을 한 번만 담아 넘기는 실수.

**해결**

`patrol_task.py`에서 `cancel_check=lambda: goal_handle.is_cancel_requested`로 수정. `_go_through`가 매 반복(0.1초 주기)마다 이 lambda를 호출해 그 시점 값을 새로 읽도록 계약을 명확히 했다.

**검증**

현재 코드(`patrol_task.py:131,179`)에 `cancel_check=lambda: goal_handle.is_cancel_requested`가 그대로 적용돼 있고, `waypoint_manager.py::_go_through`가 `cancel_check()`로 호출하는 계약과 정확히 맞아떨어짐을 확인 — **해결 완료**.

---

## 2. `get_result_async()` 재호출 — goal_handle 정리가 항상 스킵됨

**증상**

순찰이 끝나도(성공/실패/취소 무엇으로든) `_patrol_goal_handle`이 정리되지 않아, 다음 "주행 취소"가 이미 끝난 goal을 취소하려 들 수 있는 상태로 남아있었다. 불필요한 GetResult 요청도 계속 서버로 나갔다.

**진단 과정**

1. `downlink_node.py::_on_patrol_result`가 `goal_handle.get_result_async()`로 다시 만든 Future를 `is` 비교하고 있었다.
2. `get_result_async()`는 rclpy 액션 클라이언트 API상 **호출할 때마다 새 Future 객체를 생성**해 서버에 새 GetResult 요청을 보낸다 — 콜백 안에서 다시 호출해 비교하면 절대 같은 객체가 될 수 없다.

**원인**

goal 접수 시점(`_on_patrol_goal_response`)에 만든 `result_future`를 어디에도 저장해두지 않고, 나중에 "이 콜백이 지금 추적 중인 goal의 것인가"를 확인하려고 그 함수를 또 불러버린 설계 실수.

**해결**

`_on_patrol_goal_response`에서 `result_future`를 `self._patrol_result_future`에 저장. `_on_patrol_result(future)`는 `self._patrol_result_future is future`로만 비교 — 그 사이 새 goal이 시작돼 다른 handle로 덮어써진 경우까지 정확히 구분된다.

**검증**

현재 코드(`downlink_node.py:320-334`)에서 `result_future = goal_handle.get_result_async()` 직후 `self._patrol_result_future = result_future`로 저장하고, `_on_patrol_result`가 `self._patrol_result_future is future`로 비교하는 걸 확인 — **해결 완료**.

---

## 3. "취소는 무관하게 즉시 실행"이라는 docstring과 실제 순차 실행의 불일치

**증상**

`cancel_patrol_run`의 docstring은 "로봇 명령 전송과 DB 정리를 서로 무관하게 둘 다 한다"고 명시하고 있었지만, 실제로는 `await robot_ws.send_command(...)`가 끝난 뒤에야 `_clear_running_patrol_run`이 실행되는 순차 코드였다.

이 docstring 자체는 같은 날 앞서, `robot_id=4`의 `patrol_run`이 `status='running'`인 채 8건이나 멈춰 쌓여 있던 걸 실사용 중 발견하고 — 로봇이 오프라인이라 취소 명령이 응답 없이 걸려도 DB 정리만큼은 반드시 즉시 끝나야 한다는 설계 의도로 작성된 것. "왜 이렇게 짜야 하는가"는 이미 명확했는데, 실제 구현이 그 의도를 못 지킨 케이스였다.

**진단 과정**

1. 코드 리뷰가 docstring과 실제 코드의 불일치를 지적.
2. 설계 의도(로봇 응답 지연과 DB 정리는 서로 발목 잡으면 안 됨)를 재확인한 뒤 `asyncio.gather`로 병행 실행하도록 결론.

**원인**

`await A; await B` 순차 코드는 "둘 다 한다"는 보장은 맞지만 "무관하게 즉시"는 보장하지 못한다 — 로봇 쪽 명령이 타임아웃 없이 오래 걸리면 그만큼 고아 레코드 정리도 늦어져, 애초에 이 함수를 만든 이유(8건 stuck 방지)가 무력화될 수 있는 상태였다.

**해결**

```python
connected, _ = await asyncio.gather(
    robot_ws.send_command(payload.robot_code, {"type": "patrol_stop"}),
    _clear_running_patrol_run(payload.robot_code),
)
```
두 작업을 병행 실행 — 로봇이 오프라인이라 1번이 실패/지연돼도 2번(DB 정리)은 그와 무관하게 즉시 끝난다.

**검증**

현재 코드(`patrol.py::cancel_patrol_run`)에 위 `asyncio.gather` 패턴이 그대로 남아 있고, docstring도 "asyncio.gather로 둘을 동시에 실행해야 한다"는 이유를 명시한 채 일치함을 확인 — **해결 완료**.

---

**교훈**: 세 버그 다 "새 기능을 만들면서 세운 계약(콜러블이어야 한다, Future는 한 번만 만들어 재사용해야 한다, 두 작업은 병행이어야 한다)"이 실제 구현에서 지켜지지 않은 케이스다. 기능이 "일단 동작하는 것처럼 보인다"고 끝내지 않고, 완성 직후 바로 코드 리뷰를 돌려 의도-구현 불일치를 잡은 게 유효했다.
