import asyncio

import bcrypt
from fastapi import FastAPI, HTTPException

from . import db
from .accounts import router as accounts_router
from .api.v1 import map as map_api
from .api.v1 import mission as mission_api
from .api.v1 import patrol as patrol_api
from .api.v1 import recipient as recipient_api
from .api.v1 import teleop as teleop_api
from .audit import router as audit_router
from .api.v1.media import router as media_router
from .api.v1.media import video_events_router
from .robots import router as robots_router
from .schemas import LoginRequest, LoginResponse
from .services.scheduler import run_scheduler_loop
from .ws import dashboard_ws, robot_ws

app = FastAPI(title="CareGuard Server")
app.include_router(media_router)
app.include_router(video_events_router)
app.include_router(accounts_router)
app.include_router(map_api.router)
app.include_router(map_api.waypoints_router)
app.include_router(map_api.slam_jobs_router)
app.include_router(patrol_api.routes_router)
app.include_router(patrol_api.schedules_router)
app.include_router(patrol_api.runs_router)
app.include_router(mission_api.router)
app.include_router(recipient_api.router)
app.include_router(teleop_api.router)
app.include_router(robot_ws.router)
app.include_router(dashboard_ws.router)
app.include_router(audit_router)
app.include_router(robots_router)

INVALID_CREDENTIALS = "아이디 또는 비밀번호가 올바르지 않습니다"


@app.on_event("startup")
async def start_background_tasks():
    # patrol_schedule을 주기적으로 확인해 시각이 되면 자동으로 순찰을 시작한다.
    asyncio.create_task(run_scheduler_loop())


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
