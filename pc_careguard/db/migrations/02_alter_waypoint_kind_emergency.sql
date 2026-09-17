-- waypoint.kind에 emergency(긴급 버튼 위치) 추가.
-- migrations/ 폴더는 docker-entrypoint-initdb.d에 마운트되지 않는다(schema/ 폴더만
-- 마운트됨) — 새 볼륨을 만들 때도 자동 실행되지 않으므로, 개발 DB에는 직접 psql로
-- 실행해 반영해야 한다:
--   docker exec -i careguard_postgres psql -U careguard -d careguard \
--     < pc_careguard/db/migrations/02_alter_waypoint_kind_emergency.sql
--
-- emergency는 charge와 달리 맵당 여러 개 등록할 수 있고(긴급 버튼이 여러 곳에
-- 있을 수 있음) — 별도 유니크 인덱스는 추가하지 않는다(요청 사양). patrol_route.steps
-- 에는 patrol과 마찬가지로 넣을 수 있다("경로 이동"으로 경로 없이 즉시 단독 이동도
-- 별도로 가능). charge만 steps에서 막는데, 그 검증은 DB CHECK로는 표현할 수 없어
-- server/app/api/v1/patrol.py의 애플리케이션 코드가 담당한다.
ALTER TABLE waypoint DROP CONSTRAINT waypoint_kind_check;
ALTER TABLE waypoint ADD CONSTRAINT waypoint_kind_check CHECK (
    kind IN ('patrol', 'charge', 'emergency')
);

COMMENT ON COLUMN waypoint.kind IS 'patrol(순찰 지점) / charge(충전 스테이션 = 초기 위치, 경로 순서에 포함하지 않음) / emergency(긴급 버튼 위치, 맵당 여러 개 가능. patrol과 마찬가지로 경로에 포함할 수 있고, "경로 이동"으로 경로 없이 즉시 단독 이동도 가능하다)';
