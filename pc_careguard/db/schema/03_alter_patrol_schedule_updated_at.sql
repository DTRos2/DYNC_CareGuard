-- patrol_schedule.updated_at 추가 — 스케줄을 수정한 뒤에도 같은 날 다시 트리거될 수
-- 있어야 한다.
-- docker-entrypoint-initdb.d는 새 볼륨을 만들 때만 이 폴더를 실행하므로, 이미 떠 있는
-- 개발 DB에는 이 파일을 직접 psql로 실행해 반영해야 한다:
--   docker exec -i careguard_postgres psql -U careguard -d careguard \
--     < pc_careguard/db/schema/03_alter_patrol_schedule_updated_at.sql
--
-- 실사용 중 발견(2026-09-15): run_time=23분에 스케줄이 정상 트리거된 뒤, 같은 스케줄을
-- run_time=26분으로 수정해도 26분이 지나도록 patrol_run이 전혀 안 생겼다 — 원인은
-- scheduler.py::_tick()의 already_run 판정이 "오늘 이 schedule_id로 이미 한 번이라도
-- 돌았는지"만 봐서, run_time을 바꿔도 "오늘 이미 돌았다"는 사실 자체는 안 바뀌기
-- 때문이었다. updated_at을 기준으로 "마지막 수정 이후에 돈 적이 있는지"로 바꾸면,
-- 수정 시각 이전의 실행은 더 이상 "오늘 이미 돌았다"로 안 쳐서 새 시각에 다시 돈다.
ALTER TABLE patrol_schedule ADD COLUMN updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW();

COMMENT ON COLUMN patrol_schedule.updated_at IS '마지막 수정 시각(생성 시 created_at과 동일). scheduler.py의 "마지막 수정 이후 이미 돌았는지" 판정 기준';
