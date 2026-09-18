# 2026-09-17 트러블슈팅 — 순찰 시작 WS 전송 실패 시 patrol_run 고아화 + 스케줄 재시도 불능 (연쇄)

> 작성자: 도희정
> 대상 파일: `pc_careguard/server/app/api/v1/patrol.py`, `pc_careguard/server/app/services/scheduler.py`
> 대상 로봇: R-07 (run_id 113/127에서 실제 재현·확인)
> 관련 문서: [2026-09-08-p6-patrol-live-robot-debugging.md](./2026-09-08-p6-patrol-live-robot-debugging.md) (`_is_due()`/`already_run` 스케줄러 판정 로직 최초 도입), [2026-09-15-r07-patrol-schedule-priority-and-retrigger.md](./2026-09-15-r07-patrol-schedule-priority-and-retrigger.md) (같은 스케줄러/patrol_run 영역의 이전 사고)

## 요약

같은 날 안에 순서대로 발견된 두 문제. 1번 수정이 `patrol_run.status='failed'`라는 새 상태를 만들었는데, 그 상태를 스케줄러가 어떻게 취급할지 미처 안 정해서 2번이 바로 이어졌다.

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | `patrol_run`이 `running`으로 DB에 남아있는데 로봇 쪽엔 `patrol_start`를 받은 흔적이 전혀 없음(run_id 113/127에서 실제 확인) — P6/P8 화면에는 "주행 중"으로 영원히 표시됨 | `start_patrol_run()`이 `status='running'`으로 먼저 커밋한 뒤 로봇에 WS로 `patrol_start`를 보내는데, 그 전송 자체가 실패해도 되돌리는 코드가 없었음. 스케줄러 자동 트리거는 반환값을 안 보므로 특히 조용히 stuck됨 | WS 전송이 실패하면 그 자리에서 즉시 `status='failed'`, `ended_at=NOW()`로 되돌림 |
| 2 | "스케줄 순찰이 됐다 안 됐다 한다" — 특정 날 특정 스케줄이 하루 종일 한 번도 안 도는 경우가 생김 | 1번 수정으로 실패가 `status='failed'` row로 남게 됐는데, `scheduler.py::_tick()`의 "오늘 이미 돌았는지" 체크(`already_run`)가 status를 안 보고 그날 row가 하나라도 있으면 무조건 "이미 시도함"으로 쳤음. 20초 틱 중 하필 로봇이 재연결 중이거나 네트워크가 잠깐 흔들려 WS 전송이 한 번 실패하면, 그 하루는 다시는 트리거되지 않음 | `already_run` 조회에서 `status != 'failed'` 조건 추가(실패는 "오늘 안 돌았다"로 침) + 로봇이 아예 연결 안 된 흔한 경우엔 `robot_ws.is_connected()`로 미리 걸러 `patrol_run` row 자체를 안 만들고 다음 틱(20초 후)으로 넘김 |

---

## 1. patrol_run이 running으로 고아 남는 문제

### 현상

R-07 run_id 113, 127 — DB의 `patrol_run.status`는 `running`인데, 로봇 쪽 `journalctl`/agent 로그에는 `patrol_start` 메시지를 받은 흔적 자체가 없었다. 로봇은 실제로 아무 것도 하지 않고 가만히 있는데, 대시보드(P6/P8)에는 "주행 중" 상태가 계속 남아 사용자가 취소도 못 하고 헷갈리는 상황이었다.

### 원인

`start_patrol_run()`(`patrol.py`)의 순서가 다음과 같았다:

1. `INSERT INTO patrol_run (...) VALUES (..., 'running', ...)` — **로봇 연결 여부와 무관하게** 먼저 `running`으로 커밋. P8/P1이 "순찰이 시작됨"을 새로고침 없이 바로 보게 하려는 의도적 설계(2026-09-12 도입, `patrol_run_created` broadcast와 같은 목적).
2. `await robot_ws.send_command(robot["code"], {"type": "patrol_start", ...})` 호출.

문제는 2번이 실패해도(로봇이 그 순간 연결 안 돼 있었거나 `send_json`이 예외를 던진 경우) **그 실패를 되돌리는 코드가 전혀 없었다**는 것. 로봇은 명령을 받은 적도 없는데 DB엔 `running`만 영원히 남는다. `scheduler.py`의 자동 트리거는 `start_patrol_run()`의 반환값을 아예 확인하지 않으므로, 스케줄 순찰에서 이 실패가 나면 아무도 모르게 조용히 stuck된다.

### 해결

`robot_ws.send_command()`의 반환값(`connected`)을 확인해, 실패 시 방금 커밋한 row를 즉시 `failed`로 되돌린다.

