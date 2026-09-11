-- waypoint.kind에 emergency(긴급 버튼 위치) 추가.
-- docker-entrypoint-initdb.d는 새 볼륨을 만들 때만 이 폴더를 실행하므로, 이미
-- 떠 있는 개발 DB에는 이 파일을 직접 psql로 실행해 반영해야 한다:
--   docker exec -i careguard_postgres psql -U careguard -d careguard \
--     < pc_careguard/db/schema/01_alter_waypoint_kind_emergency.sql
--
-- emergency는 charge와 달리 맵당 여러 개 등록할 수 있고(긴급 버튼이 여러 곳에
-- 있을 수 있음), patrol_route.steps에는 넣지 않는다(경로 없이 그 지점으로 바로
-- 이동) — 별도 유니크 인덱스는 추가하지 않는다(요청 사양). steps 검증은
-- server/app/api/v1/patrol.py의 애플리케이션 코드가 담당한다(DB 레벨 CHECK로는
-- "이 kind는 steps에 넣을 수 없다"를 표현할 수 없다).
ALTER TABLE waypoint DROP CONSTRAINT waypoint_kind_check;
ALTER TABLE waypoint ADD CONSTRAINT waypoint_kind_check CHECK (
    kind IN ('patrol', 'charge', 'emergency')
);

COMMENT ON COLUMN waypoint.kind IS 'patrol(순찰 지점) / charge(충전 스테이션 = 초기 위치, 경로 순서에 포함하지 않음) / emergency(긴급 버튼 위치, 맵당 여러 개 가능, 경로 순서에 포함하지 않고 즉시 단독 이동만 한다)';
