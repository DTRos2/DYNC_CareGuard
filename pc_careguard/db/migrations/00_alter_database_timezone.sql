-- 운영 DB 타임존 고정. 신규/기존 DB 모두에 1회 실행.
-- migrations/ 폴더는 docker-entrypoint-initdb.d에 마운트되지 않는다(schema/ 폴더만
-- 마운트됨) — 이 파일은 새 볼륨을 만들 때 자동 실행되지 않으므로, 개발 DB에는
-- 직접 psql로 실행해 반영해야 한다:
--   docker exec -i careguard_postgres psql -U careguard -d careguard \
--     < pc_careguard/db/migrations/00_alter_database_timezone.sql
ALTER DATABASE careguard SET timezone = 'Asia/Seoul';
