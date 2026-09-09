from typing import List

from fastapi import APIRouter, HTTPException, Query

import psycopg2.extras

from . import media
from ... import db
from ...schemas import (
    MapCreate,
    MapItem,
    SlamJobCreate,
    SlamJobItem,
    SlamJobSaveRequest,
    WaypointCreate,
    WaypointItem,
    WaypointUpdate,
)
from ...ws import robot_ws

router = APIRouter(prefix="/maps", tags=["map"])

# waypoint는 /maps 밑이 아니라 최상위 /waypoints에 둔다.
waypoints_router = APIRouter(prefix="/waypoints", tags=["map"])

# 최초 자택 매핑(SLAM) 작업. 로봇당 여러 번 실행될 수 있어 별도 리소스로 둔다.
slam_jobs_router = APIRouter(prefix="/slam-jobs", tags=["slam"])


def _waypoint_row_to_item(row) -> WaypointItem:
    return WaypointItem(**row)


@router.get("/active", response_model=List[MapItem])
def list_active_maps():
    """로봇별 현재 활성 맵 목록. map.is_active=TRUE는 로봇당 1개만 존재한다(스키마 유니크 인덱스).

    대시보드가 로봇 목록을 별도로 조회하지 않아도 맵 스위처를 바로 그릴 수 있도록
    robot.code와 담당 recipient.name을 함께 내려준다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT
                m.id, m.robot_id, r.code AS robot_code, m.name,
                rc.name AS recipient_name,
                m.resolution, m.origin_x, m.origin_y, m.origin_yaw,
                m.width_px, m.height_px, m.is_active, m.created_at,
                f.id AS image_file_id,
                COALESCE(m.raw_yaml->>'mode', 'trinary') AS mode,
                COALESCE((m.raw_yaml->>'negate')::int, 0) AS negate,
                COALESCE((m.raw_yaml->>'occupied_thresh')::float, 0.65) AS occupied_thresh,
                COALESCE((m.raw_yaml->>'free_thresh')::float, 0.196) AS free_thresh
            FROM map m
            JOIN robot r ON r.id = m.robot_id
            LEFT JOIN recipient rc
                ON rc.robot_id = m.robot_id AND rc.deregistered_at IS NULL
            LEFT JOIN files f
                ON f.entity_type = 'map_image' AND f.entity_id = m.id::text AND f.deleted_at IS NULL
            WHERE m.is_active = TRUE
            ORDER BY r.code
            """
        )
        rows = cur.fetchall()

    items = []
    for row in rows:
        image_file_id = row["image_file_id"]
        items.append(
            MapItem(
                **row,
                image_url=f"/media/files/{image_file_id}" if image_file_id else None,
            )
        )
    return items


