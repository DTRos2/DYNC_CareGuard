import asyncio
import json
import uuid
from datetime import datetime, time as time_cls
from zoneinfo import ZoneInfo

from .. import db
from ..api.v1.patrol import start_patrol_run

KST = ZoneInfo("Asia/Seoul")

# patrol_schedule을 확인하는 주기. run_time과 지금 사이의 간격이 이 값보다 작으면
# "지금 트리거할 시각"으로 본다(자정을 넘는 경계는 단순화를 위해 다루지 않는다).
POLL_INTERVAL_SEC = 20


async def run_scheduler_loop() -> None:
    """patrol_schedule·medication_schedule을 주기적으로 확인한다.

    체크리스트 ⑦의 "사람이 아니라 스케줄러가 트리거하는 버전" — 순찰은
    api/v1/patrol.py::start_patrol_run()을 그대로 재사용하고, 복약은 이 파일의
    _medication_tick()이 미수령을 감지해 care_event를 남긴다.
    """
    while True:
        try:
            await _tick()
        except Exception:
            # 개별 tick 실패로 스케줄러 자체가 죽으면 안 된다 — 다음 tick에서 다시 시도한다.
            pass
        try:
            await _medication_tick()
        except Exception:
            pass
        await asyncio.sleep(POLL_INTERVAL_SEC)


