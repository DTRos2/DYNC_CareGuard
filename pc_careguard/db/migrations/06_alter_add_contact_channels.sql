-- 보호자 알림을 Solapi SMS 대신 이메일로 보내기 위한 컬럼 추가 + 대상자 본인
-- 연락처 컬럼 추가. 겸사겸사 요청받은 두 컬럼을 한 파일로 묶는다(04번 파일처럼
-- 하나의 기능 단위로 여러 ALTER를 같이 담는 이 저장소 관례를 따름).
--
-- migrations/ 폴더는 docker-entrypoint-initdb.d에 마운트되지 않는다(schema/ 폴더만
-- 마운트됨) — 새 볼륨을 만들 때도 자동 실행되지 않으므로, 이미 떠 있는 개발 DB에는
-- 직접 psql로 실행해 반영해야 한다:
--   docker exec -i careguard_postgres psql -U careguard -d careguard \
--     < pc_careguard/db/migrations/06_alter_add_contact_channels.sql

ALTER TABLE recipient ADD COLUMN phone VARCHAR(32);
COMMENT ON COLUMN recipient.phone IS '대상자 본인 연락처. NULL 허용';

ALTER TABLE guardian ADD COLUMN email VARCHAR(120);
COMMENT ON COLUMN guardian.email IS '보호자 이메일 (알림 메일 발송 대상). NULL이면 메일 발송 대상에서 제외';

ALTER TABLE notification_log DROP CONSTRAINT notification_log_channel_check;
ALTER TABLE notification_log ADD CONSTRAINT notification_log_channel_check CHECK (
    channel IN ('sms', 'dashboard', 'call', 'mail')
);
