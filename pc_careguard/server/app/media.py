import uuid
from collections import defaultdict
from pathlib import Path
from typing import List, Optional

from fastapi import APIRouter, File, Form, HTTPException, Query, UploadFile
from fastapi.responses import FileResponse

from . import config, db
from .schemas import MediaUploadResponse, VideoEventItem, ViewHistoryEntry

router = APIRouter(prefix="/media", tags=["media"])

# video-events는 media 업로드와 같은 파일(camera_recording/files)을 다루지만, URL은
# /media 밑이 아니라 최상위 /video-events로 둔다 (5단계 POST /video-events/{id}/view와
# 경로 체계를 맞추기 위함). 그래서 router를 하나 더 둔다.
video_events_router = APIRouter(prefix="/video-events", tags=["video-events"])

VIDEO_DIR = config.STORAGE_DIR / "videos"

# 한 번에 메모리에 올릴 최대 청크 크기. 영상 파일이 커도 통째로 읽지 않고 이 크기씩 나눠 쓴다.
CHUNK_SIZE = 1024 * 1024

# severity -> (tag_text, tag_style). care_event가 없는 순찰 스냅샷과 severity='info'는
# 구분할 근거 컬럼이 없어서 둘 다 순찰/neutral로 묶는다.
SEVERITY_TAGS = {
    "emergency": ("긴급", "emergency"),
    "attention": ("주의", "attention"),
}
DEFAULT_TAG = ("순찰", "neutral")


@router.post("/upload", response_model=MediaUploadResponse)
def upload_video(
    robot_id: int = Form(..., description="촬영한 로봇 id"),
    care_event_id: Optional[int] = Form(None, description="연동된 이벤트 id. 순찰 스냅샷 등은 비워둔다"),
    duration_sec: Optional[int] = Form(None, description="영상 길이(초)"),
    file: UploadFile = File(..., description="업로드할 영상 파일"),
):
    """이벤트/순찰 녹화 클립을 업로드해 files + camera_recording에 기록한다.

    OpenCV 저장 코드(6단계)가 아직 없어도, 로봇이 로컬에 만든 아무 영상 파일이나
    curl -F로 이 엔드포인트에 넣어서 저장/DB 기록까지 먼저 검증하기 위한 단계.
    """
    if not file.filename:
        raise HTTPException(status_code=400, detail="파일이 비어 있습니다")

    robot_dir = VIDEO_DIR / str(robot_id)
    robot_dir.mkdir(parents=True, exist_ok=True)

    ext = Path(file.filename).suffix
    storage_path = robot_dir / f"{uuid.uuid4().hex}{ext}"

    with storage_path.open("wb") as out:
        while chunk := file.file.read(CHUNK_SIZE):
            out.write(chunk)

    # files.entity_id는 폴리모픽 연결이라 문자열. 이벤트에 연동됐으면 이벤트를, 아니면(순찰
    # 스냅샷 등) 로봇을 가리키게 해서 entity_id가 항상 채워지게 한다.
    entity_id = str(care_event_id) if care_event_id is not None else str(robot_id)

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            INSERT INTO files (entity_type, entity_id, storage_path, original_filename, duration_sec)
            VALUES ('event_clip', %s, %s, %s, %s)
            RETURNING id
            """,
            (entity_id, str(storage_path), file.filename, duration_sec),
        )
        file_id = cur.fetchone()["id"]

        cur.execute(
            """
            INSERT INTO camera_recording (robot_id, care_event_id, file_id)
            VALUES (%s, %s, %s)
            RETURNING id
            """,
            (robot_id, care_event_id, file_id),
        )
        camera_recording_id = cur.fetchone()["id"]

        conn.commit()

    return MediaUploadResponse(
        file_id=file_id,
        camera_recording_id=camera_recording_id,
        storage_path=str(storage_path),
        original_filename=file.filename,
        duration_sec=duration_sec,
    )


@router.api_route("/files/{file_id}", methods=["GET", "HEAD"])
def get_media_file(file_id: int):
    """영상 재생/다운로드용 실제 바이트를 내려준다.

    files.storage_path는 서버 로컬 경로라 대시보드(다른 PC일 수 있음)가 직접 열 수
    없어서, 그 경로를 읽어 HTTP로 스트리밍하는 이 엔드포인트를 거친다. FileResponse가
    Range 요청(구간 탐색)을 자동으로 처리해준다.

    HEAD도 명시적으로 받아야 한다 — @router.get()만 쓰면 이 FastAPI 버전에서는
    include_router로 등록된 라우트에 HEAD가 자동으로 안 붙어서 405가 났었다. QMediaPlayer의
    FFmpeg 백엔드가 재생 전에 HEAD로 한 번 더 확인하는데, 그게 막히면 검은 화면으로 멈춘다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT storage_path, original_filename FROM files WHERE id = %s AND deleted_at IS NULL",
            (file_id,),
        )
        row = cur.fetchone()

    if row is None:
        raise HTTPException(status_code=404, detail="파일을 찾을 수 없습니다")

    path = Path(row["storage_path"])
    if not path.is_file():
        raise HTTPException(status_code=404, detail="저장된 파일이 디스크에 없습니다")

    return FileResponse(
        path,
        filename=row["original_filename"],
        content_disposition_type="inline",
    )