async def _tick() -> None:
    now = datetime.now(KST)
    weekday_bit = 1 << now.weekday()  # 월요일(0) -> 1, ... 일요일(6) -> 64
    now_time = now.time()

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT ps.id AS schedule_id, ps.route_id, ps.run_time, pr.map_id
            FROM patrol_schedule ps
            JOIN patrol_route pr ON pr.id = ps.route_id
            WHERE ps.deregistered_at IS NULL
              AND pr.deregistered_at IS NULL
              AND (ps.days_of_week & %(bit)s) != 0
            """,
            {"bit": weekday_bit},
        )
        schedules = cur.fetchall()

        due = [s for s in schedules if _is_due(s["run_time"], now_time)]
        if not due:
            return
        due_ids = [s["schedule_id"] for s in due]

        cur.execute(
            "SELECT DISTINCT schedule_id FROM patrol_run WHERE schedule_id = ANY(%s) AND started_at::date = %s",
            (due_ids, now.date()),
        )
        already_run = {r["schedule_id"] for r in cur.fetchall()}

        cur.execute("SELECT id, robot_id FROM map WHERE is_active")
        robot_by_map = {r["id"]: r["robot_id"] for r in cur.fetchall()}

    for s in due:
        if s["schedule_id"] in already_run:
            continue
        robot_id = robot_by_map.get(s["map_id"])
        if robot_id is None:
            continue
        await start_patrol_run(robot_id, s["route_id"], "schedule", schedule_id=s["schedule_id"])


def _is_due(run_time: time_cls, now_time: time_cls) -> bool:
    # 예전엔 이 창 폭이 POLL_INTERVAL_SEC(20초)와 똑같았다 — _tick() 자체도 정확히
    # 20초마다 도는데, 서버가 언제 시작됐냐에 따라 그 20초 틱이 벽시계 20초 경계와
    # 안 맞아떨어지면 run_time~run_time+20s 사이에 틱이 단 한 번도 안 걸릴 수 있고,
    # 그러면 그날은 영영 트리거가 안 됐다 — 실사용 중 실제로 겪은 버그(2026-09-08,
    # 19:35 스케줄이 19:36까지 계속 안 켜짐). 창을 좁게 유지할 이유가 없다: 같은 날
    # 중복 실행은 _tick()의 already_run(DB 조회)이 이미 따로 막고 있으므로, 여기서는
    # "오늘 run_time이 이미 지났는지"만 보면 된다 — 서버가 잠깐 멈췄다 늦게 봐도 그날
    # 안에는 늦게라도 한 번은 도는 게, 아예 안 도는 것보다 낫다.
    run_sec = run_time.hour * 3600 + run_time.minute * 60 + run_time.second
    now_sec = now_time.hour * 3600 + now_time.minute * 60 + now_time.second
    return now_sec >= run_sec


async def _medication_tick() -> None:
    """medication_schedule을 확인해 복약 시각이 지나도록 'delivered' 기록이 없으면
    care_event(MED_MISSED)를 남긴다 (시스템 설정 > 임계값·알림의 "미수령 판정
    시간"·"재안내 횟수"를 그대로 쓴다).

    system_event가 아니라 care_event를 쓰는 이유: MED_MISSED는 care_event의
    event_type CHECK 목록에 이미 정의돼 있고(대시보드 이벤트 큐 원본), 스키마
    주석상 system_event는 "로봇·서버 자체 장애 로그" 전용이라 복약 같은 돌봄
    이벤트를 넣을 자리가 아니다.

    "재안내 횟수"는 로봇이 음성으로 다시 안내하는 기능이 아직 없어서(HMI 음성
    응답 대기 자체가 미구현), 여기서는 "담당자에게 몇 번까지 반복 경보할지"로
    해석한다 — 재안내 간격은 별도 임계값이 없어 "미수령 판정 시간"을 그대로
    재사용한다(패턴을 늘리지 않으려는 의도적 단순화).
    """
    now = datetime.now(KST)
    weekday_bit = 1 << now.weekday()
    now_time = now.time()

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT ms.id AS schedule_id, ms.recipient_id, ms.slot_no, ms.drug_name, ms.dose_time,
                   rc.robot_id, rb.code AS robot_code,
                   COALESCE(rt.med_missed_wait_min, 10) AS wait_min,
                   COALESCE(rt.med_reannounce_count, 2) AS reannounce_count
            FROM medication_schedule ms
            JOIN recipient rc ON rc.id = ms.recipient_id
            JOIN robot rb ON rb.id = rc.robot_id
            LEFT JOIN robot_threshold rt ON rt.robot_id = rc.robot_id
            WHERE ms.deregistered_at IS NULL
              AND (ms.days_of_week & %(bit)s) != 0
            """,
            {"bit": weekday_bit},
        )
        schedules = cur.fetchall()

        due = [s for s in schedules if _med_is_due(s["dose_time"], now_time, s["wait_min"])]
        if not due:
            return

        for s in due:
            cur.execute(
                """
                SELECT result, created_at FROM medication_log
                WHERE schedule_id = %s AND created_at::date = %s
                ORDER BY created_at DESC
                """,
                (s["schedule_id"], now.date()),
            )
            logs = cur.fetchall()
            if any(log["result"] == "delivered" for log in logs):
                continue  # 이미 정상 수령 — 미수령 아님

            missed_logs = [log for log in logs if log["result"] == "missed"]
            if len(missed_logs) >= s["reannounce_count"]:
                continue  # 재안내 횟수를 이미 다 써서 더 경보하지 않음
            if missed_logs and (now - missed_logs[0]["created_at"]).total_seconds() < s["wait_min"] * 60:
                continue  # 마지막 경보 이후 재안내 간격이 아직 안 지남

            cur.execute(
                """
                INSERT INTO medication_log (schedule_id, recipient_id, robot_id, slot_no, result)
                VALUES (%s, %s, %s, %s, 'missed')
                """,
                (s["schedule_id"], s["recipient_id"], s["robot_id"], s["slot_no"]),
            )

            missed_count = len(missed_logs) + 1
            event_uid = f"EV-{s['robot_code']}-{now:%y%m%d}-{uuid.uuid4().hex[:8]}"
            cur.execute(
                """
                INSERT INTO care_event (event_uid, recipient_id, robot_id, trigger_path, event_type,
                    severity, summary, evidence, occurred_at)
                VALUES (%s, %s, %s, 'SYS', 'MED_MISSED', 'attention', %s, %s, NOW())
                """,
                (
                    event_uid, s["recipient_id"], s["robot_id"],
                    f"{s['drug_name']} 복약 미수령 ({missed_count}/{s['reannounce_count']}회 경보)",
                    json.dumps({
                        "schedule_id": s["schedule_id"],
                        "slot_no": s["slot_no"],
                        "dose_time": s["dose_time"].isoformat(),
                        "missed_count": missed_count,
                    }),
                ),
            )
        conn.commit()


def _med_is_due(dose_time: time_cls, now_time: time_cls, wait_min: int) -> bool:
    """복약 시각 + 미수령 판정 대기시간이 지났으면 True. _is_due와 같은 "창을
    넓게" 원칙을 따른다 — 정확히 그 순간만 잡는 좁은 창은 서버가 잠깐 멈추면
    그날 영영 못 잡을 수 있다(scheduler.py _is_due 주석 참고)."""
    dose_sec = dose_time.hour * 3600 + dose_time.minute * 60 + dose_time.second
    now_sec = now_time.hour * 3600 + now_time.minute * 60 + now_time.second
    return now_sec >= dose_sec + wait_min * 60
