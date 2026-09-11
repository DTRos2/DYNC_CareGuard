# 2026-09-10 트러블슈팅 — WS 메시지 하나 놓치면 "새로고침"으로도 안 풀리던 주행 버튼

> 작성자: 도희정
> P6에 로봇 온라인/주행중/SLAM중/Nav2실행 4개 상태를 배너·버튼에 반영하는 기능(`refreshRobotState`)을 막 만든 뒤, 코드 리뷰로 그 코드를 검토하다 발견. robotOnline_/isPatrolling_/slamActive_ 셋은 WS 이벤트 유실에 대비해 HTTP 재조회(GET) 경로를 같이 뒀는데, navRunning_ 하나만 WS `nav_status` 메시지로만 갱신되고 재조회 경로가 없었다 — 그 메시지 하나만 유실/지연되면 로봇·Nav2가 멀쩡해도 이동·순찰 버튼이 "새로고침해보세요"라는 안내 문구와 함께 영구 잠겼다. "주행 취소" 파이프라인 크래시 3종([2026-09-10-patrol-cancel-pipeline.md](./2026-09-10-patrol-cancel-pipeline.md))과 같은 코드 리뷰에서 함께 나온 결과.
> 대상 파일: `pc_careguard/server/app/ws/robot_ws.py`, `pc_careguard/server/app/schemas.py`, `pc_careguard/server/app/api/v1/robots.py`, `pc_careguard/dashboard/src/net/api_client.{h,cpp}`, `pc_careguard/dashboard/src/pages/p6_patrol_page.{h,cpp}`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | `nav_status` WS 메시지를 하나만 놓치면, "새로고침" 버튼을 눌러도 이동·순찰 버튼이 계속 비활성 | `navRunning_`만 유일하게 HTTP 재동기화 경로가 없어 WS push 단일 채널에 의존 | 서버에 `nav_status` 캐시(`robot_ws.py::_nav_running`)를 두고 `GET /robots`가 같이 내려주도록 해, `refreshRobotState()`가 이 값으로도 재동기화 가능하게 함 |

---

## 1. "새로고침"을 눌러도 이동·순찰 버튼이 계속 비활성

**증상**

로봇 상태 동기화 코드를 코드 리뷰로 검토하던 중 발견. `canDrive()`는 `robotOnline_ && !isPatrolling_ && !slamActive_ && navRunning_` 네 조건의 AND라서, `navRunning_`이 한 번 false로 굳으면 나머지 세 조건이 전부 정상이어도 이동·순찰 버튼이 계속 잠긴다. 이때 UI는 "Nav2 준비 중이거나 연결할 수 없습니다 · 잠시 후 새로고침해보세요"라고 안내하지만, 그 새로고침 버튼(`refreshRobotStateBtn_`)을 눌러도 `navRunning_`은 절대 바뀌지 않았다 — 안내 문구가 거짓말을 하는 셈.

**진단 과정**

1. 4개 상태 플래그의 갱신 경로를 비교:

   | 플래그 | 갱신 경로 |
   |---|---|
   | `robotOnline_` | WS `robot_status` **+ `refreshRobotState()`의 `GET /robots`** |
   | `isPatrolling_` | WS 이벤트 **+ `GET /patrol-runs?status=running`** |
   | `slamActive_` | WS 이벤트 **+ `GET /slam-jobs?status=queued`** |
   | `navRunning_` | **오직** WS `nav_status` 메시지뿐 — `refreshRobotState()`에 이를 재조회하는 HTTP 호출 자체가 없음 |

2. `refreshRobotState()`를 봐도 `fetchRobots`/`fetchPatrolRuns`/`fetchSlamJobs` 세 개만 호출 — `navRunning_`을 다시 물어볼 GET 엔드포인트가 서버에 아예 없었다(`nav_status`는 로봇이 상태 변화 시에만 자발적으로 쏘는 WS push일 뿐, "지금 떠 있어?"를 질의하는 API가 없었음).
3. `navRunning_`이 false로 굳는 시나리오를 특정: (a) 페이지를 연 시점에 WS 연결이 아직 안 붙어 그 사이의 push를 놓친 경우, (b) 순간적인 네트워크 끊김으로 메시지 하나만 유실된 경우(재전송/큐잉 없음), (c) 로봇이 이미 정상인 상태에서 대시보드가 나중에 연결된 경우(상태 변화 이벤트라 "지금 상태"를 물을 방법이 없음).

**원인**

나머지 세 플래그를 만들 때 정착시킨 "WS + GET 재조회" 패턴이 `navRunning_` 하나만 빠뜨린 채로 넘어갔다 — 개발 당시 필드 주석에는 이 리스크("SLAM 중도 아니고 Nav2도 안 뜬 제3의 상태")를 이미 적어뒀지만, 실제 복구 경로 구현이 빠져 있었다.

**해결**

- 서버(`robot_ws.py`): `_latest_positions`와 같은 패턴으로 `_nav_running: Dict[str, bool]` 캐시 신설, `nav_status` 메시지 수신마다 갱신, `nav_running(robot_code)` 게터 추가.
- 서버(`schemas.py`/`api/v1/robots.py`): `RobotItem`에 `is_nav_running` 필드 추가, `GET /robots`가 `nav_running()` 값을 같이 내려주도록 연결.
- 대시보드(`api_client.h/cpp`): `RobotInfo`에 `isNavRunning` 필드 추가.
- 대시보드(`p6_patrol_page.cpp`): `refreshRobotState()`의 `fetchRobots` 콜백에서 `navRunning_ = r.isNavRunning`도 같이 반영 — 나머지 세 플래그와 동일하게 "새로고침" 버튼이 실제로 복구 가능해짐.

**검증**

현재 코드에서 확인:
- `robot_ws.py`에 `_nav_running: Dict[str, bool] = {}`, `nav_running(robot_code)` 게터, `nav_status` 수신 시 `_nav_running[robot_code] = bool(message.get("running"))` 갱신이 모두 있음.
- `api/v1/robots.py`가 `is_nav_running=nav_running(row["code"])`로 `RobotItem`에 채워 응답.
- `schemas.py`의 `RobotItem.is_nav_running` 필드 존재.
- `api_client.h`의 `RobotInfo::isNavRunning`, `p6_patrol_page.cpp`의 `navRunning_ = r.isNavRunning`(`refreshRobotState()` 콜백 내부) 확인.

— **해결 완료**. 다만 실제로 `nav_status` WS를 끊어본 뒤 "새로고침" 클릭으로 버튼이 풀리는지까지의 실기 재현 테스트 기록은 남아있지 않다 — 필요하면 별도로 재현해볼 가치가 있다.

---

**교훈**: 같은 종류의 상태 4개 중 3개에 패턴(WS+HTTP 재동기화)을 적용해놓고 하나만 빠뜨리면, 그 하나가 "가끔만" 터지는 락(lock) 버그가 된다 — 코드 리뷰에서 "이 네 가지는 왜 갱신 경로가 다른가"처럼 대칭성을 점검하는 게 이런 종류의 누락을 잡는 데 효과적이었다. 또한 "새로고침해보세요"라는 안내 문구는 실제로 새로고침이 그 상태를 고칠 수 있을 때만 정직하다 — UI 문구와 실제 복구 가능 여부가 어긋나면 사용자가 잘못된 것을 계속 반복하게 만든다.