@video_events_router.get("", response_model=List[VideoEventItem])
def list_video_events(
    robot_id: Optional[int] = Query(None, description="특정 로봇으로 필터링"),
    limit: int = Query(50, ge=1, le=200),
    offset: int = Query(0, ge=0),
):
    """영상 카드 목록 조회.

    camera_recording을 기준으로 robot·care_event·recipient·files를 조인한다.
    care_event_id가 NULL인 행(순찰 스냅샷)도 보여야 해서 care_event/recipient는
    LEFT JOIN이고, 이벤트가 없을 때는 robot_id로 담당 recipient를 역조회한다
    (rc_by_robot).

    조회 이력(view_count/view_history)은 user_action_log(action_type='view_video')에서
    가져온다. care_action과 달리 care_event_id가 필수가 아니라서, 순찰 스냅샷도 조회
    기록을 남길 수 있다 — care_action은 이벤트 조치 전용 원본이라 여기 쓰지 않는다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT
                cr.id AS camera_recording_id,
                r.code AS robot_code,
                COALESCE(rc.name, rc_by_robot.name) AS recipient_name,
                ce.id AS care_event_id,
                ce.event_uid AS event_uid,
                ce.severity AS severity,
                ce.summary AS summary,
                ce.location_name AS location,
                cr.created_at AS captured_at,
                f.id AS file_id,
                f.storage_path AS storage_path,
                f.duration_sec AS duration_sec,
                f.retention_until AS retention_until
            FROM camera_recording cr
            JOIN robot r ON r.id = cr.robot_id
            JOIN files f ON f.id = cr.file_id
            LEFT JOIN care_event ce ON ce.id = cr.care_event_id
            LEFT JOIN recipient rc ON rc.id = ce.recipient_id
            LEFT JOIN recipient rc_by_robot
                ON rc_by_robot.robot_id = cr.robot_id AND rc_by_robot.deregistered_at IS NULL
            WHERE cr.deleted_at IS NULL
                AND (%(robot_id)s IS NULL OR cr.robot_id = %(robot_id)s)
            ORDER BY cr.created_at DESC
            LIMIT %(limit)s OFFSET %(offset)s
            """,
            {"robot_id": robot_id, "limit": limit, "offset": offset},
        )
        rows = cur.fetchall()

        history_by_recording = defaultdict(list)
        recording_ids = [row["camera_recording_id"] for row in rows]
        if recording_ids:
            cur.execute(
                """
                SELECT
                    ual.target_id AS camera_recording_id,
                    ual.occurred_at AS occurred_at,
                    u.login_id AS login_id,
                    ual.payload ->> 'reason' AS reason
                FROM user_action_log ual
                LEFT JOIN careguard_user u ON u.id = ual.user_id
                WHERE ual.action_type = 'view_video'
                    AND ual.target_table = 'camera_recording'
                    AND ual.target_id = ANY(%(ids)s)
                ORDER BY ual.occurred_at DESC
                """,
                {"ids": recording_ids},
            )
            for entry in cur.fetchall():
                history_by_recording[entry["camera_recording_id"]].append(
                    ViewHistoryEntry(
                        occurred_at=entry["occurred_at"],
                        login_id=entry["login_id"],
                        reason=entry["reason"],
                    )
                )

    items = []
    for row in rows:
        tag_text, tag_style = SEVERITY_TAGS.get(row["severity"], DEFAULT_TAG)
        history = history_by_recording[row["camera_recording_id"]]
        items.append(
            VideoEventItem(
                camera_recording_id=row["camera_recording_id"],
                robot_code=row["robot_code"],
                recipient_name=row["recipient_name"],
                care_event_id=row["care_event_id"],
                event_uid=row["event_uid"],
                summary=row["summary"],
                tag_text=tag_text,
                tag_style=tag_style,
                captured_at=row["captured_at"],
                location=row["location"],
                file_id=row["file_id"],
                storage_path=row["storage_path"],
                video_url=f"/media/files/{row['file_id']}",
                duration_sec=row["duration_sec"],
                retention_until=row["retention_until"],
                view_count=len(history),
                view_history=history,
            )
        )
    return items
