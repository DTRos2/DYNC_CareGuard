-- 운영 DB 타임존 고정. 스키마 v3 헤더 주석에 따라 1회 실행.
-- docker-entrypoint-initdb.d 에서 파일명 알파벳 순으로 실행되므로
-- 스키마 본문(careguard_schema_v1.sql)보다 먼저 적용된다.
ALTER DATABASE careguard SET timezone = 'Asia/Seoul';
