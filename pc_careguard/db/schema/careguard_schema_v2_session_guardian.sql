-- v2: 세션 토큰 테이블 + 보호자 계정-대상자 연결 컬럼
-- careguard_schema_v1.sql 이후에 적용되어야 함 (careguard_user, recipient 참조)

CREATE TABLE careguard_session (
    id          INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    token       VARCHAR(64) NOT NULL UNIQUE,
    user_id     INTEGER NOT NULL REFERENCES careguard_user(id), -- ← careguard_user 테이블이 있어야 함
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at  TIMESTAMPTZ NOT NULL
);

CREATE INDEX idx_careguard_session_token ON careguard_session (token);
CREATE INDEX idx_careguard_session_user  ON careguard_session (user_id);

-- 보호자 계정 ↔ 대상자 연결. operator_id와 동일한 패턴.S
ALTER TABLE recipient ADD COLUMN guardian_user_id INTEGER REFERENCES careguard_user(id);  -- ← recipient 테이블이 있어야 함
CREATE INDEX idx_recipient_guardian_user ON recipient (guardian_user_id);