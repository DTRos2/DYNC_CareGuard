from datetime import date, datetime, time
from typing import Any, Dict, List, Optional

from pydantic import BaseModel


class LoginRequest(BaseModel):
    login_id: str
    password: str


class LoginResponse(BaseModel):
    id: int
    login_id: str
    role: str


class AccountItem(BaseModel):
    id: int
    login_id: str
    display_name: str  # careguard_user.display_name (NOT NULL)
    role_code: str  # role.code: admin / operator / guardian
    role_name: str  # role.name (한글 표시용)
    is_active: bool
    last_login_at: Optional[datetime] = None
    charge_summary: Optional[str] = None  # "김순자 R-01 · 박영호 R-02" 형태. 담당 대상자 없으면 None


class RegisterAccountRequest(BaseModel):
    login_id: str
    password: str
    display_name: str
    role_code: str  # admin / operator / guardian
    is_active: bool = True


class RegisterAccountResponse(AccountItem):
    pass


class UpdateAccountRequest(BaseModel):
    display_name: str
    role_code: str  # admin / operator / guardian
    is_active: bool


class ChangePasswordRequest(BaseModel):
    new_password: str


class DeleteAccountsRequest(BaseModel):
    user_ids: List[int]  # 계정 관리 탭에서 체크박스로 선택한 계정 id 목록


class AuditLogEntry(BaseModel):
    occurred_at: datetime
    account: Optional[str] = None  # 조치한 계정의 표시 이름(없으면 로그인 ID, 그마저 없으면 None)
    action: str        # 한글 라벨 — audit.py의 라벨 표에서 변환
    detail: str        # "대상 · 상세" 컬럼에 그대로 표시
    highlight: bool    # 영상 열람·이벤트 완료·E-STOP·119/112 신고 등 책임 추적 대상 여부


class RecipientOption(BaseModel):
    id: int
    code: str
    name: str
    robot_code: Optional[str] = None
    operator_id: Optional[int] = None  # 현재 담당 계정 id. 계정 수정 모달의 담당 배정 체크리스트용


class AssignRecipientsRequest(BaseModel):
    recipient_ids: List[int]  # 이 계정이 담당할 대상자 id 전체 목록(교체 방식)


class MediaUploadResponse(BaseModel):
    file_id: int
    camera_recording_id: int
    storage_path: str
    original_filename: str
    duration_sec: Optional[int] = None


class MapImageUploadResponse(BaseModel):
    file_id: int
    storage_path: str
    original_filename: str


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


class RobotItem(BaseModel):
    # GET /maps/active와 달리 아직 맵이 없는(SLAM 전) 로봇도 포함한다 — 매핑 페이지의
    # 로봇 선택처럼 "맵 없는 로봇"을 골라야 하는 화면이 이 스키마를 쓴다.
    id: int
    code: str  # 예: "R-01"
    name: str
    model: str
    ip: Optional[str] = None
    created_at: datetime


class MapCreate(BaseModel):
    # "파일로 등록" 모달이 쓴다 — SLAM 저장(POST /slam-jobs/{id}/save)과 달리 slam_job이
    # 없으므로 메타데이터(해상도·원점·크기)를 .yaml에서 직접 읽어 그대로 실어 보낸다.
    robot_code: str
    map_name: str
    file_id: int  # POST /media/upload-map 응답의 file_id
    resolution: float
    origin_x: float = 0.0
    origin_y: float = 0.0
    origin_yaw: float = 0.0
    width_px: int
    height_px: int
    raw_yaml: Dict[str, Any] = {}


class MapItem(BaseModel):
    id: int
    robot_id: int
    robot_code: str  # robot.code, 예: "R-01"
    name: str
    recipient_name: Optional[str] = None  # 담당 대상자. 미배정이면 None
    resolution: Optional[float] = None
    origin_x: float
    origin_y: float
    origin_yaw: float
    width_px: Optional[int] = None
    height_px: Optional[int] = None
    is_active: bool
    created_at: datetime
    image_file_id: Optional[int] = None  # files.id (entity_type='map_image')
    image_url: Optional[str] = None  # 기존 /media/files/{file_id} 재사용 — 없으면 None