```python
connected = await robot_ws.send_command(
    robot["code"],
    {"type": "patrol_start", "run_id": row["id"], ...},
)
if not connected:
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "UPDATE patrol_run SET status = 'failed', ended_at = NOW() "
            "WHERE id = %s AND status = 'running' "
            "RETURNING status, ended_at",
            (row["id"],),
        )
        updated = cur.fetchone()
        conn.commit()
    if updated is not None:
        row["status"] = updated["status"]
        row["ended_at"] = updated["ended_at"]
return PatrolRunItem(**row, robot_connected=connected)
```

`WHERE ... AND status = 'running'` 조건을 붙여, 그 사이 다른 경로(취소 등)로 이미 상태가 바뀐 경우엔 되돌리지 않도록 했다.

---

## 2. 스케줄 순찰이 하루 종일 재시도 안 되는 문제

### 현상

1번을 고친 지 몇 시간 뒤, "스케줄 순찰이 됐다 안 됐다 한다"는 리포트가 들어왔다. 특정 스케줄이 예약 시각에 트리거되지 않고 그대로 그날이 넘어가는 경우가 있었다.

### 원인

`scheduler.py::_tick()`의 "오늘 이미 돌았는지" 판정(`already_run`)이 문제였다:

```sql
SELECT DISTINCT pr.schedule_id
FROM patrol_run pr
JOIN patrol_schedule ps ON ps.id = pr.schedule_id
WHERE pr.schedule_id = ANY(%s) AND pr.started_at::date = %s AND pr.started_at >= ps.updated_at
```

이 쿼리는 `status`를 전혀 안 본다 — 그날 그 스케줄의 `patrol_run` row가 **하나라도 있으면** 무조건 "오늘 이미 시도했다"로 친다. 1번 수정 이전에는 실패가 아예 row로 안 남거나(고아로 `running`) 문제가 됐지만, 1번을 고치고 나니 실패가 이제 정직하게 `status='failed'` row로 남게 됐다 — 그런데 이 row가 바로 "이미 시도함" 판정에 걸려버린 것.

20초마다 도는 틱 중 하필 로봇이 마침 재연결 중이었거나(`RECONNECT_DELAY_SEC=3.0`) 네트워크가 한 번 흔들려서 `send_command`가 그 순간에만 실패하면, `status='failed'` row 하나가 그날 남고, 그 스케줄은 다음 틱은 물론 그날 남은 모든 틱에서도 계속 "이미 돌았다"로 건너뛰어진다 — 단 한 번의 일시적 전송 실패가 하루 전체의 스케줄 미실행으로 번지는 구조였다.

이건 [2026-09-08 문서](./2026-09-08-p6-patrol-live-robot-debugging.md)에서 `_is_due()`에 이미 적용했던 "판정 창을 좁게 잡지 말고, 늦게라도 한 번은 도는 게 안 도는 것보다 낫다"는 원칙이 `already_run` 쪽에는 아직 안 들어가 있었던 셈이다.

### 해결

1. **`already_run` 조회에서 `status != 'failed'` 제외** — 실패는 "오늘 안 돌았다"로 쳐서 다음 틱(20초 후)에 자동 재시도되게 한다.

   ```sql
   WHERE pr.schedule_id = ANY(%s) AND pr.started_at::date = %s AND pr.started_at >= ps.updated_at
     AND pr.status != 'failed'
   ```

2. **로봇 미연결이 뻔히 보이는 흔한 경우는 애초에 시도 자체를 안 함** — map-robot 조인에 `robot.code`를 추가로 가져와서, `start_patrol_run` 호출 전에 `robot_ws.is_connected(robot_code)`를 먼저 확인한다. 연결 안 된 게 확실하면 `patrol_run` row 자체를 안 만들고 건너뛴다(1번 조건 덕분에 다음 틱에 자연스럽게 다시 시도됨 — `failed` row가 계속 쌓이지 않는다).

   ```python
   entry = robot_by_map.get(s["map_id"])
   if entry is None:
       continue
   robot_id, robot_code = entry
   if not robot_ws.is_connected(robot_code):
       continue
   await start_patrol_run(robot_id, s["route_id"], "schedule", schedule_id=s["schedule_id"])
   ```

   `send_json`이 연결은 있어 보이는데 전송 그 순간에만 실패하는 더 좁은 경합은 여전히 1번(`start_patrol_run` 내부의 `status='failed'` 처리)이 잡는다 — 두 방어선이 서로 다른 실패 범위를 담당하는 구조.

## 교훈

1번(원자적 롤백)만으로는 안 끝난다는 게 이번 사고의 핵심이다. **새로운 상태값(`failed`)을 도입하면, 그 상태를 참조하는 다른 모든 쿼리/판정 로직도 같이 검토해야 한다** — `patrol_run.status`를 보는 곳이 `start_patrol_run` 하나가 아니라 `scheduler.py`의 "오늘 이미 돌았나" 판정에도 있었는데, 그쪽은 새 상태값의 존재 자체를 몰랐다. 상태 머신에 값을 하나 추가하는 변경은 항상 "이 컬럼을 읽는 다른 쿼리가 또 있는가"를 같이 확인하는 습관이 필요하다.
