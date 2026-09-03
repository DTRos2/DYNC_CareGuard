import bcrypt
from fastapi import FastAPI, HTTPException

from . import db
from .media import router as media_router
from .media import video_events_router
from .schemas import LoginRequest, LoginResponse

app = FastAPI(title="CareGuard Server")
app.include_router(media_router)
app.include_router(video_events_router)

INVALID_CREDENTIALS = "아이디 또는 비밀번호가 올바르지 않습니다"


@app.get("/health")
def health():
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1")
    return {"status": "ok"}


@app.post("/auth/login", response_model=LoginResponse)
def login(payload: LoginRequest):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT u.id, u.login_id, u.password_hash, u.is_active, r.code AS role
            FROM careguard_user u
            JOIN role r ON r.id = u.role_id
            WHERE u.login_id = %s AND u.deleted_at IS NULL
            """,
            (payload.login_id,),
        )
        user = cur.fetchone()

        if (
            not user
            or not user["is_active"]
            or not bcrypt.checkpw(payload.password.encode(), user["password_hash"].encode())
        ):
            raise HTTPException(status_code=401, detail=INVALID_CREDENTIALS)

        cur.execute(
            "UPDATE careguard_user SET last_login_at = NOW() WHERE id = %s",
            (user["id"],),
        )
        conn.commit()

    return LoginResponse(id=user["id"], login_id=user["login_id"], role=user["role"])
