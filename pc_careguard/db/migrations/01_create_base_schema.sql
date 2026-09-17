-- migrations/ 재구성(2026-09-17)으로 새로 만든 파일 — 번호 붙은 alter들이 실제로
-- 의존하는 테이블을 먼저 만들어두는 "기준 스냅샷"이다. 02_alter_waypoint_kind_emergency.sql이
-- 추가되기 직전 커밋(64a7b3d^)의 schema/careguard_schema_v1.sql 본문을 그대로 얼렸다 —
-- 그 시점 이후의 각 번호 파일을 이 파일 위에 순서대로 적용하면 지금의
-- schema/careguard_schema.sql과 같은 상태가 된다.
--
-- migrations/ 폴더는 docker-entrypoint-initdb.d에 마운트되지 않는다(schema/ 폴더만
-- 마운트됨) — 새 볼륨은 schema/careguard_schema.sql 하나로 바로 지금 상태로 뜨므로
-- 이 파일을 실행할 필요가 없다. 이 파일은 "이 시점까지의 스키마를 기록해 둔 이력"
-- 및 migrations/ 폴더만으로 처음부터 재구성해보고 싶을 때를 위한 것이다.

-- =========================================================
-- CareGuard Central Server DB Schema  v4
-- PostgreSQL 16 (Docker, 로컬 운영)
--
-- - 모든 FK(REFERENCES) 제약 제거. 참조 정합성은 애플리케이션 코드의
--   JOIN으로 관리한다 (컬럼 자체와 값의 의미는 v2와 동일, 제약만 제거)
-- - v4: recipient.operator_id 추가 — 계정 관리 화면의 "담당(대상자·로봇)"을
--   저장할 곳이 없어서(로봇은 recipient.robot_id로 이미 연결) 신설. 담당
--   로봇은 recipient.robot_id를 통해 간접적으로 따라온다
--
-- 시간 처리
-- - 컬럼 타입은 TIMESTAMPTZ 유지. 운영 DB에 아래를 1회 실행할 것:
--       ALTER DATABASE careguard SET timezone = 'Asia/Seoul';
-- =========================================================
SET timezone = 'Asia/Seoul';

BEGIN;

-- =========================================================
-- 1. Role / Users
-- =========================================================

