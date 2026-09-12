from datetime import date, datetime, time
from typing import Any, Dict, List, Optional

from pydantic import BaseModel, Field


class LoginRequest(BaseModel):
    login_id: str
    password: str


class LoginResponse(BaseModel):
    id: int
    login_id: str
    role: str
    token: str


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
    guardian_user_id: Optional[int] = None  # 추가 — 현재 담당 보호자 계정 id

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
    # map.raw_yaml(원본 .yaml 전체)에서 뽑아낸 값 — 로봇의 downlink_node._download_map이
    # Nav2용 .yaml을 다시 만들 때 이 세 값을 map_saver_cli 기본값으로 하드코딩해 왔다
    # ("파일로 등록" 지도가 다른 값을 쓰던 원본이면 Nav2가 점유/자유 공간을 잘못
    # 해석하는 원인). raw_yaml에 키가 없는 옛날 행을 위해 map_saver_cli 기본값으로
    # 폴백한다(list_active_maps의 COALESCE와 값을 맞춰야 한다).
    mode: str = "trinary"
    negate: int = 0
    occupied_thresh: float = 0.65
    free_thresh: float = 0.196


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
    # 이 회차가 매칭된 medication_schedule.id. "복약 전달" 경로가 아니거나 매칭 안 했으면 None.
    medication_schedule_id: Optional[int] = None
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
    medication_schedule_id: Optional[int] = None
    created_by: Optional[int] = None


class PatrolScheduleUpdate(BaseModel):
    run_time: Optional[time] = None
    days_of_week: Optional[int] = None
    # DB엔 별도 enabled 컬럼이 없다 — False는 deregistered_at=NOW(), True는 NULL로 되돌린다.
    enabled: Optional[bool] = None
    medication_schedule_id: Optional[int] = None


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


class CancelPatrolRunRequest(BaseModel):
    # P6 웨이포인트 팝오버 "주행 취소" — patrol_run.id를 모른 채로도(P6 "이동"은
    # run_id=0이라 애초에 patrol_run 행이 없다) "이 로봇이 지금 하고 있는 걸 멈춰라"를
    # 표현할 수 있어야 해서 robot_code 하나만 받는다.
    robot_code: str


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


class PatrolRunLogItem(BaseModel):
    """시스템 설정 > 경로순찰 로그 탭 한 줄. PatrolRunItem과 달리 ID가 아니라
    사람이 읽을 이름(로봇 코드·대상자·경로명)과 한글 라벨을 바로 담아, 대시보드가
    AuditRow처럼 그대로 테이블에 그릴 수 있게 한다."""
    id: int
    started_at: datetime
    ended_at: Optional[datetime] = None
    robot_code: str
    recipient_name: Optional[str] = None
    route_name: Optional[str] = None
    trigger: str  # 한글 라벨(예약/수동/재확인)
    status: str   # 한글 라벨(진행 중/완료/중단/실패)
    waypoints_done: int
    waypoints_total: int
    highlight: bool = False  # 중단·실패


class SystemEventLogItem(BaseModel):
    """시스템 설정 > 임계값·알림 로그 탭 한 줄. system_event(배터리 임계값 등 자동
    감지 경보) 원본을 감사 로그(AuditRow)와 같은 모양으로 변환한 것."""
    occurred_at: datetime
    robot_code: Optional[str] = None
    event_type: str  # 한글 라벨
    severity: str    # info/warn/error (원본 그대로 — 색상 표시용)
    message: str
    highlight: bool = False


class RobotItem(BaseModel):
    id: int
    code: str
    name: str
    model: str
    ros_domain_id: Optional[int] = None
    ip: Optional[str] = None
    map_recipient: str = "-"
    is_online: bool
    # WS nav_status로만 갱신되는 캐시값(robot_ws.py::nav_running) — 로봇이 한 번도
    # nav_status를 보낸 적 없으면(막 등록됐거나 재시작 후 재연결 전) False.
    is_nav_running: bool = False


