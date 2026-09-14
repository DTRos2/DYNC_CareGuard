import uuid
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

import anyio.from_thread
import yaml
from fastapi import APIRouter, File, Form, HTTPException, Query, UploadFile
from fastapi.responses import FileResponse
from psycopg2.extras import Json

from ...core import config, db
from ...schemas import (
    MapImageUploadResponse,
    MediaUploadResponse,
    RecordVideoViewRequest,
    RecordVideoViewResponse,
    VideoEventItem,
    ViewHistoryEntry,
)
from ...ws import dashboard_ws

router = APIRouter(prefix="/media", tags=["media"])

# video-events는 media 업로드와 같은 파일(camera_recording/files)을 다루지만, URL은
# /media 밑이 아니라 최상위 /video-events로 둔다 (5단계 POST /video-events/{id}/view와
# 경로 체계를 맞추기 위함). 그래서 router를 하나 더 둔다.
video_events_router = APIRouter(prefix="/video-events", tags=["video-events"])

VIDEO_DIR = config.STORAGE_DIR / "videos"
MAP_DIR = config.STORAGE_DIR / "maps"


def _relative_storage_path(absolute_path: Path) -> str:
    """DB에 저장할 경로를 STORAGE_DIR 기준 상대경로 문자열로 만든다.

    예전엔 절대경로(str(storage_path))를 그대로 저장했는데, 이 서버를 clone한
    계정/경로가 사람마다 달라서(예: /home/ktel/... vs /home/coc/...) 다른 사람
    PC에서 같은 DB를 보면 그 절대경로가 존재하지 않아 파일을 못 찾는 문제가 있었다.
    STORAGE_DIR 기준 상대경로만 저장해두면 어느 머신에서 서버를 띄우든
    config.STORAGE_DIR와 조합해서 항상 올바른 실제 경로를 얻을 수 있다.
    """
    return str(absolute_path.relative_to(config.STORAGE_DIR))


def resolve_storage_path(stored_value: str) -> Path:
    """DB의 files.storage_path 값을 이 머신에서 실제로 열 수 있는 Path로 바꾼다.

    - 상대경로(이 수정 이후 새로 저장된 값)면 그냥 STORAGE_DIR과 합친다.
    - 절대경로(이 수정 전에 다른 머신/계정에서 저장된 레거시 값)면, 그 경로가 이
      머신에도 우연히 그대로 존재하는 경우가 아닌 한 STORAGE_DIR 폴더 이름(기본
      "storage", STORAGE_DIR 환경변수로 다른 이름을 쓰면 그 이름)이 나오는 지점
      다음 부분(예: "maps/R-01/xxx.pgm")만 뽑아 이 머신의 STORAGE_DIR과 다시
      조합해본다 — 폴더 구조(STORAGE_DIR 아래)는 어느 머신이든 동일하므로 이렇게
      복구하면 clone 계정/경로만 다른 레거시 데이터도 코드 수정만으로 다시
      읽힌다. 그래도 못 찾으면(예: 폴더 구조 자체가 다르거나 파일이 진짜 없어진
      경우) 원래 값을 그대로 돌려줘 호출부가 "파일 없음"으로 처리하게 둔다.
    """
    p = Path(stored_value)
    if not p.is_absolute():
        return config.STORAGE_DIR / p
    if p.is_file():
        return p

    marker = config.STORAGE_DIR.name
    parts = p.parts
    if marker in parts:
        idx = len(parts) - 1 - parts[::-1].index(marker)
        candidate = config.STORAGE_DIR.joinpath(*parts[idx + 1 :])
        if candidate.is_file():
            return candidate
    return p


def _heal_legacy_path(file_id: int, stored_value: str, resolved_path: Path) -> None:
    """레거시 절대경로를 성공적으로 찾아낸 김에 DB 값을 상대경로로 정리해둔다.

    팀원마다 자기 컴퓨터에서 서버를 띄우고 있어서 앞으로도 "폴더 구조는 같은데
    계정 홈 디렉터리만 다른" 절대경로가 새로 생길 수 있다. 파일을 실제로 서빙할
    때마다 이렇게 조용히 상대경로로 고쳐두면, 그 이후로는 어느 컴퓨터에서 읽어도
    다시는 이 파일을 위해 resolve_storage_path의 레거시 경로 추정을 안 타도 된다.
    """
    if not Path(stored_value).is_absolute():
        return
    try:
        relative = resolved_path.relative_to(config.STORAGE_DIR)
    except ValueError:
        return
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("UPDATE files SET storage_path = %s WHERE id = %s", (str(relative), file_id))
        conn.commit()


