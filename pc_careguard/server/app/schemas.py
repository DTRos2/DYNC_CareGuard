from datetime import datetime
from typing import List, Optional

from pydantic import BaseModel


class LoginRequest(BaseModel):
    login_id: str
    password: str


class LoginResponse(BaseModel):
    id: int
    login_id: str
    role: str


class MediaUploadResponse(BaseModel):
    file_id: int
    camera_recording_id: int
    storage_path: str
    original_filename: str
    duration_sec: Optional[int] = None


class ViewHistoryEntry(BaseModel):
    occurred_at: datetime
    login_id: Optional[str] = None  # careguard_user.login_id. 계정에 표시 이름 컬럼이 없어 대신 사용
    reason: Optional[str] = None  # user_action_log.payload->>'reason'


class RecordVideoViewRequest(BaseModel):
    login_id: str  # 열람한 계정의 로그인 ID. careguard_user.id로 역조회해 user_action_log.user_id에 남긴다
    reason: str  # 열람 사유. user_action_log CHECK 제약상 공백 제외 1자 이상이어야 한다


class RecordVideoViewResponse(BaseModel):
    view_count: int
    view_history: List[ViewHistoryEntry]  # 방금 기록한 열람 포함, 최신순


class VideoEventItem(BaseModel):
    camera_recording_id: int
    robot_code: str  # robot.code, 예: "R-01"
    recipient_name: Optional[str] = None
    care_event_id: Optional[int] = None
    event_uid: Optional[str] = None
    summary: Optional[str] = None  # care_event.summary. 이벤트 미연동이면 None(=순찰)
    tag_text: str  # "긴급" / "주의" / "순찰"
    tag_style: str  # "emergency" / "attention" / "neutral"
    captured_at: datetime
    location: Optional[str] = None  # care_event.location_name. 이벤트 미연동이면 None
    file_id: int
    storage_path: str  # 서버 로컬 경로. 클라이언트 재생용이 아니라 디버깅/관리용으로만 남겨둠
    video_url: str  # 재생용. 서버 기준 상대 경로("/media/files/{file_id}") — 클라이언트가 serverUrl에 이어붙여 쓴다
    duration_sec: Optional[int] = None
    retention_until: Optional[datetime] = None
    view_count: int = 0
    view_history: List[ViewHistoryEntry] = []
