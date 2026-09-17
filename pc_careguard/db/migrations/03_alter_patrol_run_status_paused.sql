-- patrol_run.status에 paused(ESP32 긴급 버튼 호출로 잠시 밀려난 순찰) 추가.
-- migrations/ 폴더는 docker-entrypoint-initdb.d에 마운트되지 않는다(schema/ 폴더만
-- 마운트됨) — 새 볼륨을 만들 때도 자동 실행되지 않으므로, 개발 DB에는 직접 psql로
-- 실행해 반영해야 한다:
--   docker exec -i careguard_postgres psql -U careguard -d careguard \
--     < pc_careguard/db/migrations/03_alter_patrol_run_status_paused.sql
--
-- 순찰(schedule/manual) 도중 ESP32 SOS 버튼이 눌리면 그 순찰을 완전히 중단
-- (aborted)시키는 대신 paused로 남겨 긴급 이동이 끝난 뒤 자동으로 이어서 돈다
-- (server/app/ws/robot_ws.py::_resume_patrol_run). running과 동일하게 "이 로봇을
-- 점유 중"으로 취급해 새 수동 순찰/이동 요청을 막는다(patrol.py::_reject_if_already_patrolling).
ALTER TABLE patrol_run DROP CONSTRAINT patrol_run_status_check;
ALTER TABLE patrol_run ADD CONSTRAINT patrol_run_status_check CHECK (
    status IN ('running', 'paused', 'completed', 'aborted', 'failed')
);

COMMENT ON COLUMN patrol_run.status IS 'running(진행 중) / paused(ESP32 긴급 호출로 일시 중단, 자동 재개 대상) / completed / aborted / failed';