class WaypointItem(BaseModel):
    id: int
    map_id: int
    code: str  # 예: WP_01, CHARGE
    name: Optional[str] = None
    kind: str  # "patrol" / "charge"
    pos_x: float
    pos_y: float
    yaw: float
    created_at: datetime


class PatrolRouteItem(BaseModel):
    id: int
    map_id: int
    name: str
    description: Optional[str] = None
    steps: List[int] = []  # 방문할 waypoint id를 순서대로 담은 배열
    created_by: Optional[int] = None
    created_at: datetime


class PatrolScheduleItem(BaseModel):
    id: int
    route_id: int
    run_time: time
    days_of_week: int  # 요일 비트마스크. 월=1 화=2 수=4 목=8 금=16 토=32 일=64
    enabled: bool  # deregistered_at IS NULL 이면 True
    created_by: Optional[int] = None
    created_at: datetime


class WaypointCreate(BaseModel):
    map_id: int
    code: str
    name: Optional[str] = None
    kind: str = "patrol"  # "patrol" / "charge"
    pos_x: float
    pos_y: float
    yaw: float = 0.0


class WaypointUpdate(BaseModel):
    name: Optional[str] = None
    kind: Optional[str] = None
    pos_x: Optional[float] = None
    pos_y: Optional[float] = None
    yaw: Optional[float] = None


class PatrolRouteCreate(BaseModel):
    map_id: int
    name: str
    description: Optional[str] = None
    steps: List[int] = []
    created_by: Optional[int] = None


class PatrolRouteUpdate(BaseModel):
    name: Optional[str] = None
    description: Optional[str] = None
    steps: Optional[List[int]] = None


class PatrolScheduleCreate(BaseModel):
    route_id: int
    run_time: time
    days_of_week: int = 127
    created_by: Optional[int] = None


class PatrolScheduleUpdate(BaseModel):
    run_time: Optional[time] = None
    days_of_week: Optional[int] = None
    # DB엔 별도 enabled 컬럼이 없다 — False는 deregistered_at=NOW(), True는 NULL로 되돌린다.
    enabled: Optional[bool] = None


class SlamJobCreate(BaseModel):
    # 아직 맵이 없는(=최초 매핑) 로봇은 대시보드가 robot.id를 알 방법이 없다 — 로봇 목록
    # 조회 API(robots.py)는 이번 스코프 밖이라, 화면에 이미 표시되는 robot.code로 받는다.
    robot_code: str
    map_name: str
    slam_backend: Optional[str] = "slam_toolbox"


class SlamJobSaveRequest(BaseModel):
    # P5 "맵 저장 정보" 카드(맵 이름 입력칸)에서 온 최종 이름 — "매핑 시작" 시점에 넣은
    # 임시 이름(예: "새 맵")을 저장 시점에 덮어쓴다. 안 넘기면 기존 이름 그대로 둔다.
    map_name: Optional[str] = None


class TeleopCommand(BaseModel):
    # P5 방향키 패널이 계산한 목표 속도. 로봇이 그대로 받아 cmd_vel로 퍼블리시한다.
    linear: float = 0.0   # m/s, 전진(+)/후진(-)
    angular: float = 0.0  # rad/s, 좌회전(+)/우회전(-)


class SlamJobItem(BaseModel):
    id: int
    robot_id: int
    map_id: Optional[int] = None
    map_name: str
    slam_backend: Optional[str] = None
    status: str
    started_at: datetime
    ended_at: Optional[datetime] = None
    robot_connected: bool = False  # ws/robot_ws로 명령이 실제 전달됐는지


class PatrolRunCreate(BaseModel):
    robot_id: int
    route_id: int
    trigger: str = "manual"  # "schedule" / "manual" / "recheck"


class DriveToPointRequest(BaseModel):
    # P6 "이동"(단일 웨이포인트 주행) — patrol_run으로 기록하지 않는 일회성 명령이라
    # route_id 없이 좌표를 직접 받는다. GUI가 캔버스에 이미 갖고 있는 값을 그대로 보낸다
    # (patrol_start가 waypoints를 route_id 재조회 없이 좌표로 직접 싣는 것과 같은 이유).
    robot_code: str
    code: str = "AD_HOC"
    x: float
    y: float
    yaw: float = 0.0
    # downlink_node가 Nav2를 자동으로 준비하는 데 필요하다(map.id) — GUI가
    # maps_[currentMapIndex_].serverMapId를 그대로 보낸다.
    map_id: int


