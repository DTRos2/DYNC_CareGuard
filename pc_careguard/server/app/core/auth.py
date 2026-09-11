# 이 요청을 보낸 사람이 누구인지 확인하는 서버쪽 관문을 모아둔 파일
# 토큰 발급 / 토큰 검증 / 역할 체크
# 다른 API파일들이 require_roles를 갖다 씀 -> 로그인 안 했으면 여기서 막고, 했으면 current_user에 그 사람 정보를 넣어준다를 처리함함
import secrets
from datetime import datetime, timedelta, timezone

from fastapi import Depends, Header, HTTPException

from . import db

SESSION_TTL_HOURS = 12

# [ 토큰 발급 ]
# 로그인이 성공했을 때 호출 
# 랜덤 토큰 만듦 -> careguard_session 테이블에 이 토큰 = 이 user_id로 저장 -> 클라이언트에게 로그인 응답으로 그 토큰 반환 
def create_session(cur, user_id: int) -> str: 
    token = secrets.token_urlsafe(32)
    expires_at = datetime.now(timezone.utc) + timedelta(hours=SESSION_TTL_HOURS)
    cur.execute(
        "INSERT INTO careguard_session (token, user_id, expires_at) VALUES (%s, %s, %s)",
        (token, user_id, expires_at),
    )
    return token


def delete_session(cur, token: str) -> None:
    cur.execute("DELETE FROM careguard_session WHERE token = %s", (token,))


class CurrentUser:
    def __init__(self, id: int, login_id: str, role: str):
        self.id = id
        self.login_id = login_id
        self.role = role  # admin / operator / guardian


# [ 토큰 검증 ]
# 로그인 이후의 모든 API요청이 이 함수 거침
# 요청 헤더에 실려온 토큰을 읽음 -> careguard_session 테이블 가지고 토큰 일치 확인 
#                                   -> 있으면 토큰의 정보(CurremtUser:몇번계정/이름/role) 돌려줌 
#                                   -> 없거나 만료면 401 에러 던짐
def get_current_user(authorization: str = Header(default=None)) -> CurrentUser:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="인증이 필요합니다")
    token = authorization.removeprefix("Bearer ")

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT u.id, u.login_id, r.code AS role
            FROM careguard_session s
            JOIN careguard_user u ON u.id = s.user_id
            JOIN role r ON r.id = u.role_id
            WHERE s.token = %s AND s.expires_at > NOW()
              AND u.deleted_at IS NULL AND u.is_active
            """,
            (token,),
        )
        row = cur.fetchone()

    if row is None:
        raise HTTPException(status_code=401, detail="세션이 만료되었거나 유효하지 않습니다")
    return CurrentUser(id=row["id"], login_id=row["login_id"], role=row["role"])


def require_roles(*roles: str):
    def _dep(user: CurrentUser = Depends(get_current_user)) -> CurrentUser:
        if user.role not in roles:
            raise HTTPException(status_code=403, detail="접근 권한이 없습니다")
        return user
    return _dep