# 한 번에 메모리에 올릴 최대 청크 크기. 영상 파일이 커도 통째로 읽지 않고 이 크기씩 나눠 쓴다.
CHUNK_SIZE = 1024 * 1024

# severity -> (tag_text, tag_style). care_event가 없는 순찰 스냅샷과 severity='info'는
# 구분할 근거 컬럼이 없어서 둘 다 순찰/neutral로 묶는다.
SEVERITY_TAGS = {
    "emergency": ("긴급", "emergency"),
    "attention": ("주의", "attention"),
}
DEFAULT_TAG = ("순찰", "neutral")

# 보관 정책 (RetentionPolicyDialog 안내 문구와 동일): 이벤트에 연동된 영상은 30일,
# 사건과 무관한 일반 영상(순찰 스냅샷 등, care_event_id 없음)은 7일 보관 후 자동 삭제 대상이 된다.
RETENTION_DAYS_EVENT_CLIP = 30
RETENTION_DAYS_PATROL_SNAPSHOT = 7


@router.post("/upload", response_model=MediaUploadResponse)
def upload_video(
    robot_id: Optional[int] = Form(None, description="촬영한 로봇 id. robot_code 대신 이걸 쓸 수도 있다"),
    robot_code: Optional[str] = Form(None, description="촬영한 로봇 code (예: R-01). robot_id 대신 이걸 써도 된다"),
    care_event_id: Optional[int] = Form(None, description="연동된 이벤트 id. 순찰 스냅샷 등은 비워둔다"),
    duration_sec: Optional[int] = Form(None, description="영상 길이(초)"),
    file: UploadFile = File(..., description="업로드할 영상 파일"),
):
    """이벤트/순찰 녹화 클립을 업로드해 files + camera_recording에 기록한다.

    OpenCV 저장 코드(6단계)가 아직 없어도, 로봇이 로컬에 만든 아무 영상 파일이나
    curl -F로 이 엔드포인트에 넣어서 저장/DB 기록까지 먼저 검증하기 위한 단계.

    robot_id 또는 robot_code 중 하나만 있으면 된다(upload-map과 같은 방식).
    둘 다 오면 robot_id를 우선한다.
    """
    if not file.filename:
        raise HTTPException(status_code=400, detail="파일이 비어 있습니다")

    if robot_id is None:
        if not robot_code:
            raise HTTPException(status_code=400, detail="robot_id 또는 robot_code 중 하나는 필요합니다")
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
            robot = cur.fetchone()
        if robot is None:
            raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
        robot_id = robot["id"]

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

    # care_event_id 유무로 보관 기간을 정한다 (RETENTION_DAYS_* 주석 참고).
    retention_days = (
        RETENTION_DAYS_EVENT_CLIP if care_event_id is not None else RETENTION_DAYS_PATROL_SNAPSHOT
    )
    retention_until = datetime.now(timezone.utc) + timedelta(days=retention_days)

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            INSERT INTO files (entity_type, entity_id, storage_path, original_filename, duration_sec, retention_until)
            VALUES ('event_clip', %s, %s, %s, %s, %s)
            RETURNING id
            """,
            (entity_id, _relative_storage_path(storage_path), file.filename, duration_sec, retention_until),
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

    # P4(영상 관리) 화면이 폴링 없이도 새 클립을 바로 받도록 push한다. 이 엔드포인트는
    # 동기(def) 핸들러라 스레드풀에서 실행되므로, robot_ws.py처럼 바로 await하지 못하고
    # anyio.from_thread.run으로 서버 이벤트 루프 스레드에 코루틴 실행을 맡긴다.
    # 페이로드는 최소 식별자만 담고, 실제 목록은 항상 GET /video-events를 다시 불러
    # 채운다(view_count 등은 그때그때 다시 세는 값이라 여기서 흉내내지 않는다).
    anyio.from_thread.run(
        dashboard_ws.broadcast,
        {
            "type": "video_event_created",
            "camera_recording_id": camera_recording_id,
            "robot_id": robot_id,
            "care_event_id": care_event_id,
        },
    )

    return MediaUploadResponse(
        file_id=file_id,
        camera_recording_id=camera_recording_id,
        storage_path=str(storage_path),
        original_filename=file.filename,
        duration_sec=duration_sec,
    )


@router.post("/upload-map", response_model=MapImageUploadResponse)
def upload_map_image(
    robot_code: str = Form(..., description="매핑을 수행한 로봇 code (예: R-04)"),
    file: UploadFile = File(..., description="업로드할 맵 이미지(.pgm 등) 파일"),
    current_user: CurrentUser = Depends(require_roles("admin", "operator")),
):
    """SLAM으로 완성한(또는 파일로 직접 등록하는) 맵 이미지를 업로드해 files에
    entity_type='map_image'로 기록한다.

    /media/upload(영상용)를 재사용하면 entity_type이 'event_clip'으로 고정되고
    camera_recording에도 불필요한 행이 생겨, 이후 map 행 재연결("UPDATE files ...
    WHERE entity_type = 'map_image'")이 아무 행도 못 찾고 조용히 실패한다 — 그래서
    맵 전용 엔드포인트를 따로 둔다.

    다른 곳(POST /slam-jobs 등)과 마찬가지로 robot_id가 아니라 robot_code로 받는다 —
    robots.py(로봇 목록 API)가 이번 스코프 밖이라 대시보드가 robot_id를 알 방법이
    없다. downlink_node.py도 이미 알고 있는 자기 ROBOT_CODE를 그대로 쓰면 되므로
    서버가 알려주는 robot_id를 따로 안 받아도 된다.

    업로드 시점엔 아직 map.id가 없으므로 entity_id는 임시로 robot_id를 담아 두고,
    나중에 map 행이 만들어질 때(_save_slam_result 또는 POST /maps) 그 map.id로
    다시 연결한다.
    """
    if not file.filename:
        raise HTTPException(status_code=400, detail="파일이 비어 있습니다")

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        robot = cur.fetchone()
    if robot is None:
        raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
    robot_id = robot["id"]

    robot_dir = MAP_DIR / robot_code
    robot_dir.mkdir(parents=True, exist_ok=True)

    ext = Path(file.filename).suffix
    storage_path = robot_dir / f"{uuid.uuid4().hex}{ext}"

    with storage_path.open("wb") as out:
        while chunk := file.file.read(CHUNK_SIZE):
            out.write(chunk)

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            INSERT INTO files (entity_type, entity_id, storage_path, original_filename)
            VALUES ('map_image', %s, %s, %s)
            RETURNING id
            """,
            (str(robot_id), _relative_storage_path(storage_path), file.filename),
        )
        file_id = cur.fetchone()["id"]
        conn.commit()

    return MapImageUploadResponse(
        file_id=file_id,
        storage_path=str(storage_path),
        original_filename=file.filename,
    )


