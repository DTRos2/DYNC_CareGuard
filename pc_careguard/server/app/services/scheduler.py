import asyncio
from datetime import datetime, time as time_cls
from zoneinfo import ZoneInfo

from .. import db
from ..api.v1.patrol import start_patrol_run

KST = ZoneInfo("Asia/Seoul")

# patrol_schedule을 확인하는 주기. run_time과 지금 사이의 간격이 이 값보다 작으면
# "지금 트리거할 시각"으로 본다(자정을 넘는 경계는 단순화를 위해 다루지 않는다).
POLL_INTERVAL_SEC = 20


async def run_scheduler_loop() -> None:
    """patrol_schedule을 주기적으로 확인해 시각이 되면 자동으로 순찰을 시작한다.

    체크리스트 ⑦의 "사람이 아니라 스케줄러가 트리거하는 버전" — 핵심 로직은
    api/v1/patrol.py::start_patrol_run()을 그대로 재사용한다.
    """
    while True:
        try:
            await _tick()
        except Exception:
            # 개별 tick 실패로 스케줄러 자체가 죽으면 안 된다 — 다음 tick에서 다시 시도한다.
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