class RegisterRobotRequest(BaseModel):
    # DB 제약(robot.code VARCHAR(16), robot.ros_domain_id CHECK 0~232)과 동일한 범위를
    # 여기서도 검증한다 — 안 그러면 DB 제약 위반이 그대로 처리 안 된 예외로 터져 500이
    # 되고, 클라이언트는 "서버에 연결할 수 없습니다" 같은 엉뚱한 메시지를 보게 된다.
    code: str = Field(max_length=16)
    name: str
    model: str = "limo"
    ros_domain_id: Optional[int] = Field(default=None, ge=0, le=232)
    ip: Optional[str] = Field(default=None, max_length=45)


class RegisterRobotResponse(RobotItem):
    pass


class AssignRobotRecipientRequest(BaseModel):
    recipient_id: Optional[int] = None  # None이면 배정 해제


class RobotThresholds(BaseModel):
    battery_attention_pct: int = Field(default=30, ge=0, le=100)
    battery_return_pct: int = Field(default=28, ge=0, le=100)
    voice_wait_sec: int = Field(default=8, ge=1, le=120)
    reconnect_retry_sec: int = Field(default=30, ge=1, le=600)
    med_missed_wait_min: int = Field(default=10, ge=1, le=1440)
    med_reannounce_count: int = Field(default=2, ge=0, le=20)


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
    operator_id: Optional[int] = None  # 담당 관제담당자 계정 id. 미배정이면 None
    guardians: List[GuardianCreate] = []
    schedules: List[MedicationScheduleCreate] = []


class MedicationTodayItem(BaseModel):
    # 오늘 요일에 해당하는 스케줄 슬롯만 담는다 — 대상자마다 슬롯 수가 다를 수 있어(꼭 3개일
    # 필요 없음) 스케줄이 없는 슬롯은 아예 목록에서 빠진다.
    slot_no: int
    state: str  # "done" / "missed" / "pending". medication_log 결과가 아직 없으면 pending


class MedicationHistoryDay(BaseModel):
    date: date
    adherence: float  # 1.0 그날 스케줄 전부 수령 · 0.0 하나라도 미수령 · -1.0 그날 스케줄 없음(빈칸)


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
    operator_id: Optional[int] = None  # recipient.operator_id. 미배정이면 None
    operator_name: Optional[str] = None  # careguard_user.display_name 조인. 미배정이면 None
    # "normal"/"attention"/"emergency" — 열려있는(open/acknowledged) care_event 중 가장 심각한
    # 것 하나로 정한다. severity='info'는 위험도를 올리지 않는다(대시보드 Tokens::Level에
    # Info가 없음, media.py의 SEVERITY_TAGS와 같은 분류). 로봇 오프라인(offline)은 이 값에
    # 안 실린다 — 대시보드가 WS robot_status로 별도 판정해 이 값과 조합한다.
    level: str = "normal"
    status_summary: str = "등록됨 · 이상 없음"  # 활성 이벤트 있으면 care_event.summary로 대체
    meds_today: List[MedicationTodayItem] = []
    created_at: datetime


class RecipientDetailItem(RecipientItem):
    guardians: List[GuardianItem] = []
    schedules: List[MedicationScheduleItem] = []
    history: List[MedicationHistoryDay] = []  # 최근 28일. 목록(List) 조회에는 없고 상세에만 있다


class EventItem(BaseModel):
    event_uid: str
    severity: str  # "info" / "attention" / "emergency"
    event_type: str  # care_event.event_type 원본 코드
    event_type_label: str  # 한글 라벨 — event.py::EVENT_TYPE_LABELS로 변환
    summary: str
    occurred_at: datetime
    resolved: bool  # status == 'resolved'
    resolved_at: Optional[datetime] = None
    robot_code: Optional[str] = None  # 로봇 없는(자체 장애가 아닌) 이벤트면 None
    recipient_name: Optional[str] = None  # 대상자 없는 시스템 이벤트면 None