def write_map_yaml_sidecar(file_id: int, raw_yaml: Dict[str, Any]) -> None:
    """업로드된 맵 이미지(.pgm) 옆에 실제 .yaml 파일을 같이 남긴다.

    SLAM 저장(ws/robot_ws.py::_save_slam_result)·파일로 등록(api/v1/map.py::
    create_map) 둘 다 지금까지는 .yaml 내용(resolution/origin 등)을 DB 컬럼에만
    남기고 실제 파일은 안 남겼다 — 그래서 나중에 "파일로 등록"으로 이 폴더의
    pgm+yaml 쌍을 다시 불러오고 싶어도 yaml이 없어서 불가능했다(등록엔 .yaml이
    필수). 이 함수가 그 실제 파일을 만든다.

    raw_yaml의 image 필드는 원본 위치(로봇의 임시 경로, 또는 사용자 PC의 로컬
    경로)를 가리키고 있어서 그대로 두면 잘못된 참조가 된다 — 실제로 옆에 저장된
    파일명으로 고쳐 쓴다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT storage_path FROM files WHERE id = %s", (file_id,))
        row = cur.fetchone()
    if row is None:
        return

    pgm_path = resolve_storage_path(row["storage_path"])
    if pgm_path.is_file():
        _heal_legacy_path(file_id, row["storage_path"], pgm_path)
    yaml_path = pgm_path.with_suffix(".yaml")
    content = dict(raw_yaml)
    content["image"] = pgm_path.name
    with yaml_path.open("w") as f:
        yaml.safe_dump(content, f, default_flow_style=None, sort_keys=False)


@router.api_route("/files/{file_id}", methods=["GET", "HEAD"])
def get_media_file(file_id: int, current_user: CurrentUser = Depends(get_current_user)):
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
    
    if current_user.role == "guardian":
            cur.execute(
                """
                SELECT COALESCE(rc.guardian_user_id, rc_by_robot.guardian_user_id) AS owner_id
                FROM camera_recording cr
                LEFT JOIN care_event ce ON ce.id = cr.care_event_id
                LEFT JOIN recipient rc ON rc.id = ce.recipient_id
                LEFT JOIN recipient rc_by_robot
                    ON rc_by_robot.robot_id = cr.robot_id AND rc_by_robot.deregistered_at IS NULL
                WHERE cr.file_id = %s
                """,
                (file_id,),
            )
            owner = cur.fetchone()
            if owner is None or owner["owner_id"] != current_user.id:
                raise HTTPException(status_code=403, detail="접근 권한이 없습니다")

    if row is None:
        raise HTTPException(status_code=404, detail="파일을 찾을 수 없습니다")

    path = resolve_storage_path(row["storage_path"])
    if not path.is_file():
        raise HTTPException(status_code=404, detail="저장된 파일이 디스크에 없습니다")

    _heal_legacy_path(file_id, row["storage_path"], path)

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
    current_user: CurrentUser = Depends(get_current_user),
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
                COALESCE(rc.id, rc_by_robot.id) AS recipient_id,
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
                AND (
                    %(guardian_id)s IS NULL
                    OR COALESCE(rc.guardian_user_id, rc_by_robot.guardian_user_id) = %(guardian_id)s
                )
            ORDER BY cr.created_at DESC
            LIMIT %(limit)s OFFSET %(offset)s
            """,
            {
                "robot_id": robot_id, 
                "limit": limit, 
                "offset": offset,
                "guardian_id": current_user.id if current_user.role == "guardian" else None,
            },
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


