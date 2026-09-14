# 2026-09-09 트러블슈팅 — 복약 스케줄을 바꿔도 주행 스케줄에 반영되지 않음

> 작성자: 도희정
> P6 "주행 스케줄"에 복약 스케줄(`medication_schedule`) 매칭 기능(`patrol_schedule.medication_schedule_id`)을 새로 넣은 뒤, 실사용 테스트 중 "대상자 페이지에서 복약 스케줄 시각을 바꿔도 그 스케줄과 매칭된 주행 스케줄에는 반영이 안 된다"는 리포트로 발견한 문제. 원인이 두 겹으로 겹쳐 있었다 — 서버 DB 레벨에서 참조 자체가 끊어지는 문제와, DB는 맞아도 떠 있는 대시보드에 알려줄 방법이 없던 문제.
> 대상 파일: `pc_careguard/server/app/api/v1/recipient.py`, `pc_careguard/server/app/api/v1/patrol.py`, `pc_careguard/server/app/schemas.py`, `pc_careguard/db/schema/careguard_schema_v1.sql`, `pc_careguard/dashboard/src/net/api_client.{h,cpp}`, `pc_careguard/dashboard/src/pages/p6_patrol_page.{h,cpp}`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 복약 스케줄 시각을 바꿔도 매칭된 주행 스케줄의 시각이 그대로 | `recipient.py::update_recipient`가 대상자 저장 때마다 `medication_schedule` 행을 전부 삭제 후 새로 만들어서, `patrol_schedule.medication_schedule_id`가 참조하던 id가 죽은 행을 가리키게 됨 | 슬롯 번호(slot_no) 기준으로 기존 행을 그 자리에서 UPDATE(id 유지) + 그 id를 참조하는 `patrol_schedule`도 같이 UPDATE |
| 2 | 1번을 고친 뒤에도, DB는 맞게 바뀌는데 **켜져 있는 대시보드 화면**엔 안 보임(껐다 켜야 보임) | 이 변경을 실시간으로 알려주는 WS 메시지 자체가 없었음 — P6은 생성자에서 스케줄을 딱 한 번만 조회 | 서버가 `patrol_schedules_changed`(route_id) WS 브로드캐스트를 보내고, P6이 그 경로의 스케줄만 다시 조회해 반영 |

---

## 1. 대상자 페이지에서 복약 스케줄을 바꿔도 주행 스케줄 시각이 안 바뀜

**증상**

P3 대상자 상세에서 복약 스케줄(약 이름·시각·요일)을 수정해 저장해도, P6에서 그 복약 스케줄과 매칭해둔 "주행 스케줄"의 시각·요일이 그대로 남아있었다.

**진단 과정**

1. 실제 테스트에 쓰인 대상자(박영호, C-002)로 DB를 직접 조회 — `patrol_schedule.medication_schedule_id = 13`인데, `medication_schedule` 테이블엔 id 13이 `deregistered_at`이 채워진 채(삭제됨) 남아있고, 그 대신 id 22(같은 슬롯 1번, 같은 약)가 **새로** 만들어져 있었다.
2. `recipient.py::update_recipient`의 복약 스케줄 저장 코드를 확인 — `UPDATE medication_schedule SET deregistered_at = NOW() WHERE recipient_id = %s`로 **그 대상자의 활성 스케줄을 전부** 지운 뒤, `payload.schedules`를 전부 새로 INSERT하고 있었다. 즉 주소만 고쳐도, 슬롯 하나만 바꿔도 그 대상자의 모든 복약 스케줄 행이 매번 새 id를 받는 구조였다.

**원인**

id가 매번 바뀌니, 그 id를 참조로 들고 있는 `patrol_schedule.medication_schedule_id`가 저장 직후 곧바로 죽은 행을 가리키게 된다. 그 뒤로는 복약 스케줄을 아무리 고쳐도 patrol_schedule 쪽에서 참조하는 id 자체가 실존하지 않으니 반영될 방법이 없다.

**해결**

`update_recipient`를 슬롯 번호(slot_no) 기준 upsert로 바꿨다:
- 기존 활성 슬롯과 겹치는 슬롯 번호가 오면 그 행을 **UPDATE**(id 유지)하고, 곧바로 `UPDATE patrol_schedule SET run_time=%s, days_of_week=%s WHERE medication_schedule_id=%s`로 참조하는 주행 스케줄에도 새 시각·요일을 전파한다.
- 새 슬롯 번호면 그대로 INSERT(기존과 동일).
- 응답에 없는(사용자가 지운) 슬롯만 deregister하고, 그 슬롯을 매칭으로 쓰던 patrol_schedule은 `medication_schedule_id`만 NULL로 풀어준다("선택 안 함" 상태로 — 스케줄 자체는 안 지움).