CREATE TABLE role (
    id          INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    code        VARCHAR(16) NOT NULL UNIQUE,
    name        VARCHAR(64) NOT NULL,
    description TEXT,
    is_active   BOOLEAN NOT NULL DEFAULT TRUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

COMMENT ON TABLE role IS '계정 역할. 역할 추가·설명 변경을 코드 수정 없이 데이터로 관리한다';
COMMENT ON COLUMN role.id IS '역할 PK, 자동 증가';
COMMENT ON COLUMN role.code IS '역할 코드. 애플리케이션 분기 기준, 예: admin / operator / guardian';
COMMENT ON COLUMN role.name IS '역할 표시 이름';
COMMENT ON COLUMN role.description IS '역할 설명 및 권한 범위';
COMMENT ON COLUMN role.is_active IS '사용 여부. guardian은 1차 구현에서 FALSE로 두어 로그인을 막는다';
COMMENT ON COLUMN role.created_at IS '역할 등록 시간';

INSERT INTO role (code, name, description, is_active) VALUES
    ('admin',    '관리자',   '전체 기능 및 계정·설정 관리', TRUE),
    ('operator', '관제 담당자', '이벤트 처리, 영상 열람, 로봇 명령', TRUE),
    ('guardian', '보호자',   '담당 대상자 정보 열람 및 메시지 등록. 1차 구현에서는 비활성', FALSE);

CREATE TABLE careguard_user (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    login_id      VARCHAR(64) NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    display_name  VARCHAR(64) NOT NULL,
    role_id       INTEGER NOT NULL,
    is_active     BOOLEAN NOT NULL DEFAULT TRUE,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_login_at TIMESTAMPTZ,
    deleted_at    TIMESTAMPTZ
);

CREATE UNIQUE INDEX careguard_user_active_login_id_idx
ON careguard_user (login_id) WHERE deleted_at IS NULL;

CREATE INDEX idx_careguard_user_role ON careguard_user (role_id);

COMMENT ON TABLE careguard_user IS '관제 대시보드 로그인 계정';
COMMENT ON COLUMN careguard_user.id IS '계정 PK, 자동 증가';
COMMENT ON COLUMN careguard_user.login_id IS '로그인 ID. 활성 계정끼리만 유니크';
COMMENT ON COLUMN careguard_user.password_hash IS '비밀번호 해시';
COMMENT ON COLUMN careguard_user.display_name IS '화면 표시용 이름. 상단바 담당자, 계정 목록, 감사 로그, 이벤트 처리자 표시에 사용';
COMMENT ON COLUMN careguard_user.role_id IS '역할 id (role.id). FK 제약 없음, 조회 시 애플리케이션에서 JOIN';
COMMENT ON COLUMN careguard_user.is_active IS '계정 사용 가능 여부';
COMMENT ON COLUMN careguard_user.created_at IS '계정 생성 시간';
COMMENT ON COLUMN careguard_user.last_login_at IS '마지막 로그인 시간';
COMMENT ON COLUMN careguard_user.deleted_at IS '계정 삭제 시간. NULL이면 사용 중';


-- =========================================================
-- 2. Robot
-- =========================================================

CREATE TABLE robot (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    code            VARCHAR(16) NOT NULL,
    name            VARCHAR(64) NOT NULL,
    model           VARCHAR(64) NOT NULL DEFAULT 'limo',
    ros_domain_id   INTEGER CHECK (ros_domain_id IS NULL OR ros_domain_id BETWEEN 0 AND 232),
    ip              VARCHAR(45),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deregistered_at TIMESTAMPTZ
);

CREATE UNIQUE INDEX robot_active_code_idx
ON robot (code) WHERE deregistered_at IS NULL;

COMMENT ON TABLE robot IS '로봇 등록 정보';
COMMENT ON COLUMN robot.id IS '로봇 PK, 자동 증가';
COMMENT ON COLUMN robot.code IS '로봇 코드. MQTT 토픽 cg/v1/robot/{code}/... 의 키, 예: R-01';
COMMENT ON COLUMN robot.name IS '로봇 표시 이름';
COMMENT ON COLUMN robot.model IS '로봇 모델명';
COMMENT ON COLUMN robot.ros_domain_id IS '로봇 내부 전용 ROS_DOMAIN_ID. 로봇마다 분리해 DDS가 기계 경계를 넘지 않게 한다';
COMMENT ON COLUMN robot.ip IS '로봇에 고정 할당된 IP (등록 시 직접 입력, 자동 갱신 아님)';
COMMENT ON COLUMN robot.created_at IS '로봇 등록 시간';
COMMENT ON COLUMN robot.deregistered_at IS '로봇 등록 해제 시간. NULL이면 사용 중';

CREATE TABLE robot_threshold (
    robot_id              INTEGER PRIMARY KEY,
    battery_attention_pct SMALLINT NOT NULL DEFAULT 30,
    battery_return_pct    SMALLINT NOT NULL DEFAULT 20,
    voice_wait_sec        SMALLINT NOT NULL DEFAULT 8,
    reconnect_retry_sec   SMALLINT NOT NULL DEFAULT 30,
    med_missed_wait_min   SMALLINT NOT NULL DEFAULT 10,
    med_reannounce_count  SMALLINT NOT NULL DEFAULT 2,
    updated_at            TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

COMMENT ON TABLE robot_threshold IS '로봇별 임계값·알림 설정 (시스템 설정 > 임계값·알림 탭). 행이 없는 로봇은 기본값(위 컬럼 DEFAULT)을 쓴다';
COMMENT ON COLUMN robot_threshold.robot_id IS 'robot.id — 로봇 한 대당 설정 행 하나';
COMMENT ON COLUMN robot_threshold.battery_attention_pct IS '배터리 주의 알림 기준 (%)';
COMMENT ON COLUMN robot_threshold.battery_return_pct IS '배터리 자동 복귀 기준 (%)';
COMMENT ON COLUMN robot_threshold.voice_wait_sec IS '음성 응답 대기 시간 (초)';
COMMENT ON COLUMN robot_threshold.reconnect_retry_sec IS '재연결 재시도 주기 (초)';
COMMENT ON COLUMN robot_threshold.med_missed_wait_min IS '복약 미수령 판정까지 대기 시간 (분)';
COMMENT ON COLUMN robot_threshold.med_reannounce_count IS '복약 재안내 횟수';
COMMENT ON COLUMN robot_threshold.updated_at IS '마지막 저장 시간';


-- =========================================================
-- 3. Files (모든 파일의 단일 관리 지점)
-- =========================================================

CREATE TABLE files (
    id                INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    entity_type       VARCHAR(50) NOT NULL,
    entity_id         VARCHAR(100) NOT NULL,
    storage_path      TEXT NOT NULL,
    original_filename TEXT NOT NULL,
    duration_sec      INTEGER CHECK (duration_sec IS NULL OR duration_sec >= 0),
    uploaded_by       INTEGER,
    retention_until   TIMESTAMPTZ,
    display_order     INTEGER NOT NULL DEFAULT 0,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMPTZ,
    deleted_at        TIMESTAMPTZ
);

CREATE INDEX idx_files_entity ON files (entity_type, entity_id);
CREATE INDEX idx_files_active ON files (deleted_at) WHERE deleted_at IS NULL;
CREATE INDEX idx_files_retention ON files (retention_until) WHERE deleted_at IS NULL;

COMMENT ON TABLE files IS '통합 파일 관리. 대상자 사진, 침입자 캡처, 이벤트·주행 녹화, 보호자 영상·음성 메시지, 맵 이미지를 모두 수용한다. 로컬 파일시스템만 사용';
COMMENT ON COLUMN files.id IS '파일 PK, 자동 증가';
COMMENT ON COLUMN files.entity_type IS '연결 대상 종류이자 파일 분류, 예: recipient_photo / intruder_capture / event_clip / driving_clip / guardian_message / map_image';
COMMENT ON COLUMN files.entity_id IS '연결 대상 id. 폴리모픽 연결이라 문자열 사용';
COMMENT ON COLUMN files.storage_path IS '로컬 저장 경로. 확장자로 재생 형식을 판별한다';
COMMENT ON COLUMN files.original_filename IS '원본 파일명';
COMMENT ON COLUMN files.duration_sec IS '영상·음성 길이 초. 이미지는 NULL';
COMMENT ON COLUMN files.uploaded_by IS '업로드한 계정 id (careguard_user). 로봇이 올린 파일은 NULL';
COMMENT ON COLUMN files.retention_until IS '보관 만료 시각. 지난 파일은 정리 작업이 삭제';
COMMENT ON COLUMN files.display_order IS '같은 대상에 여러 파일이 붙을 때 표시 순서';
COMMENT ON COLUMN files.created_at IS '파일 레코드 생성 시간';
COMMENT ON COLUMN files.updated_at IS '파일 레코드 수정 시간';
COMMENT ON COLUMN files.deleted_at IS 'soft delete 시간';


-- =========================================================
-- 4. Map / SLAM
-- =========================================================

CREATE TABLE map (
    id         INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id   INTEGER NOT NULL,
    name       VARCHAR(120) NOT NULL,
    raw_yaml   JSONB NOT NULL,
    resolution DOUBLE PRECISION CHECK (resolution > 0),
    origin_x   DOUBLE PRECISION NOT NULL DEFAULT 0,
    origin_y   DOUBLE PRECISION NOT NULL DEFAULT 0,
    origin_yaw DOUBLE PRECISION NOT NULL DEFAULT 0,
    width_px   INTEGER CHECK (width_px > 0),
    height_px  INTEGER CHECK (height_px > 0),
    is_active  BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE (robot_id, name)
);

CREATE UNIQUE INDEX map_one_active_per_robot_idx ON map (robot_id) WHERE is_active;
CREATE INDEX idx_map_robot_created_at ON map (robot_id, created_at DESC);

COMMENT ON TABLE map IS 'SLAM 맵 메타데이터. 맵 이미지 파일은 files(entity_type=map_image)에 저장';
COMMENT ON COLUMN map.id IS '맵 PK, 자동 증가';
COMMENT ON COLUMN map.robot_id IS '대상 로봇 id';
COMMENT ON COLUMN map.name IS '맵 이름';
COMMENT ON COLUMN map.raw_yaml IS 'map.yaml 내용을 JSONB로 저장';
COMMENT ON COLUMN map.resolution IS '맵 해상도';
COMMENT ON COLUMN map.origin_x IS '맵 원점 x';
COMMENT ON COLUMN map.origin_y IS '맵 원점 y';
COMMENT ON COLUMN map.origin_yaw IS '맵 원점 yaw';
COMMENT ON COLUMN map.width_px IS '맵 이미지 가로 픽셀';
COMMENT ON COLUMN map.height_px IS '맵 이미지 세로 픽셀';
COMMENT ON COLUMN map.is_active IS '현재 사용 중인 맵 여부. 로봇당 1개만 TRUE';
COMMENT ON COLUMN map.created_at IS '맵 생성 시간';

CREATE TABLE slam_job (
    id           INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id     INTEGER NOT NULL,
    map_id       INTEGER,
    map_name     VARCHAR(120) NOT NULL,
    slam_backend VARCHAR(32),
    status       VARCHAR(32) NOT NULL CHECK (
        status IN ('queued', 'running', 'saved', 'failed', 'cancelled')
    ),
    started_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    ended_at     TIMESTAMPTZ
);

CREATE INDEX idx_slam_job_robot_started_at ON slam_job (robot_id, started_at DESC);

COMMENT ON TABLE slam_job IS 'SLAM 매핑 작업 단위. 최초 1회 자택 매핑에 사용';
COMMENT ON COLUMN slam_job.id IS 'SLAM 작업 PK, 자동 증가';
COMMENT ON COLUMN slam_job.robot_id IS '대상 로봇 id';
COMMENT ON COLUMN slam_job.map_id IS '저장된 맵 id';
COMMENT ON COLUMN slam_job.map_name IS '저장할 맵 이름';
COMMENT ON COLUMN slam_job.slam_backend IS 'SLAM 백엔드';
COMMENT ON COLUMN slam_job.status IS 'SLAM 작업 상태';
COMMENT ON COLUMN slam_job.started_at IS '작업 시작 시간';
COMMENT ON COLUMN slam_job.ended_at IS '작업 종료 시간';


-- =========================================================
-- 5. Waypoint / Patrol Route (경로+순서 병합) / Patrol Schedule
--    v3: patrol_route_step 제거. steps JSONB 배열로 순서를 담는다
--        (v1의 waypoint_set 방식으로 회귀, waypoint 마스터 테이블은 유지)
-- =========================================================

CREATE TABLE waypoint (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    map_id          INTEGER NOT NULL,
    code            VARCHAR(32) NOT NULL,
    name            VARCHAR(120),
    kind            VARCHAR(16) NOT NULL DEFAULT 'patrol' CHECK (
        kind IN ('patrol', 'charge')
    ),
    pos_x           DOUBLE PRECISION NOT NULL,
    pos_y           DOUBLE PRECISION NOT NULL,
    yaw             DOUBLE PRECISION NOT NULL DEFAULT 0,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deregistered_at TIMESTAMPTZ
);

CREATE UNIQUE INDEX waypoint_active_code_idx
ON waypoint (map_id, code) WHERE deregistered_at IS NULL;

-- 맵당 충전 스테이션은 1개. 자동 출발·복귀 지점이자 초기 위치.
CREATE UNIQUE INDEX waypoint_one_charge_per_map_idx
ON waypoint (map_id) WHERE kind = 'charge' AND deregistered_at IS NULL;

COMMENT ON TABLE waypoint IS '맵 위의 개별 지점. 순찰 지점과 충전 스테이션을 kind로 구분한다';
COMMENT ON COLUMN waypoint.id IS '지점 PK, 자동 증가';
COMMENT ON COLUMN waypoint.map_id IS '소속 맵 id';
COMMENT ON COLUMN waypoint.code IS '지점 코드, 예: WP_01, CHARGE. 맵 안에서 유니크';
COMMENT ON COLUMN waypoint.name IS '지점 표시 이름, 예: 거실';
COMMENT ON COLUMN waypoint.kind IS 'patrol(순찰 지점) / charge(충전 스테이션 = 초기 위치, 경로 순서에 포함하지 않음)';
COMMENT ON COLUMN waypoint.pos_x IS '맵 기준 x 좌표';
COMMENT ON COLUMN waypoint.pos_y IS '맵 기준 y 좌표';
COMMENT ON COLUMN waypoint.yaw IS '도착 시 로봇이 바라볼 방향(라디안)';
COMMENT ON COLUMN waypoint.created_at IS '지점 등록 시간';
COMMENT ON COLUMN waypoint.deregistered_at IS '등록 해제 시간. NULL이면 사용 중';

CREATE TABLE patrol_route (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    map_id          INTEGER NOT NULL,
    name            VARCHAR(120) NOT NULL,
    description     TEXT,
    steps           JSONB NOT NULL DEFAULT '[]'::jsonb,
    created_by      INTEGER,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deregistered_at TIMESTAMPTZ,
    CHECK (jsonb_typeof(steps) = 'array')
);

CREATE UNIQUE INDEX patrol_route_active_name_idx
ON patrol_route (map_id, name) WHERE deregistered_at IS NULL;

COMMENT ON TABLE patrol_route IS '순찰 경로. 방문 순서와 실행 시각(patrol_schedule)을 관리한다. v2의 patrol_route_step을 steps 배열로 병합';
COMMENT ON COLUMN patrol_route.id IS '경로 PK, 자동 증가';
COMMENT ON COLUMN patrol_route.map_id IS '기준 맵 id';
COMMENT ON COLUMN patrol_route.name IS '경로 이름, 예: 오전 확인 경로';
COMMENT ON COLUMN patrol_route.description IS '경로 설명';
COMMENT ON COLUMN patrol_route.steps IS '방문할 waypoint id를 순서대로 담은 JSONB 배열, 예: [12, 15, 9]. 배열의 인덱스 순서가 곧 방문 순서이며, 화면의 드래그 재정렬은 이 배열을 다시 저장하는 동작. charge kind인 waypoint는 넣지 않는다(자동 출발·복귀)';
COMMENT ON COLUMN patrol_route.created_by IS '등록한 계정 id';
COMMENT ON COLUMN patrol_route.created_at IS '경로 등록 시간';
COMMENT ON COLUMN patrol_route.deregistered_at IS '등록 해제 시간. NULL이면 사용 중';

CREATE TABLE patrol_schedule (
    id                      INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    route_id                INTEGER NOT NULL,
    run_time                TIME NOT NULL,
    days_of_week            SMALLINT NOT NULL DEFAULT 127 CHECK (days_of_week BETWEEN 1 AND 127),
    -- 이 스케줄이 "복약 전달" 경로(patrol_route.name='medicine')의 특정 회차라면,
    -- 그 근거가 된 medication_schedule.id. 한 경로를 여러 시각(아침/저녁 등)에 걸쳐
    -- 쓸 수 있어 이 매칭은 경로가 아니라 각 스케줄(회차) 단위 속성이다.
    medication_schedule_id  INTEGER,
    created_by              INTEGER,
    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deregistered_at         TIMESTAMPTZ
);

CREATE UNIQUE INDEX patrol_schedule_active_idx
ON patrol_schedule (route_id, run_time) WHERE deregistered_at IS NULL;

CREATE INDEX idx_patrol_schedule_run_time
ON patrol_schedule (run_time) WHERE deregistered_at IS NULL;

COMMENT ON TABLE patrol_schedule IS '경로 실행 예약. 한 경로에 여러 시각을 등록할 수 있다';
COMMENT ON COLUMN patrol_schedule.id IS '스케줄 PK, 자동 증가';
COMMENT ON COLUMN patrol_schedule.route_id IS '실행할 경로 id (patrol_route.id). FK 제약 없음';
COMMENT ON COLUMN patrol_schedule.run_time IS '실행 시각, 예: 10:00';
COMMENT ON COLUMN patrol_schedule.days_of_week IS '요일 비트마스크. 월=1 화=2 수=4 목=8 금=16 토=32 일=64. 평일=31, 매일=127';
COMMENT ON COLUMN patrol_schedule.medication_schedule_id IS '복약 전달 경로의 이 회차가 매칭된 medication_schedule.id. 매칭 안 했으면 NULL. FK 제약 없음';
COMMENT ON COLUMN patrol_schedule.created_by IS '등록한 계정 id';
COMMENT ON COLUMN patrol_schedule.created_at IS '등록 시간';
COMMENT ON COLUMN patrol_schedule.deregistered_at IS '등록 해제 시간. NULL이면 사용 중';


-- =========================================================
-- 6. Recipient / Guardian
-- =========================================================

CREATE TABLE recipient (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    code            VARCHAR(16) NOT NULL,
    robot_id        INTEGER,
    operator_id     INTEGER,
    name            VARCHAR(64) NOT NULL,
    gender          VARCHAR(8) CHECK (gender IS NULL OR gender IN ('male', 'female', 'other')),
    birth_date      DATE,
    home_address    TEXT,
    home_lat        DOUBLE PRECISION CHECK (home_lat IS NULL OR home_lat BETWEEN -90 AND 90),
    home_lng        DOUBLE PRECISION CHECK (home_lng IS NULL OR home_lng BETWEEN -180 AND 180),
    diseases        TEXT,
    note            TEXT,
    photo_file_id   INTEGER,
    face_registered BOOLEAN NOT NULL DEFAULT FALSE,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deregistered_at TIMESTAMPTZ
);

CREATE UNIQUE INDEX recipient_active_code_idx
ON recipient (code) WHERE deregistered_at IS NULL;

CREATE INDEX idx_recipient_robot ON recipient (robot_id) WHERE deregistered_at IS NULL;

CREATE INDEX idx_recipient_operator ON recipient (operator_id) WHERE deregistered_at IS NULL;

COMMENT ON TABLE recipient IS '돌봄 대상자. 컬럼 구성은 위험 상황 문자 항목(시나리오서 §6.3)과 1:1 대응한다';
COMMENT ON COLUMN recipient.id IS '대상자 PK, 자동 증가';
COMMENT ON COLUMN recipient.code IS '대상자 코드, 예: C-002';
COMMENT ON COLUMN recipient.robot_id IS '담당 로봇 id. 미배정 상태는 NULL';
COMMENT ON COLUMN recipient.operator_id IS '담당 계정 id (careguard_user.id, role=operator). FK 제약 없음 — 조회 시 애플리케이션에서 JOIN. NULL이면 미배정. 담당 로봇은 robot_id로 함께 조회';
COMMENT ON COLUMN recipient.name IS '이름 (문자 항목)';
COMMENT ON COLUMN recipient.gender IS '성별 (문자 항목)';
COMMENT ON COLUMN recipient.birth_date IS '생년월일. 나이는 조회 시 계산해 문자에 포함';
COMMENT ON COLUMN recipient.home_address IS '집 주소 (문자 항목)';
COMMENT ON COLUMN recipient.home_lat IS '집 위치 위도 (문자 항목)';
COMMENT ON COLUMN recipient.home_lng IS '집 위치 경도 (문자 항목)';
COMMENT ON COLUMN recipient.diseases IS '평소 지병 (문자 항목)';
COMMENT ON COLUMN recipient.note IS '담당자 참고 메모';
COMMENT ON COLUMN recipient.photo_file_id IS '프로필 사진 files.id';
COMMENT ON COLUMN recipient.face_registered IS '얼굴 정보 등록 완료 여부. FALSE면 미등록 인물 대조가 동작하지 않는다';
COMMENT ON COLUMN recipient.created_at IS '대상자 등록 시간';
COMMENT ON COLUMN recipient.deregistered_at IS '등록 해제 시간. NULL이면 사용 중';

CREATE TABLE guardian (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    recipient_id    INTEGER NOT NULL,
    user_id         INTEGER,
    name            VARCHAR(64) NOT NULL,
    phone           VARCHAR(32) NOT NULL,
    relation        VARCHAR(32),
    priority        SMALLINT NOT NULL DEFAULT 1 CHECK (priority >= 1),
    notify_enabled  BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deregistered_at TIMESTAMPTZ
);

CREATE INDEX idx_guardian_recipient_priority
ON guardian (recipient_id, priority) WHERE deregistered_at IS NULL;

COMMENT ON TABLE guardian IS '보호자. 대상자 1명에 여러 명 등록 가능';
COMMENT ON COLUMN guardian.id IS '보호자 PK, 자동 증가';
COMMENT ON COLUMN guardian.recipient_id IS '대상자 id (recipient.id). FK 제약 없음';
COMMENT ON COLUMN guardian.user_id IS '보호자 본인의 로그인 계정 id (careguard_user, role=guardian). 문자만 받고 대시보드는 안 쓰는 보호자는 NULL. FK 제약 없음';
COMMENT ON COLUMN guardian.name IS '보호자 이름 (문자 항목)';
COMMENT ON COLUMN guardian.phone IS '보호자 연락처 (문자 발송 대상)';
COMMENT ON COLUMN guardian.relation IS '관계, 예: 자녀 / 배우자 / 요양보호사';
COMMENT ON COLUMN guardian.priority IS '연락 우선순위. 1이 최우선';
COMMENT ON COLUMN guardian.notify_enabled IS '자동 문자 수신 여부';
COMMENT ON COLUMN guardian.created_at IS '보호자 등록 시간';
COMMENT ON COLUMN guardian.deregistered_at IS '등록 해제 시간. NULL이면 사용 중';


-- =========================================================
-- 7. Call Button (침실 등 고정 위치 응급 호출 버튼)
-- =========================================================

CREATE TABLE call_button (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    code            VARCHAR(32) NOT NULL,
    map_id          INTEGER NOT NULL,
    waypoint_id     INTEGER,
    room_name       VARCHAR(120) NOT NULL,
    pos_x           DOUBLE PRECISION,
    pos_y           DOUBLE PRECISION,
    yaw             DOUBLE PRECISION,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deregistered_at TIMESTAMPTZ
);

CREATE UNIQUE INDEX call_button_active_code_idx
ON call_button (code) WHERE deregistered_at IS NULL;

CREATE INDEX idx_call_button_map ON call_button (map_id) WHERE deregistered_at IS NULL;

COMMENT ON TABLE call_button IS '침실 등 고정 위치의 응급 호출 버튼. 유선 전원, ESP32 + MQTT. 누르면 care_event(SOS_BUTTON)가 생성되고 로봇이 해당 위치로 출동한다. 누름 이력은 care_event가 그대로 담당한다';
COMMENT ON COLUMN call_button.id IS '호출 버튼 PK, 자동 증가';
COMMENT ON COLUMN call_button.code IS '버튼 코드. MQTT 토픽 cg/v1/button/{code}/press 의 키이자 장치 식별자, 예: BTN_BEDROOM';
COMMENT ON COLUMN call_button.map_id IS '설치된 맵 id. 담당 로봇·대상자는 맵을 통해 역참조한다';
COMMENT ON COLUMN call_button.waypoint_id IS '출동 목표 지점. 등록된 지점을 재사용할 때 사용하며, 없으면 아래 좌표로 직접 이동. FK 제약 없음';
COMMENT ON COLUMN call_button.room_name IS '설치 장소 이름, 예: 침실';
COMMENT ON COLUMN call_button.pos_x IS '맵 기준 x 좌표. waypoint_id가 있으면 참고용';
COMMENT ON COLUMN call_button.pos_y IS '맵 기준 y 좌표';
COMMENT ON COLUMN call_button.yaw IS '도착 시 로봇이 바라볼 방향(라디안)';
COMMENT ON COLUMN call_button.created_at IS '버튼 등록 시간';
COMMENT ON COLUMN call_button.deregistered_at IS '등록 해제 시간. NULL이면 사용 중';


-- =========================================================
-- 8. Wellness Policy (시스템 전역 단일 정책)
-- =========================================================

CREATE TABLE wellness_policy (
    id                     SMALLINT PRIMARY KEY DEFAULT 1 CHECK (id = 1),
    question_count         SMALLINT NOT NULL DEFAULT 2 CHECK (question_count BETWEEN 1 AND 5),
    answer_wait_sec        SMALLINT NOT NULL DEFAULT 10 CHECK (answer_wait_sec BETWEEN 5 AND 60),
    absence_attention_runs SMALLINT NOT NULL DEFAULT 3 CHECK (absence_attention_runs >= 1),
    absence_emergency_runs SMALLINT NOT NULL DEFAULT 5 CHECK (absence_emergency_runs >= 1),
    med_missed_streak      SMALLINT NOT NULL DEFAULT 2 CHECK (med_missed_streak >= 1),
    med_wait_sec           SMALLINT NOT NULL DEFAULT 180 CHECK (med_wait_sec BETWEEN 30 AND 900),
    updated_by             INTEGER,
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CHECK (absence_emergency_runs >= absence_attention_runs)
);

INSERT INTO wellness_policy (id) VALUES (1);

COMMENT ON TABLE wellness_policy IS '안부 확인·부재 감지 전역 정책. 모든 대상자에게 동일하게 적용되며 항상 1행만 존재한다(id=1 고정). 시스템 설정 화면에서 편집';
COMMENT ON COLUMN wellness_policy.id IS '고정 PK. 항상 1';
COMMENT ON COLUMN wellness_policy.question_count IS '안부 질문 반복 횟수. 기본 2회';
COMMENT ON COLUMN wellness_policy.answer_wait_sec IS '질문 1회당 응답 대기 초. 기본 10초';
COMMENT ON COLUMN wellness_policy.absence_attention_runs IS 'ATTENTION 승격 기준. 연속 미검출 순찰 회차 수. 기본 3';
COMMENT ON COLUMN wellness_policy.absence_emergency_runs IS 'EMERGENCY 승격 기준. 연속 미검출 순찰 회차 수. 기본 5';
COMMENT ON COLUMN wellness_policy.med_missed_streak IS 'EMERGENCY 승격 기준. 복약 연속 미수령 횟수. 기본 2';
COMMENT ON COLUMN wellness_policy.med_wait_sec IS '복약 안내 후 수령 대기 초. 기본 180초(3분)';
COMMENT ON COLUMN wellness_policy.updated_by IS '최종 수정 계정 id';
COMMENT ON COLUMN wellness_policy.updated_at IS '최종 수정 시간';


-- =========================================================
-- 9. Patrol Run / Wellness Check Log  (순찰 로그)
-- =========================================================

CREATE TABLE patrol_run (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id        INTEGER NOT NULL,
    recipient_id    INTEGER,
    route_id        INTEGER,
    schedule_id     INTEGER,
    trigger         VARCHAR(16) NOT NULL DEFAULT 'schedule' CHECK (
        trigger IN ('schedule', 'manual', 'recheck')
    ),
    status          VARCHAR(16) NOT NULL CHECK (
        status IN ('running', 'completed', 'aborted', 'failed')
    ),
    waypoints_total INTEGER NOT NULL DEFAULT 0 CHECK (waypoints_total >= 0),
    waypoints_done  INTEGER NOT NULL DEFAULT 0 CHECK (waypoints_done >= 0),
    started_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    ended_at        TIMESTAMPTZ
);

CREATE INDEX idx_patrol_run_recipient_started_at
ON patrol_run (recipient_id, started_at DESC);

CREATE INDEX idx_patrol_run_robot_started_at
ON patrol_run (robot_id, started_at DESC);

COMMENT ON TABLE patrol_run IS '순찰 1회차 로그. 부재 감지(경로 C)의 누적 단위. 이 회차에서 사람을 봤는지는 wellness_check_log를 집계해 판단한다';
COMMENT ON COLUMN patrol_run.id IS '순찰 회차 PK, 자동 증가';
COMMENT ON COLUMN patrol_run.robot_id IS '수행 로봇 id';
COMMENT ON COLUMN patrol_run.recipient_id IS '대상자 id. FK 제약 없음';
COMMENT ON COLUMN patrol_run.route_id IS '사용한 순찰 경로 id (patrol_route)';
COMMENT ON COLUMN patrol_run.schedule_id IS '이 회차를 발생시킨 예약 id (patrol_schedule). 수동 실행은 NULL';
COMMENT ON COLUMN patrol_run.trigger IS 'schedule(예약) / manual(담당자 지시) / recheck(재확인 요청)';
COMMENT ON COLUMN patrol_run.status IS '순찰 회차 상태';
COMMENT ON COLUMN patrol_run.waypoints_total IS '계획된 지점 수';
COMMENT ON COLUMN patrol_run.waypoints_done IS '실제 방문한 지점 수';
COMMENT ON COLUMN patrol_run.started_at IS '순찰 시작 시간';
COMMENT ON COLUMN patrol_run.ended_at IS '순찰 종료 시간';

CREATE TABLE wellness_check_log (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    patrol_run_id   INTEGER,
    robot_id        INTEGER NOT NULL,
    recipient_id    INTEGER,
    waypoint_id     INTEGER,
    person_detected BOOLEAN NOT NULL DEFAULT FALSE,
    face_matched    BOOLEAN NOT NULL DEFAULT FALSE,
    track_id        VARCHAR(64),
    question_count  SMALLINT NOT NULL DEFAULT 0 CHECK (question_count >= 0),
    response_kind   VARCHAR(16) NOT NULL DEFAULT 'none' CHECK (
        response_kind IN ('normal', 'help', 'unclear', 'none', 'not_asked')
    ),
    response_input  VARCHAR(16) CHECK (
        response_input IS NULL OR response_input IN ('voice', 'touch')
    ),
    wait_sec        SMALLINT CHECK (wait_sec IS NULL OR wait_sec >= 0),
    care_event_id   INTEGER,
    checked_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_wellness_check_log_recipient_checked_at
ON wellness_check_log (recipient_id, checked_at DESC);

CREATE INDEX idx_wellness_check_log_patrol_run
ON wellness_check_log (patrol_run_id, checked_at);

COMMENT ON TABLE wellness_check_log IS '순찰 지점별 사람 검출 및 안부 확인 결과. 경로 B의 판정 근거이자 경로 C(부재)의 누적 원본. 부재는 이 테이블에 person_detected=FALSE 행으로 남는다';
COMMENT ON COLUMN wellness_check_log.id IS 'PK, 자동 증가';
COMMENT ON COLUMN wellness_check_log.patrol_run_id IS '소속 순찰 회차 id';
COMMENT ON COLUMN wellness_check_log.robot_id IS '수행 로봇 id';
COMMENT ON COLUMN wellness_check_log.recipient_id IS '대상자 id. FK 제약 없음';
COMMENT ON COLUMN wellness_check_log.waypoint_id IS '확인이 이루어진 지점 id';
COMMENT ON COLUMN wellness_check_log.person_detected IS '해당 지점에서 사람을 검출했는지. FALSE가 순찰 전 구간에 이어지면 부재로 누적';
COMMENT ON COLUMN wellness_check_log.face_matched IS '검출 인물이 등록 거주자와 일치했는지. person_detected=TRUE인데 FALSE면 침입 분기';
COMMENT ON COLUMN wellness_check_log.track_id IS '검출 추적 id. object_detection.tracking_id와 대응';
COMMENT ON COLUMN wellness_check_log.question_count IS '실제 수행한 안부 질문 횟수';
COMMENT ON COLUMN wellness_check_log.response_kind IS 'normal(정상) / help(도움 요청) / unclear(불명확) / none(무응답) / not_asked(사람 미검출로 질문 안 함)';
COMMENT ON COLUMN wellness_check_log.response_input IS '응답 수단. 음성 인식 실패 대비 터치 병행';
COMMENT ON COLUMN wellness_check_log.wait_sec IS '총 응답 대기 초';
COMMENT ON COLUMN wellness_check_log.care_event_id IS '이 확인으로 생성된 care_event id. 정상 응답이면 NULL';
COMMENT ON COLUMN wellness_check_log.checked_at IS '확인 수행 시간';


-- =========================================================
-- 10. Medication
-- =========================================================

CREATE TABLE medication_schedule (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    recipient_id    INTEGER NOT NULL,
    slot_no         SMALLINT NOT NULL CHECK (slot_no BETWEEN 1 AND 3),
    drug_name       VARCHAR(120) NOT NULL,
    dose_time       TIME NOT NULL,
    days_of_week    SMALLINT NOT NULL DEFAULT 127 CHECK (days_of_week BETWEEN 1 AND 127),
    created_by      INTEGER,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deregistered_at TIMESTAMPTZ
);

CREATE UNIQUE INDEX medication_schedule_active_slot_time_idx
ON medication_schedule (recipient_id, slot_no, dose_time) WHERE deregistered_at IS NULL;

CREATE INDEX idx_medication_schedule_recipient_time
ON medication_schedule (recipient_id, dose_time) WHERE deregistered_at IS NULL;

COMMENT ON TABLE medication_schedule IS '대상자별 복약 스케줄. 서버가 원본을 보관하고 로봇에 동기화하며, 로봇은 자체 시계로 실행한다(관제 단절 시에도 복약 수행). 사용 중지는 deregistered_at으로 처리';
COMMENT ON COLUMN medication_schedule.id IS '복약 스케줄 PK, 자동 증가';
COMMENT ON COLUMN medication_schedule.recipient_id IS '대상자 id. FK 제약 없음';
COMMENT ON COLUMN medication_schedule.slot_no IS '약통 칸 번호. 1=아침 2=점심 3=저녁 (3칸 구성)';
COMMENT ON COLUMN medication_schedule.drug_name IS '약 이름 (위험 문자 항목)';
COMMENT ON COLUMN medication_schedule.dose_time IS '복용 시각 (위험 문자 항목)';
COMMENT ON COLUMN medication_schedule.days_of_week IS '요일 비트마스크. 월=1 화=2 수=4 목=8 금=16 토=32 일=64. 매일=127';
COMMENT ON COLUMN medication_schedule.created_by IS '등록한 계정 id';
COMMENT ON COLUMN medication_schedule.created_at IS '등록 시간';
COMMENT ON COLUMN medication_schedule.deregistered_at IS '등록 해제 시간. NULL이면 사용 중';

CREATE TABLE medication_log (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    schedule_id   INTEGER,
    recipient_id  INTEGER NOT NULL,
    robot_id      INTEGER NOT NULL,
    mission_id    INTEGER,
    slot_no       SMALLINT CHECK (slot_no IS NULL OR slot_no BETWEEN 1 AND 3),
    announced_at  TIMESTAMPTZ,
    delivered_at  TIMESTAMPTZ,
    result        VARCHAR(16) NOT NULL CHECK (
        result IN ('delivered', 'missed', 'not_found', 'error', 'skipped')
    ),
    confirmed_by  VARCHAR(16) CHECK (
        confirmed_by IS NULL OR confirmed_by IN ('sensor', 'operator')
    ),
    care_event_id INTEGER,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_medication_log_recipient_created_at
ON medication_log (recipient_id, created_at DESC);

CREATE INDEX idx_medication_log_result
ON medication_log (recipient_id, result, created_at DESC);

COMMENT ON TABLE medication_log IS '복약 안내·전달 결과 1건. 연속 미수령 판정(경로 C)의 원본. 예정 시각과 약 이름은 schedule_id로 조회한다';
COMMENT ON COLUMN medication_log.id IS '복약 기록 PK, 자동 증가';
COMMENT ON COLUMN medication_log.schedule_id IS '근거 스케줄 id. 약 이름·예정 시각은 여기서 조회. FK 제약 없음';
COMMENT ON COLUMN medication_log.recipient_id IS '대상자 id. FK 제약 없음';
COMMENT ON COLUMN medication_log.robot_id IS '수행 로봇 id';
COMMENT ON COLUMN medication_log.mission_id IS '수행 미션 id';
COMMENT ON COLUMN medication_log.slot_no IS '실제 개방한 약통 칸 번호';
COMMENT ON COLUMN medication_log.announced_at IS '음성 안내 시각';
COMMENT ON COLUMN medication_log.delivered_at IS '수령 확정 시각. 미수령이면 NULL';
COMMENT ON COLUMN medication_log.result IS 'delivered(수령) / missed(대기 초과) / not_found(대상자 미발견) / error(배출 오류) / skipped(재고 부족 등)';
COMMENT ON COLUMN medication_log.confirmed_by IS '수령 확인 주체. sensor=리드스위치+로드셀, operator=담당자 수동 확인';
COMMENT ON COLUMN medication_log.care_event_id IS '미수령으로 생성된 care_event id';
COMMENT ON COLUMN medication_log.created_at IS '복약 로그 시작 시간. 로봇이 복약 미션을 개시한 시점';


-- =========================================================
-- 11. Care Event / Care Action / Notification
-- =========================================================

CREATE TABLE care_event (
    id              INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    event_uid       VARCHAR(64) NOT NULL UNIQUE,
    seq             BIGINT,
    recipient_id    INTEGER,
    robot_id        INTEGER,
    patrol_run_id   INTEGER,
    trigger_path    VARCHAR(8) NOT NULL CHECK (
        trigger_path IN ('A', 'B', 'C', 'SYS')
    ),
    event_type      VARCHAR(32) NOT NULL CHECK (
        event_type IN (
            'SOS_BUTTON',            -- 경로 A: 침실 응급 버튼
            'SOS_SCREEN',            -- 경로 A: LIMO LCD 도와주세요
            'SOS_VOICE',             -- 경로 A: 음성 도움 요청
            'WELLNESS_NO_RESPONSE',  -- 경로 B: 안부 질문 무응답
            'ABSENCE',               -- 경로 C: 순찰 연속 미검출
            'MED_MISSED',            -- 복약 미수령 1건
            'MED_MISSED_STREAK',     -- 경로 C: 복약 연속 미수령
            'MED_ERROR',             -- 약통 배출 오류/재고 부족
            'INTRUDER',              -- 미등록 인물 검출
            'NAV_FAILED',
            'BATTERY_LOW',
            'ROBOT_OFFLINE',
            'ESTOP'
        )
    ),
    severity        VARCHAR(16) NOT NULL CHECK (
        severity IN ('info', 'attention', 'emergency')
    ),
    status          VARCHAR(16) NOT NULL DEFAULT 'open' CHECK (
        status IN ('open', 'acknowledged', 'resolved', 'cancelled')
    ),
    summary         TEXT NOT NULL,
    evidence        JSONB NOT NULL DEFAULT '{}'::jsonb,
    waypoint_id     INTEGER,
    location_name   VARCHAR(120),
    pos_x           DOUBLE PRECISION,
    pos_y           DOUBLE PRECISION,
    occurred_at     TIMESTAMPTZ NOT NULL,
    ingested_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    resolved_at     TIMESTAMPTZ,
    resolved_by     INTEGER,
    CHECK (jsonb_typeof(evidence) = 'object')
);

CREATE INDEX idx_care_event_open
ON care_event (severity, occurred_at DESC) WHERE status IN ('open', 'acknowledged');

CREATE INDEX idx_care_event_recipient_occurred_at
ON care_event (recipient_id, occurred_at DESC);

CREATE INDEX idx_care_event_robot_occurred_at
ON care_event (robot_id, occurred_at DESC);

CREATE INDEX idx_care_event_evidence ON care_event USING GIN (evidence);

COMMENT ON TABLE care_event IS '돌봄 이벤트. 대시보드 이벤트 큐의 원본이자 담당자 처리 대상. 호출 버튼 누름도 SOS_BUTTON 행으로 여기에 남는다(어떤 버튼인지는 evidence 또는 waypoint_id/location_name으로 확인). 로봇·서버 자체 장애 로그는 system_event가 담당한다';
COMMENT ON COLUMN care_event.id IS '이벤트 내부 PK, 자동 증가';
COMMENT ON COLUMN care_event.event_uid IS '로봇·장치가 채번한 전역 고유 id, 예: EV-R01-260828-0014. 오프라인 재전송 시 ON CONFLICT (event_uid) DO NOTHING으로 중복을 흡수한다';
COMMENT ON COLUMN care_event.seq IS '로봇별 단조 증가 시퀀스. 누락 구간 탐지에 사용';
COMMENT ON COLUMN care_event.recipient_id IS '대상자 id. 로봇 자체 장애 이벤트는 NULL. FK 제약 없음';
COMMENT ON COLUMN care_event.robot_id IS '로봇 id';
COMMENT ON COLUMN care_event.patrol_run_id IS '순찰 중 발생한 이벤트의 소속 순찰 회차 id';
COMMENT ON COLUMN care_event.trigger_path IS 'A(능동 SOS) / B(안부 확인) / C(부재·무활동) / SYS(로봇 장애)';
COMMENT ON COLUMN care_event.event_type IS '이벤트 종류';
COMMENT ON COLUMN care_event.severity IS 'info / attention / emergency. emergency는 일괄 완료 처리 대상에서 제외한다';
COMMENT ON COLUMN care_event.status IS 'open(미처리) / acknowledged(담당자 확인) / resolved(완료) / cancelled(오접수 취소). 최초 확인 시각·주체는 care_action(action_type=acknowledge) 이력으로 조회';
COMMENT ON COLUMN care_event.summary IS '담당자가 그대로 읽는 판단 근거 문장. 점수가 아니라 관측 사실을 쓴다';
COMMENT ON COLUMN care_event.evidence IS '근거 상세 JSON, 예: {"question_count":2,"wait_s":20,"absence_runs":3}';
COMMENT ON COLUMN care_event.waypoint_id IS '발생 지점 id';
COMMENT ON COLUMN care_event.location_name IS '발생 장소 표시명, 예: 거실 입구';
COMMENT ON COLUMN care_event.pos_x IS '발생 위치 x';
COMMENT ON COLUMN care_event.pos_y IS '발생 위치 y';
COMMENT ON COLUMN care_event.occurred_at IS '로봇·장치 기준 실제 발생 시각';
COMMENT ON COLUMN care_event.ingested_at IS '서버 수신 시각. occurred_at과 크게 벌어지면 오프라인 구간이었음을 뜻한다';
COMMENT ON COLUMN care_event.resolved_at IS '처리 완료 시각';
COMMENT ON COLUMN care_event.resolved_by IS '완료 처리한 계정 id';

CREATE TABLE care_action (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    care_event_id INTEGER NOT NULL,
    user_id       INTEGER,
    action_type   VARCHAR(32) NOT NULL CHECK (
        action_type IN (
            'acknowledge',
            'view_video',
            'call_recipient',
            'call_guardian',
            'notify_guardian',
            'report_119',
            'report_112',
            'dispatch_robot',
            'med_reannounce',
            'request_visit',
            'escalate',
            'resolve',
            'cancel'
        )
    ),
    result        VARCHAR(16) CHECK (
        result IS NULL OR result IN ('success', 'no_answer', 'failed', 'n/a')
    ),
    memo          TEXT,
    acted_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_care_action_event_acted_at
ON care_action (care_event_id, acted_at);

COMMENT ON TABLE care_action IS '이벤트에 대한 담당자 조치 이력. care_event 1행에 care_action 여러 행이 시간순으로 쌓인다. care_event는 무슨 일이 있었나, care_action은 사람이 무엇을 했나를 기록한다. 최초 확인(acknowledge) 시각·주체도 이 테이블에서 조회한다';
COMMENT ON COLUMN care_action.id IS '조치 PK, 자동 증가';
COMMENT ON COLUMN care_action.care_event_id IS '대상 이벤트 id. FK 제약 없음';
COMMENT ON COLUMN care_action.user_id IS '조치한 계정 id';
COMMENT ON COLUMN care_action.action_type IS '조치 종류. report_119/report_112는 담당자 확인 후에만 수행하며 자동 발생하지 않는다';
COMMENT ON COLUMN care_action.result IS '조치 결과. 통화 시도의 no_answer 등';
COMMENT ON COLUMN care_action.memo IS '담당자 메모';
COMMENT ON COLUMN care_action.acted_at IS '조치 시각';

CREATE TABLE notification_log (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    care_event_id INTEGER,
    recipient_id  INTEGER,
    guardian_id   INTEGER,
    channel       VARCHAR(16) NOT NULL CHECK (
        channel IN ('sms', 'dashboard', 'call')
    ),
    template_code VARCHAR(32),
    body          TEXT,
    status        VARCHAR(16) NOT NULL DEFAULT 'queued' CHECK (
        status IN ('queued', 'sent', 'failed', 'skipped')
    ),
    retry_count   SMALLINT NOT NULL DEFAULT 0 CHECK (retry_count >= 0),
    sent_at       TIMESTAMPTZ,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_notification_log_event_created_at
ON notification_log (care_event_id, created_at DESC);

CREATE INDEX idx_notification_log_status
ON notification_log (status, created_at) WHERE status IN ('queued', 'failed');

COMMENT ON TABLE notification_log IS '보호자 알림 발송 이력. 중복 발송 방지와 재시도 관리에 사용. 수신 번호는 guardian_id로 조회한다';
COMMENT ON COLUMN notification_log.id IS '발송 PK, 자동 증가';
COMMENT ON COLUMN notification_log.care_event_id IS '근거 이벤트 id';
COMMENT ON COLUMN notification_log.recipient_id IS '대상자 id';
COMMENT ON COLUMN notification_log.guardian_id IS '수신 보호자 id';
COMMENT ON COLUMN notification_log.channel IS '발송 수단';
COMMENT ON COLUMN notification_log.template_code IS '문자 템플릿 코드, 예: EMERG_NO_RESPONSE / MED_MISSED / INTRUDER';
COMMENT ON COLUMN notification_log.body IS '실제 발송 본문. 이름·성별·나이·시각·GPS·지병·약·보호자 항목이 채워진 결과';
COMMENT ON COLUMN notification_log.status IS '발송 상태';
COMMENT ON COLUMN notification_log.retry_count IS '재시도 횟수';
COMMENT ON COLUMN notification_log.sent_at IS '발송 완료 시각';
COMMENT ON COLUMN notification_log.created_at IS '발송 요청 생성 시각';


-- =========================================================
-- 12. Mission / Guardian Message
-- =========================================================

CREATE TABLE mission (
    id                 INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id           INTEGER,
    recipient_id       INTEGER,
    kind               VARCHAR(16) NOT NULL CHECK (
        kind IN ('patrol', 'medication', 'message', 'response', 'recheck', 'return_home')
    ),
    care_event_id      INTEGER,
    schedule_id        INTEGER,
    patrol_run_id      INTEGER,
    target_waypoint_id INTEGER,
    is_priority        BOOLEAN NOT NULL DEFAULT FALSE,
    status             VARCHAR(32) NOT NULL CHECK (
        status IN (
            'queued', 'dispatched', 'navigating', 'replanning',
            'arrived', 'acting', 'completed', 'cancelled', 'failed'
        )
    ),
    created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    dispatched_at      TIMESTAMPTZ,
    arrived_at         TIMESTAMPTZ,
    ended_at           TIMESTAMPTZ,
    fail_reason        VARCHAR(120)
);

CREATE INDEX idx_mission_status_created_at ON mission (status, created_at DESC);
CREATE INDEX idx_mission_robot_created_at ON mission (robot_id, created_at DESC);
CREATE INDEX idx_mission_recipient_created_at ON mission (recipient_id, created_at DESC);

COMMENT ON TABLE mission IS '로봇 수행 단위. 순찰·복약·메시지 재생·호출 응답을 모두 포함한다';
COMMENT ON COLUMN mission.id IS '미션 PK, 자동 증가. 화면에 표시되는 미션 번호';
COMMENT ON COLUMN mission.robot_id IS '배정 로봇 id. 미배정이면 NULL';
COMMENT ON COLUMN mission.recipient_id IS '대상자 id. FK 제약 없음';
COMMENT ON COLUMN mission.kind IS 'patrol(순찰) / medication(복약) / message(보호자 메시지 재생) / response(응급 호출 응답) / recheck(재확인) / return_home(충전 복귀)';
COMMENT ON COLUMN mission.care_event_id IS '이 미션을 유발한 이벤트 id. 호출 버튼 정보는 이 이벤트를 통해 조회';
COMMENT ON COLUMN mission.schedule_id IS 'kind=medication이면 복약 스케줄 id, kind=patrol이면 순찰 스케줄 id';
COMMENT ON COLUMN mission.patrol_run_id IS 'kind=patrol인 경우 생성된 순찰 회차 id';
COMMENT ON COLUMN mission.target_waypoint_id IS '목표 지점 id';
COMMENT ON COLUMN mission.is_priority IS '우선 미션 여부. 응급 호출 응답은 진행 중 순찰·복약을 보류시키고 선점한다';
COMMENT ON COLUMN mission.status IS '미션 상태. acting은 도착 후 안내·질의·재생을 수행 중인 상태';
COMMENT ON COLUMN mission.created_at IS '미션 생성 시간';
COMMENT ON COLUMN mission.dispatched_at IS '로봇 출발 시간';
COMMENT ON COLUMN mission.arrived_at IS '목표 도착 시간';
COMMENT ON COLUMN mission.ended_at IS '종료 시간';
COMMENT ON COLUMN mission.fail_reason IS '실패 사유';

CREATE TABLE guardian_message (
    id           INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    recipient_id INTEGER NOT NULL,
    guardian_id  INTEGER,
    kind         VARCHAR(8) NOT NULL CHECK (kind IN ('video', 'audio')),
    title        VARCHAR(120),
    file_id      INTEGER NOT NULL,
    scheduled_at TIMESTAMPTZ NOT NULL,
    status       VARCHAR(16) NOT NULL DEFAULT 'scheduled' CHECK (
        status IN ('scheduled', 'played', 'not_played', 'cancelled')
    ),
    mission_id   INTEGER,
    played_at    TIMESTAMPTZ,
    retry_count  SMALLINT NOT NULL DEFAULT 0 CHECK (retry_count >= 0),
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_guardian_message_scheduled
ON guardian_message (scheduled_at) WHERE status = 'scheduled';

CREATE INDEX idx_guardian_message_recipient_created_at
ON guardian_message (recipient_id, created_at DESC);

COMMENT ON TABLE guardian_message IS '보호자가 등록한 영상·음성 메시지와 재생 예약';
COMMENT ON COLUMN guardian_message.id IS 'PK, 자동 증가';
COMMENT ON COLUMN guardian_message.recipient_id IS '재생 대상 대상자 id. FK 제약 없음';
COMMENT ON COLUMN guardian_message.guardian_id IS '등록한 보호자 id. FK 제약 없음';
COMMENT ON COLUMN guardian_message.kind IS '메시지 종류';
COMMENT ON COLUMN guardian_message.title IS '메시지 제목';
COMMENT ON COLUMN guardian_message.file_id IS '업로드 파일 files.id';
COMMENT ON COLUMN guardian_message.scheduled_at IS '재생 예정 시각';
COMMENT ON COLUMN guardian_message.status IS 'scheduled(예약) / played(재생 완료) / not_played(대상자 미발견) / cancelled(취소)';
COMMENT ON COLUMN guardian_message.mission_id IS '재생을 수행한 미션 id';
COMMENT ON COLUMN guardian_message.played_at IS '실제 재생 시각';
COMMENT ON COLUMN guardian_message.retry_count IS '대상자 미발견으로 재시도한 횟수';
COMMENT ON COLUMN guardian_message.created_at IS '등록 시간';


-- =========================================================
-- 13. Vision / Recording
-- =========================================================

CREATE TABLE camera_recording (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id      INTEGER NOT NULL,
    care_event_id INTEGER,
    file_id       INTEGER,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at    TIMESTAMPTZ
);

CREATE INDEX idx_camera_recording_event ON camera_recording (care_event_id, created_at DESC);
CREATE INDEX idx_camera_recording_robot_created_at ON camera_recording (robot_id, created_at DESC);

COMMENT ON TABLE camera_recording IS '이벤트 전후 녹화 클립. 파일 실체와 경로는 files가 관리하고 이 테이블은 이벤트-파일 연결과 보존 상태만 담당한다';
COMMENT ON COLUMN camera_recording.id IS '녹화 PK, 자동 증가';
COMMENT ON COLUMN camera_recording.robot_id IS '촬영 로봇 id';
COMMENT ON COLUMN camera_recording.care_event_id IS '연관 이벤트 id';
COMMENT ON COLUMN camera_recording.file_id IS '업로드된 파일 files.id. 로봇 로컬에만 있고 아직 업로드 전이면 NULL';
COMMENT ON COLUMN camera_recording.created_at IS '녹화 레코드 생성 시간';
COMMENT ON COLUMN camera_recording.deleted_at IS '삭제 시간. NULL이면 보존 중';

CREATE TABLE object_detection (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id      INTEGER NOT NULL,
    patrol_run_id INTEGER,
    care_event_id INTEGER,
    recipient_id  INTEGER,
    label         VARCHAR(64) NOT NULL,
    is_registered BOOLEAN,
    confidence    REAL NOT NULL CHECK (confidence BETWEEN 0 AND 1),
    match_score   REAL CHECK (match_score IS NULL OR match_score BETWEEN 0 AND 1),
    bbox_x        INTEGER NOT NULL CHECK (bbox_x >= 0),
    bbox_y        INTEGER NOT NULL CHECK (bbox_y >= 0),
    bbox_width    INTEGER NOT NULL CHECK (bbox_width > 0),
    bbox_height   INTEGER NOT NULL CHECK (bbox_height > 0),
    tracking_id   VARCHAR(64),
    detected_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_object_detection_robot_detected_at
ON object_detection (robot_id, detected_at DESC);

CREATE INDEX idx_object_detection_patrol_run
ON object_detection (patrol_run_id, detected_at);

COMMENT ON TABLE object_detection IS '사람 검출 및 등록 거주자 대조 결과. 자세는 기록하지 않으며 사람의 유무와 등록 여부만 다룬다';
COMMENT ON COLUMN object_detection.id IS '검출 결과 PK, 자동 증가';
COMMENT ON COLUMN object_detection.robot_id IS '대상 로봇 id';
COMMENT ON COLUMN object_detection.patrol_run_id IS '연관 순찰 회차 id';
COMMENT ON COLUMN object_detection.care_event_id IS '이 검출로 생성된 이벤트 id';
COMMENT ON COLUMN object_detection.recipient_id IS '등록 거주자로 판별된 경우 대상자 id';
COMMENT ON COLUMN object_detection.label IS '검출 라벨. 사람은 person';
COMMENT ON COLUMN object_detection.is_registered IS 'TRUE=등록 거주자, FALSE=미등록 인물(침입 분기), NULL=대조 불가';
COMMENT ON COLUMN object_detection.confidence IS '검출 신뢰도';
COMMENT ON COLUMN object_detection.match_score IS '얼굴 대조 유사도';
COMMENT ON COLUMN object_detection.bbox_x IS '바운딩 박스 x';
COMMENT ON COLUMN object_detection.bbox_y IS '바운딩 박스 y';
COMMENT ON COLUMN object_detection.bbox_width IS '바운딩 박스 너비';
COMMENT ON COLUMN object_detection.bbox_height IS '바운딩 박스 높이';
COMMENT ON COLUMN object_detection.tracking_id IS '추적 id. wellness_check_log.track_id와 대응';
COMMENT ON COLUMN object_detection.detected_at IS '검출 시각';

CREATE TABLE driving_session (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id      INTEGER NOT NULL,
    file_id       INTEGER,
    video_path    TEXT,
    started_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    ended_at      TIMESTAMPTZ
);

CREATE INDEX idx_driving_session_robot_started_at
ON driving_session (robot_id, started_at DESC);

COMMENT ON TABLE driving_session IS '주행 영상 녹화 세션. 사후 검토 시 해당 구간 영상을 불러온다';
COMMENT ON COLUMN driving_session.id IS 'PK, 자동 증가';
COMMENT ON COLUMN driving_session.robot_id IS '대상 로봇 id';
COMMENT ON COLUMN driving_session.file_id IS '영상 파일 files.id. 정식 경로는 files.storage_path';
COMMENT ON COLUMN driving_session.video_path IS '영상 직접 경로. 아직 files에 등록되지 않은 로봇 로컬 파일을 가리킬 때 사용';
COMMENT ON COLUMN driving_session.started_at IS '녹화 시작 시간';
COMMENT ON COLUMN driving_session.ended_at IS '녹화 종료 시간';


-- =========================================================
-- 14. Navigation / System Logs
-- =========================================================

CREATE TABLE nav_goal_log (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id      INTEGER NOT NULL,
    mission_id    INTEGER,
    patrol_run_id INTEGER,
    waypoint_id   INTEGER,
    seq_no        INTEGER CHECK (seq_no IS NULL OR seq_no >= 0),
    start_x       DOUBLE PRECISION,
    start_y       DOUBLE PRECISION,
    start_yaw     DOUBLE PRECISION,
    goal_x        DOUBLE PRECISION NOT NULL,
    goal_y        DOUBLE PRECISION NOT NULL,
    goal_yaw      DOUBLE PRECISION NOT NULL DEFAULT 0,
    retry_count   SMALLINT NOT NULL DEFAULT 0 CHECK (retry_count >= 0),
    status        VARCHAR(32) NOT NULL CHECK (
        status IN ('queued', 'accepted', 'navigating', 'arrived', 'failed', 'cancelled')
    ),
    sent_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    finished_at   TIMESTAMPTZ
);

CREATE INDEX idx_nav_goal_log_robot_sent_at ON nav_goal_log (robot_id, sent_at DESC);
CREATE INDEX idx_nav_goal_log_mission ON nav_goal_log (mission_id, sent_at);

COMMENT ON TABLE nav_goal_log IS 'Nav2 목표 전송 및 결과 로그';
COMMENT ON COLUMN nav_goal_log.id IS 'PK, 자동 증가';
COMMENT ON COLUMN nav_goal_log.robot_id IS '대상 로봇 id';
COMMENT ON COLUMN nav_goal_log.mission_id IS '연관 미션 id';
COMMENT ON COLUMN nav_goal_log.patrol_run_id IS '연관 순찰 회차 id';
COMMENT ON COLUMN nav_goal_log.waypoint_id IS '목표 지점 id';
COMMENT ON COLUMN nav_goal_log.seq_no IS '경로 내 순서 번호';
COMMENT ON COLUMN nav_goal_log.start_x IS '전송 시점 시작 x';
COMMENT ON COLUMN nav_goal_log.start_y IS '전송 시점 시작 y';
COMMENT ON COLUMN nav_goal_log.start_yaw IS '전송 시점 시작 yaw';
COMMENT ON COLUMN nav_goal_log.goal_x IS '목표 x';
COMMENT ON COLUMN nav_goal_log.goal_y IS '목표 y';
COMMENT ON COLUMN nav_goal_log.goal_yaw IS '목표 yaw';
COMMENT ON COLUMN nav_goal_log.retry_count IS '재시도 횟수. 2회 실패 시 NAV_FAILED 이벤트 생성';
COMMENT ON COLUMN nav_goal_log.status IS '목표 상태';
COMMENT ON COLUMN nav_goal_log.sent_at IS '목표 전송 시간';
COMMENT ON COLUMN nav_goal_log.finished_at IS '목표 종료 시간';

CREATE TABLE system_event (
    id         INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    robot_id   INTEGER,
    event_type VARCHAR(64) NOT NULL,
    severity   VARCHAR(16) NOT NULL CHECK (
        severity IN ('info', 'warn', 'error', 'critical')
    ),
    message    TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_system_event_robot_created_at
ON system_event (robot_id, created_at DESC);

COMMENT ON TABLE system_event IS '로봇·서버 시스템 로그. 돌봄 이벤트(care_event)와 분리한다. 예: node_crash, disk_full, mqtt_reconnect';
COMMENT ON COLUMN system_event.id IS 'PK, 자동 증가';
COMMENT ON COLUMN system_event.robot_id IS '대상 로봇 id. 서버 자체 이벤트는 NULL';
COMMENT ON COLUMN system_event.event_type IS '이벤트 종류';
COMMENT ON COLUMN system_event.severity IS '심각도';
COMMENT ON COLUMN system_event.message IS '이벤트 메시지';
COMMENT ON COLUMN system_event.created_at IS '이벤트 발생 시간';


-- =========================================================
-- 15. User Action Log (대시보드 조작 감사 로그)
-- =========================================================

CREATE TABLE user_action_log (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id       INTEGER,
    action_type   VARCHAR(40) NOT NULL CHECK (
        action_type IN (
            'login', 'login_failed', 'login_locked', 'logout',
            'estop', 'estop_release',
            'robot_command',
            'robot_create', 'robot_update', 'robot_deregister',
            'view_live', 'view_video', 'download_video',
            'recipient_create', 'recipient_update', 'recipient_delete',
            'guardian_create', 'guardian_update', 'guardian_delete',
            'policy_update',
            'medication_schedule_create', 'medication_schedule_update', 'medication_schedule_delete',
            'waypoint_create', 'waypoint_update', 'waypoint_delete',
            'patrol_route_create', 'patrol_route_update', 'patrol_route_delete',
            'patrol_schedule_create', 'patrol_schedule_delete',
            'call_button_register', 'call_button_update', 'call_button_delete',
            'map_save', 'map_activate', 'map_delete',
            'guardian_message_create', 'guardian_message_cancel',
            'account_create', 'account_update', 'account_delete',
            'account_activate', 'account_deactivate', 'password_change',
            'role_update'
        )
    ),
    target_table  VARCHAR(40),
    target_id     INTEGER,
    robot_id      INTEGER,
    recipient_id  INTEGER,
    care_event_id INTEGER,
    payload       JSONB,
    occurred_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT user_action_log_payload_obj_chk CHECK (
        payload IS NULL OR jsonb_typeof(payload) = 'object'
    ),
    CONSTRAINT user_action_log_video_reason_chk CHECK (
        action_type NOT IN ('view_live', 'view_video', 'download_video')
        OR (payload IS NOT NULL AND payload ? 'reason' AND length(trim(payload ->> 'reason')) > 0)
    )
);

CREATE INDEX idx_user_action_log_user_occurred_at
ON user_action_log (user_id, occurred_at DESC);

CREATE INDEX idx_user_action_log_target
ON user_action_log (target_table, target_id, occurred_at DESC);

CREATE INDEX idx_user_action_log_occurred_at
ON user_action_log (occurred_at DESC);

-- 영상 열람 이력만 빠르게 뽑는 부분 인덱스 (개인정보 감사용)
CREATE INDEX idx_user_action_log_video
ON user_action_log (recipient_id, occurred_at DESC)
WHERE action_type IN ('view_live', 'view_video', 'download_video');

COMMENT ON TABLE user_action_log IS '대시보드 조작 감사 로그. 이벤트와 무관한 관리 조작(로그인·계정·설정·대상자 등)과 영상 열람 이력을 기록한다. 영상 열람 계열은 payload.reason(열람 사유)이 필수이며 CHECK 제약으로 강제된다. care_event에 대한 담당자 조치(확인/완료/취소/119·112 신고 등)는 이 테이블에 중복 기록하지 않고 care_action이 유일한 원본으로 담당한다. P7 감사 로그 화면은 이 테이블과 care_action을 시각순으로 UNION해 구성한다';
COMMENT ON COLUMN user_action_log.id IS 'PK, 자동 증가';
COMMENT ON COLUMN user_action_log.user_id IS '조작 계정 id (careguard_user). FK 제약 없음. 로그인 실패로 계정을 특정 못하면 NULL';
COMMENT ON COLUMN user_action_log.action_type IS '조작 종류. login_failed/login_locked는 로그인 실패·5회 잠금(§5-8). view_live(실시간 스트림) / view_video(녹화 재생) / download_video(내려받기)는 영상 열람 계열. 이벤트 처리 조치는 여기 넣지 않고 care_action에 기록';
COMMENT ON COLUMN user_action_log.target_table IS '조작 대상 테이블명, 예: medication_schedule / files. 어느 레코드를 건드렸는지 추적';
COMMENT ON COLUMN user_action_log.target_id IS '조작 대상 레코드 id. 영상 열람이면 files.id 또는 camera_recording.id';
COMMENT ON COLUMN user_action_log.robot_id IS '대상 로봇 id. E-STOP·실시간 영상 열람 시 사용';
COMMENT ON COLUMN user_action_log.recipient_id IS '대상 대상자 id. 대상자별 영상 열람 이력 조회에 사용';
COMMENT ON COLUMN user_action_log.care_event_id IS '연관 이벤트 id. 이벤트와 무관한 조작은 NULL';
COMMENT ON COLUMN user_action_log.payload IS '상세 데이터. 영상 열람이면 {"reason":"...","access_kind":"clip"}, 설정 변경이면 {"before":{...},"after":{...}}';
COMMENT ON COLUMN user_action_log.occurred_at IS '조작 시각';

COMMIT;