@router.post("", response_model=MapItem, status_code=201)
def create_map(payload: MapCreate):
    """"파일로 맵 등록" 모달 — POST /media/upload-map으로 이미 올려둔 파일(file_id)을
    맵으로 확정한다. SLAM 저장(ws/robot_ws.py::_save_slam_result)과 흐름이 같다
    (기존 활성 맵 비활성화 -> map 행 생성 -> files.entity_id 재연결) — slam_job이
    없다는 것과, 로봇이 아니라 대시보드가 직접 부른다는 점만 다르다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (payload.robot_code,))
        robot = cur.fetchone()
        if robot is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
        robot_id = robot["id"]

        cur.execute("UPDATE map SET is_active = FALSE WHERE robot_id = %s", (robot_id,))
        cur.execute(
            """
            INSERT INTO map (robot_id, name, raw_yaml, resolution, origin_x, origin_y, origin_yaw,
                              width_px, height_px, is_active)
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, TRUE)
            ON CONFLICT (robot_id, name) DO UPDATE SET
                raw_yaml = EXCLUDED.raw_yaml,
                resolution = EXCLUDED.resolution,
                origin_x = EXCLUDED.origin_x,
                origin_y = EXCLUDED.origin_y,
                origin_yaw = EXCLUDED.origin_yaw,
                width_px = EXCLUDED.width_px,
                height_px = EXCLUDED.height_px,
                is_active = TRUE,
                created_at = NOW()
            RETURNING id
            """,
            (
                robot_id,
                payload.map_name,
                psycopg2.extras.Json(payload.raw_yaml),
                payload.resolution,
                payload.origin_x,
                payload.origin_y,
                payload.origin_yaw,
                payload.width_px,
                payload.height_px,
            ),
        )
        # ws/robot_ws.py::_save_slam_result와 같은 이유의 ON CONFLICT — map(robot_id, name)
        # UNIQUE 제약 때문에, 같은 이름으로 다시 등록하면(예: SLAM 저장 크래시로 남은
        # .pgm+.yaml을 이 엔드포인트로 복구하는 경우) 예전엔 UniqueViolation으로 그냥
        # 실패했다. map.id가 안 바뀌어야 기존 waypoint들이 계속 유효해 기존 행을 덮어쓴다.
        map_id = cur.fetchone()["id"]

        # 같은 map_id에 이미 연결된 예전 이미지 파일이 있으면 "현재 이미지" 후보에서
        # 빼야 아래 SELECT의 LEFT JOIN files가 중복 행을 안 만든다 — 실제 파일은 지우지
        # 않는다(soft delete만, robot_ws.py의 같은 처리와 동일한 이유).
        cur.execute(
            "UPDATE files SET deleted_at = NOW() "
            "WHERE entity_type = 'map_image' AND entity_id = %s AND id != %s AND deleted_at IS NULL",
            (str(map_id), payload.file_id),
        )
        cur.execute(
            "UPDATE files SET entity_id = %s WHERE id = %s AND entity_type = 'map_image'",
            (str(map_id), payload.file_id),
        )
        conn.commit()

        cur.execute(
            """
            SELECT
                m.id, m.robot_id, r.code AS robot_code, m.name,
                rc.name AS recipient_name,
                m.resolution, m.origin_x, m.origin_y, m.origin_yaw,
                m.width_px, m.height_px, m.is_active, m.created_at,
                f.id AS image_file_id,
                COALESCE(m.raw_yaml->>'mode', 'trinary') AS mode,
                COALESCE((m.raw_yaml->>'negate')::int, 0) AS negate,
                COALESCE((m.raw_yaml->>'occupied_thresh')::float, 0.65) AS occupied_thresh,
                COALESCE((m.raw_yaml->>'free_thresh')::float, 0.196) AS free_thresh
            FROM map m
            JOIN robot r ON r.id = m.robot_id
            LEFT JOIN recipient rc
                ON rc.robot_id = m.robot_id AND rc.deregistered_at IS NULL
            LEFT JOIN files f
                ON f.entity_type = 'map_image' AND f.entity_id = m.id::text AND f.deleted_at IS NULL
            WHERE m.id = %s
            """,
            (map_id,),
        )
        row = cur.fetchone()

    # .pgm 옆에 실제 .yaml도 남겨야 나중에 "파일로 등록"으로 이 폴더에서 다시
    # 불러올 수 있다 — DB 컬럼에만 있으면 등록 화면이 요구하는 .yaml이 실제로
    # 없어서 불가능하다.
    media.write_map_yaml_sidecar(payload.file_id, payload.raw_yaml)

    image_file_id = row["image_file_id"]
    return MapItem(**row, image_url=f"/media/files/{image_file_id}" if image_file_id else None)


@waypoints_router.get("", response_model=List[WaypointItem])
def list_waypoints(map_id: int = Query(..., description="대상 맵 id")):
    """맵 위 활성 웨이포인트 목록. 순찰 지점(kind=patrol)과 충전 스테이션(kind=charge)을 모두 포함한다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT id, map_id, code, name, kind, pos_x, pos_y, yaw, created_at
            FROM waypoint
            WHERE map_id = %s AND deregistered_at IS NULL
            ORDER BY id
            """,
            (map_id,),
        )
        rows = cur.fetchall()

    return [WaypointItem(**row) for row in rows]


@waypoints_router.post("", response_model=WaypointItem, status_code=201)
def create_waypoint(payload: WaypointCreate):
    """웨이포인트 생성. 코드 중복, 충전 스테이션(맵당 1개) 중복은 사전 체크 후 409로 막는다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT id FROM waypoint WHERE map_id = %s AND code = %s AND deregistered_at IS NULL",
            (payload.map_id, payload.code),
        )
        if cur.fetchone():
            raise HTTPException(status_code=409, detail="이미 같은 코드의 웨이포인트가 있습니다")

        if payload.kind == "charge":
            cur.execute(
                "SELECT id FROM waypoint WHERE map_id = %s AND kind = 'charge' AND deregistered_at IS NULL",
                (payload.map_id,),
            )
            if cur.fetchone():
                raise HTTPException(status_code=409, detail="충전 스테이션은 맵당 1개만 등록할 수 있습니다")

        cur.execute(
            """
            INSERT INTO waypoint (map_id, code, name, kind, pos_x, pos_y, yaw)
            VALUES (%s, %s, %s, %s, %s, %s, %s)
            RETURNING id, map_id, code, name, kind, pos_x, pos_y, yaw, created_at
            """,
            (payload.map_id, payload.code, payload.name, payload.kind, payload.pos_x, payload.pos_y, payload.yaw),
        )
        row = cur.fetchone()
        conn.commit()

    return _waypoint_row_to_item(row)