**검증**

실제 DB로 확인: 끊어져 있던 링크(13→죽은 13번)를 현재 유효한 22번으로 재연결한 뒤, `PUT /recipients/2`로 시각을 08:00→09:15로 바꿔서 (a) `medication_schedule` id가 22 그대로 유지되고(재생성 안 됨), (b) `patrol_schedule.run_time`도 09:15로 같이 바뀌는 것을 확인. 다시 08:00으로 되돌려 원래 테스트 상태로 복원.

---

## 2. DB는 바뀌는데 켜져 있는 대시보드엔 재시작해야 반영됨

**증상**

1번을 고친 뒤 다시 테스트하니, `PUT /recipients/{id}`로 서버 DB는 정확히 바뀌는데(실제로 위 검증에서 확인) 이미 P6을 띄워놓고 보고 있던 대시보드 화면에는 반영이 안 됐다 — 앱을 껐다 다시 켜야만 새 값이 보였다.

**진단 과정**

P6PatrolPage가 웨이포인트·경로·스케줄을 서버에서 받아오는 시점은 생성자의 `loadMapsFromServer()`(맵마다 한 번씩) 뿐이었고, 이후 그 데이터를 다시 조회하게 만드는 경로는 `slam_done`(맵 자체가 바뀔 때)뿐이라 스케줄 변경을 알려주는 채널이 아예 없었다.

**원인**

이 앱은 이미 `/ws/dashboard` WS로 실시간 push를 쓰고 있다(robot_position, robot_status, slam_done 등) — 그런데 "복약 스케줄이 바뀌었다"는 이벤트만 이 채널을 안 타고 있었다. 즉 기능이 고장 난 게 아니라 애초에 안 만들어져 있었다.

**해결**

- 서버: `update_recipient`가 (1번의) 전파 UPDATE에 `RETURNING route_id`를 붙여 영향받은 `patrol_route.id`를 모으고, 커밋 후 `{"type": "patrol_schedules_changed", "route_id": ...}`를 `dashboard_ws.broadcast`로 보낸다. `update_recipient`는 동기(`def`) 핸들러라 `robot_ws.py`처럼 바로 `await` 못 하고, `media.py`의 `video_event_created` 브로드캐스트와 같은 패턴(`anyio.from_thread.run(dashboard_ws.broadcast, ...)`)으로 이벤트 루프 스레드에 맡겼다.
- 대시보드: `P6PatrolPage::handleWsMessage`가 `patrol_schedules_changed`를 받으면 `refreshSchedulesForRoute(routeId)`를 불러 `GET /patrol-schedules?route_id=`로 그 경로의 스케줄만 다시 받아 로컬 목록을 교체한다. 지금 편집 폼이 열려 있으면(`scheduleEditIndex_>=0`) 건드리지 않는다 — 목록을 통째로 바꾸면 폼이 가리키던 인덱스가 어긋날 수 있어서, 폼을 닫을 때까지는 미룬다.

**검증**

파이썬 `websockets` 클라이언트로 `/ws/dashboard`에 직접 붙여서 `PUT /recipients/2`를 호출해보니 `{"type": "patrol_schedules_changed", "route_id": 15}`가 실제로 수신되는 것을 확인 — 이 route_id는 1번 검증에 쓴 `patrol_schedule`이 속한 경로와 일치한다.

---

**교훈**: "데이터가 바뀌었는데 반영이 안 된다"는 리포트는 원인이 한 겹이 아닐 수 있다 — 이번처럼 (a) 애초에 DB 자체가 잘못 바뀌고 있는 경우와 (b) DB는 맞는데 클라이언트가 그걸 모르는 경우가 같이 있을 수 있다. 실제 DB를 직접 SQL로 들여다봐서 "저장은 되는데 참조가 끊어져 있다"는 걸 먼저 확인하고, 그다음에 "그럼 왜 화면엔 안 보이지"를 별도로 봐야 두 원인을 다 잡을 수 있었다. 또한 대량 교체(delete-then-recreate) 패턴은 단순하지만, 다른 테이블이 그 행의 id를 참조하기 시작하는 순간부터는 잠재적 버그가 된다 — id가 다른 테이블의 FK로 쓰이기 시작하면 그때부터는 슬롯/키 기준의 upsert로 바꿔야 안전하다.