@video_events_router.post("/{camera_recording_id}/view", response_model=RecordVideoViewResponse)
def record_video_view(camera_recording_id: int, body: RecordVideoViewRequest):
    """영상 열람 사실을 감사 로그(user_action_log)에 기록한다.

    view_count 전용 컬럼은 없다 — list_video_events()와 마찬가지로 이 테이블을 그때그때
    다시 세는 방식이 유일한 원본이라, 여기서도 INSERT 후 같은 방식으로 다시 세어 돌려준다.
    VideoPlayerDialog가 "사유 확인 후 재생"으로 잠금을 풀 때(unlock()) 이 API를 호출해야 한다.
    """
    reason = body.reason.strip()
    if not reason:
        # user_action_log_video_reason_chk와 동일한 조건. DB가 걸러주기 전에 먼저 400으로 막는다.
        raise HTTPException(status_code=400, detail="열람 사유를 입력해야 합니다")

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT robot_id, care_event_id FROM camera_recording WHERE id = %s AND deleted_at IS NULL",
            (camera_recording_id,),
        )
        recording = cur.fetchone()
        if recording is None:
            raise HTTPException(status_code=404, detail="녹화를 찾을 수 없습니다")

        cur.execute(
            "SELECT id FROM careguard_user WHERE login_id = %s AND deleted_at IS NULL",
            (body.login_id,),
        )
        user = cur.fetchone()
        if user is None:
            raise HTTPException(status_code=404, detail="계정을 찾을 수 없습니다")

        # recipient_id 역조회: list_video_events()의 rc_by_robot과 동일한 규칙
        # (이벤트에 연동됐으면 그 이벤트의 대상자, 아니면 로봇 담당 대상자).
        cur.execute(
            """
            SELECT COALESCE(ce.recipient_id, rc_by_robot.id) AS recipient_id
            FROM camera_recording cr
            LEFT JOIN care_event ce ON ce.id = cr.care_event_id
            LEFT JOIN recipient rc_by_robot
                ON rc_by_robot.robot_id = cr.robot_id AND rc_by_robot.deregistered_at IS NULL
            WHERE cr.id = %s
            """,
            (camera_recording_id,),
        )
        recipient_id = cur.fetchone()["recipient_id"]

        cur.execute(
            """
            INSERT INTO user_action_log
                (user_id, action_type, target_table, target_id, robot_id, recipient_id, care_event_id, payload)
            VALUES (%s, 'view_video', 'camera_recording', %s, %s, %s, %s, %s)
            """,
            (
                user["id"],
                camera_recording_id,
                recording["robot_id"],
                recipient_id,
                recording["care_event_id"],
                Json({"reason": reason}),
            ),
        )

        # 방금 기록한 것까지 포함해 다시 센 view_count/view_history를 그대로 응답으로 돌려준다.
        # list_video_events()가 매번 다시 계산하는 방식과 맞춰서, 클라이언트가 이 응답만으로
        # 화면을 최신 상태로 갱신할 수 있게 한다.
        cur.execute(
            """
            SELECT ual.occurred_at, u.login_id, ual.payload ->> 'reason' AS reason
            FROM user_action_log ual
            LEFT JOIN careguard_user u ON u.id = ual.user_id
            WHERE ual.action_type = 'view_video'
                AND ual.target_table = 'camera_recording'
                AND ual.target_id = %s
            ORDER BY ual.occurred_at DESC
            """,
            (camera_recording_id,),
        )
        history = [
            ViewHistoryEntry(occurred_at=row["occurred_at"], login_id=row["login_id"], reason=row["reason"])
            for row in cur.fetchall()
        ]

        conn.commit()
        dashboard_ws.broadcast_threadsafe({"type": "audit_log_created"})

    return RecordVideoViewResponse(view_count=len(history), view_history=history)