@waypoints_router.put("/{waypoint_id}", response_model=WaypointItem)
def update_waypoint(waypoint_id: int, payload: WaypointUpdate):
    """부분 수정. 보낸 필드만 반영한다."""
    fields = payload.dict(exclude_unset=True)
    if not fields:
        raise HTTPException(status_code=400, detail="변경할 내용이 없습니다")

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT map_id, kind FROM waypoint WHERE id = %s AND deregistered_at IS NULL",
            (waypoint_id,),
        )
        current = cur.fetchone()
        if current is None:
            raise HTTPException(status_code=404, detail="웨이포인트를 찾을 수 없습니다")

        if fields.get("kind") == "charge" and current["kind"] != "charge":
            cur.execute(
                """
                SELECT id FROM waypoint
                WHERE map_id = %s AND kind = 'charge' AND deregistered_at IS NULL AND id != %s
                """,
                (current["map_id"], waypoint_id),
            )
            if cur.fetchone():
                raise HTTPException(status_code=409, detail="충전 스테이션은 맵당 1개만 등록할 수 있습니다")

        sets = ", ".join(f"{key} = %s" for key in fields)
        params = list(fields.values()) + [waypoint_id]
        cur.execute(
            f"""
            UPDATE waypoint SET {sets}
            WHERE id = %s
            RETURNING id, map_id, code, name, kind, pos_x, pos_y, yaw, created_at
            """,
            params,
        )
        row = cur.fetchone()
        conn.commit()

    return _waypoint_row_to_item(row)


@waypoints_router.delete("/{waypoint_id}", status_code=204)
def delete_waypoint(waypoint_id: int):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "UPDATE waypoint SET deregistered_at = NOW() WHERE id = %s AND deregistered_at IS NULL",
            (waypoint_id,),
        )
        if cur.rowcount == 0:
            conn.rollback()
            raise HTTPException(status_code=404, detail="웨이포인트를 찾을 수 없습니다")
        conn.commit()