class DriveToPointResponse(BaseModel):
    robot_connected: bool


class PrepareNavigationRequest(BaseModel):
    # P6 페이지를 열거나 다른 로봇의 맵으로 전환할 때 — 실제 순찰/이동 목표 없이
    # Nav2(맵 다운로드+런치+초기 위치)만 미리 준비시킨다. "웨이포인트를 보내기
    # 전에도 로봇 현재 위치부터 보고 싶다"는 요청에 대응.
    robot_code: str
    map_id: int


class SetInitialPoseRequest(BaseModel):
    # P6 "위치 보정" — 지도 클릭+드래그로 로봇의 실제 현재 위치·방향을 직접 지정해
    # AMCL 초기 위치를 다시 잡는다. nav_prepare(자동, 충전 스테이션 좌표 가정)가
    # 로봇이 충전소 밖에서 켜졌을 때 틀리는 문제의 수동 보정 경로.
    robot_code: str
    map_id: int
    x: float
    y: float
    yaw: float = 0.0


class ShutdownRobotRequest(BaseModel):
    # P6 "로봇 종료" 버튼 — 베이스 구동·Nav2/SLAM·rviz2·업링크·다운링크를 전부 내린다.
    robot_code: str


class PatrolRunItem(BaseModel):
    id: int
    robot_id: int
    recipient_id: Optional[int] = None
    route_id: Optional[int] = None
    schedule_id: Optional[int] = None
    trigger: str
    status: str
    waypoints_total: int
    waypoints_done: int
    started_at: datetime
    ended_at: Optional[datetime] = None
    robot_connected: bool = False


class MissionCreate(BaseModel):
    robot_code: str
    kind: str = "medication"  # 이번 스코프에선 P3 "복약 재안내" 버튼만 이걸로 미션을 만든다


class MissionItem(BaseModel):
    id: int
    robot_id: int
    kind: str
    status: str
    created_at: datetime
    dispatched_at: Optional[datetime] = None
    robot_connected: bool = False


class GuardianCreate(BaseModel):
    name: str
    phone: str
    relation: Optional[str] = None
    priority: int = 1
    notify_enabled: bool = True


class GuardianItem(BaseModel):
    id: int
    name: str
    phone: str
    relation: Optional[str] = None
    priority: int
    notify_enabled: bool


class MedicationScheduleCreate(BaseModel):
    # medication_schedule.slot_no CHECK(1~3)과 동일한 제약. P3 등록 모달은 UI상 4번째
    # 이상 칸도 만들 수 있어(대상자 상세 모달의 "칸 없으면 계속 증가" 정책과 동일) 그런
    # 값이 오면 recipient.py가 사전에 걸러 422로 막는다.
    slot_no: int
    drug_name: str
    dose_time: time  # "HH:MM"
    days_of_week: int = 127  # 요일 비트마스크. 월=1 화=2 수=4 목=8 금=16 토=32 일=64, 매일=127


class MedicationScheduleItem(BaseModel):
    id: int
    slot_no: int
    drug_name: str
    dose_time: time
    days_of_week: int


class RecipientCreate(BaseModel):
    name: str
    gender: Optional[str] = None  # 'male' / 'female' / 'other'. recipient.gender CHECK와 동일
    birth_date: Optional[date] = None
    home_address: Optional[str] = None
    diseases: Optional[str] = None
    note: Optional[str] = None
    robot_code: Optional[str] = None  # 미배정이면 None
    guardians: List[GuardianCreate] = []
    schedules: List[MedicationScheduleCreate] = []


class RecipientItem(BaseModel):
    id: int
    code: str
    name: str
    robot_id: Optional[int] = None
    robot_code: Optional[str] = None
    gender: Optional[str] = None
    birth_date: Optional[date] = None
    home_address: Optional[str] = None
    diseases: Optional[str] = None
    note: Optional[str] = None
    created_at: datetime


class RecipientDetailItem(RecipientItem):
    guardians: List[GuardianItem] = []
    schedules: List[MedicationScheduleItem] = []
