-- robot_threshold.med_missed_wait_min(분 단위) -> med_missed_wait_sec(초 단위)로 단위 변경.
-- docker-entrypoint-initdb.d는 새 볼륨을 만들 때만 이 폴더를 실행하므로, 이미 떠 있는
-- 개발 DB에는 이 파일을 직접 psql로 실행해 반영해야 한다:
--   docker exec -i careguard_postgres psql -U careguard -d careguard \
--     < pc_careguard/db/schema/04_alter_robot_threshold_med_missed_wait_sec.sql
--
-- P7 "임계값 · 알림 > 복약 공통 > 미수령 판정 시간"이 분 단위로만 입력 가능해서, 초 단위로도
-- 세밀하게 넣을 수 있게 해달라는 요청(2026-09-16)으로 컬럼 자체의 단위를 분에서 초로
-- 바꾼다. SMALLINT(최대 32767)로는 분 단위 상한(1440분=하루)을 초로 그대로 옮기면
-- 86400을 못 담아서 INTEGER로 타입도 같이 바꾼다. 기존에 저장돼 있던 값은 "분" 의미였으므로
-- 컬럼 이름을 바꾸기 전에 60을 곱해 "초" 값으로 변환해둔다.
ALTER TABLE robot_threshold ALTER COLUMN med_missed_wait_min TYPE INTEGER;
UPDATE robot_threshold SET med_missed_wait_min = med_missed_wait_min * 60;
ALTER TABLE robot_threshold RENAME COLUMN med_missed_wait_min TO med_missed_wait_sec;
ALTER TABLE robot_threshold ALTER COLUMN med_missed_wait_sec SET DEFAULT 600;

COMMENT ON COLUMN robot_threshold.med_missed_wait_sec IS '복약 미수령 판정까지 대기 시간 (초)';