@slam_jobs_router.post("", response_model=SlamJobItem, status_code=201)
async def start_slam_job(payload: SlamJobCreate):
    """최초 자택 매핑을 시작한다. DB에 slam_job(status=queued)을 남기고 로봇에 명령을 전송한다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (payload.robot_code,))
        robot = cur.fetchone()
        if robot is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")

        cur.execute(
            """
            INSERT INTO slam_job (robot_id, map_name, slam_backend, status)
            VALUES (%s, %s, %s, 'queued')
            RETURNING id, robot_id, map_id, map_name, slam_backend, status, started_at, ended_at
            """,
            (robot["id"], payload.map_name, payload.slam_backend),
        )
        row = cur.fetchone()
        conn.commit()

    connected = await robot_ws.send_command(
        payload.robot_code, {"type": "slam_start", "job_id": row["id"], "map_name": payload.map_name}
    )
    return SlamJobItem(**row, robot_connected=connected)


@slam_jobs_router.post("/{job_id}/cancel", response_model=SlamJobItem)
async def cancel_slam_job(job_id: int):
    """진행 중인 SLAM을 취소한다 — "매핑 취소"/"매핑 재시작"(재시작은 취소 후 다시 시작)이 쓴다.

    로봇에는 slam_stop을 보내 downlink_node가 띄워둔 cartographer 프로세스를 죽이게
    한다(안 죽이고 그대로 두면 재시작 시 "이미 SLAM 작업이 진행 중입니다"로 거부됨).
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT sj.id, sj.robot_id, sj.map_id, sj.map_name, sj.slam_backend, sj.status,
                   sj.started_at, sj.ended_at, r.code AS robot_code
            FROM slam_job sj JOIN robot r ON r.id = sj.robot_id
            WHERE sj.id = %s
            """,
            (job_id,),
        )
        row = cur.fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="SLAM 작업을 찾을 수 없습니다")

        cur.execute(
            """
            UPDATE slam_job SET status = 'cancelled', ended_at = NOW() WHERE id = %s
            RETURNING id, robot_id, map_id, map_name, slam_backend, status, started_at, ended_at
            """,
            (job_id,),
        )
        updated = cur.fetchone()
        conn.commit()

    connected = await robot_ws.send_command(row["robot_code"], {"type": "slam_stop", "job_id": job_id})
    return SlamJobItem(**updated, robot_connected=connected)


@slam_jobs_router.post("/{job_id}/save", response_model=SlamJobItem)
async def save_slam_job(job_id: int, payload: SlamJobSaveRequest = SlamJobSaveRequest()):
    """"SLAM 맵 저장·배포" 버튼 — 로봇에 지금까지 그린 지도를 실제로 저장하라고 명령한다.

    payload.map_name이 오면 "매핑 시작" 때 넣어뒀던 임시 이름(예: "새 맵")을 여기서
    실제 이름으로 덮어쓴다 — slam_job.map_name이 유일한 원본이라, 여기서 갱신해두면
    로봇에 보내는 이름도, 나중에 ws/robot_ws.py::_save_slam_result가 map.name으로
    쓰는 값도 자동으로 맞는 이름이 된다(그쪽 코드는 안 건드려도 됨).

    그 외엔 여기서 DB를 안 바꾼다 — 로봇이 map_saver_cli로 저장 + /media/upload-map
    업로드까지 끝내고 slam_saved를 다시 보내오면 그때 _save_slam_result가 map 행을
    만들고 slam_job.status를 'saved'로 바꾼다. 지금 이 요청은 그 과정을 시작만
    시킨다(비동기) — 그래서 응답의 status는 아직 'queued'/'running'일 수 있다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        if payload.map_name:
            cur.execute("UPDATE slam_job SET map_name = %s WHERE id = %s", (payload.map_name, job_id))
            conn.commit()

        cur.execute(
            """
            SELECT sj.id, sj.robot_id, sj.map_id, sj.map_name, sj.slam_backend, sj.status,
                   sj.started_at, sj.ended_at, r.code AS robot_code
            FROM slam_job sj JOIN robot r ON r.id = sj.robot_id
            WHERE sj.id = %s
            """,
            (job_id,),
        )
        row = cur.fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="SLAM 작업을 찾을 수 없습니다")

    connected = await robot_ws.send_command(
        row["robot_code"],
        {
            "type": "save_map",
            "job_id": job_id,
            "map_name": row["map_name"],
        },
    )
    if not connected:
        raise HTTPException(status_code=409, detail="로봇이 연결되어 있지 않습니다")
    return SlamJobItem(**row, robot_connected=connected)


@slam_jobs_router.get("/{job_id}", response_model=SlamJobItem)
def get_slam_job(job_id: int):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT sj.id, sj.robot_id, sj.map_id, sj.map_name, sj.slam_backend, sj.status,
                   sj.started_at, sj.ended_at, r.code AS robot_code
            FROM slam_job sj JOIN robot r ON r.id = sj.robot_id
            WHERE sj.id = %s
            """,
            (job_id,),
        )
        row = cur.fetchone()

    if row is None:
        raise HTTPException(status_code=404, detail="SLAM 작업을 찾을 수 없습니다")

    connected = robot_ws.is_connected(row["robot_code"])
    return SlamJobItem(**{k: v for k, v in row.items() if k != "robot_code"}, robot_connected=connected)
