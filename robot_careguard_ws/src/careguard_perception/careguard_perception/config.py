import os
from pathlib import Path

# 로봇 로컬 영상 저장 경로. 환경변수로 뺀다 — careguard_agent/ws_config.py와 같은 방식.
VIDEO_OUTPUT_DIR = Path(
    os.environ.get('CAREGUARD_VIDEO_DIR', os.path.expanduser('~/careguard_data/videos'))
)
FACE_DATASET_DIR = Path(
    os.environ.get('CAREGUARD_FACE_DATASET_DIR', os.path.expanduser('~/careguard_data/face_dataset'))
)

# 사이드카 메타데이터에 남길 로봇 코드. POST /media/upload가 robot_code도 받으므로
# robot_id 변환 없이 그대로 쓴다.
ROBOT_CODE = os.environ.get('CAREGUARD_ROBOT_CODE', 'R-01')

CAMERA_TOPIC = '/camera/color/image_raw/compressed'
TRIGGER_TOPIC = '/careguard/perception/record_trigger'

# 순찰 시작~충전소 복귀까지 이어지는 연속 녹화 시작/종료 신호. careguard_mission/
# patrol_task.py가 발행하고(같은 문자열을 그쪽에도 하드코딩— patrol_status처럼 이
# 코드베이스는 토픽명을 패키지 간 공유 없이 문자열로 중복 정의하는 방식을 쓴다),
# video_recorder.py가 구독한다. TRIGGER_TOPIC(이벤트 pre/post 클립)과는 별개 토픽 —
# 의미(즉시 시작 vs pre-roll 포함)도, 동시 진행 가능 여부도 다르다.
PATROL_RECORDING_TOPIC = '/careguard/perception/patrol_recording'

# 트리거 전/후 기본 녹화 구간(초). ros2 파라미터로 덮어쓸 수 있다.
PRE_EVENT_SEC_DEFAULT = 5.0
POST_EVENT_SEC_DEFAULT = 15.0

# careguard_hmi/config.py의 VOICE_TOPIC과 같은 문자열(다른 패키지 간 토픽명 공유 없이
# 문자열로 중복 정의하는 기존 방식 — PATROL_RECORDING_TOPIC 주석 참고). record_trigger에
# severity가 실려 오면 video_recorder.py가 이 토픽으로 "긴급/주의 이벤트 발생" 안내를 쏜다.
VOICE_TOPIC = '/careguard/voice/announce'

# patrol_intruder_watch_node.py가 미등록 인물(침입자) 판정을 발행하는 토픽. uplink_node.py의
# _on_status_topic이 /careguard/patrol_status와 같은 방식으로 그대로 relay해서 서버
# robot_ws.py::_handle_message의 "intruder_detected" 분기로 전달한다(같은 문자열을
# uplink_node.py에도 하드코딩 — PATROL_RECORDING_TOPIC 주석과 같은 기존 방식).
INTRUDER_STATUS_TOPIC = '/careguard/perception/intruder_status'

# 대상자로 확정된 순간(2단계 서보모터 트리거) — careguard_hw 쪽 아두이노 브리지 노드가
# 이 토픽만 구독해서 서보를 움직인다. 서버/대시보드는 전혀 관여하지 않는 로봇 로컬
# 전용 토픽이라 uplink_node.py로 relay하지 않는다 — 팀원 제공 스펙(person.png)대로
# std_msgs/Bool, data:true만 발행하고 끝(false는 안 보냄, 상태 토픽이 아니라 순간
# 트리거 신호).
PERSON_DETECTED_TOPIC = '/careguard/perception/person_detected'

# 대상자로 확정된 순간의 얼굴 캡처(증거 사진) — INTRUDER_STATUS_TOPIC과 같은 이유·
# 같은 방식으로 uplink_node.py가 relay해서 서버가 files에 저장한다(다만 care_event는
# 안 만든다 — 알림이 아니라 인식 기록이라서). PERSON_DETECTED_TOPIC(서보 트리거)과는
# 완전히 별개 토픽이다 — 서보 쪽엔 이미지가 필요 없고, 순수 Bool 신호만 최대한
# 가볍고 지연 없이 전달돼야 하기 때문.
PERSON_RECOGNIZED_TOPIC = '/careguard/perception/person_recognized'

# 정지→접근→재판별(2026-09-15 추가) — patrol_intruder_watch_node.py <-> patrol_task.py
# 로컬 통신용 토픽. 서버는 이 왕복에 전혀 관여하지 않는다(로봇이 실시간으로 계산한
# 좌표라 서버가 검증/보관할 이유가 없음 — 최종 판정 결과만 INTRUDER_STATUS_TOPIC으로
# 올라간다). careguard_mission/patrol_task.py에도 같은 문자열을 하드코딩(기존 방식).
APPROACH_REQUEST_TOPIC = '/careguard/perception/approach_request'  # perception -> patrol_task
APPROACH_ARRIVED_TOPIC = '/careguard/perception/approach_arrived'  # patrol_task -> perception
APPROACH_RELEASE_TOPIC = '/careguard/perception/approach_release'  # perception -> patrol_task

# 접근 목표 거리(m) — 사람 앞 이 거리까지만 다가간다(그 이상 가까이 가면 부담스럽고,
# 카메라가 못 잡을 수도 있음). 사용자 결정값.
APPROACH_TARGET_DISTANCE_M = 1.5

# 같은 사람이 접근을 피해 계속 새 트랙으로 잡혀도 로봇이 끝없이 쫓아다니지 않도록,
# 짧은 시간 안의 접근 시도 횟수를 제한한다(사용자 결정값: 5분에 최대 2회). 제한에
# 걸리면 접근 없이 원거리 판정을 그대로 최종 결과로 쓴다.
APPROACH_MAX_ATTEMPTS = 2
APPROACH_ATTEMPTS_WINDOW_SEC = 300.0

# 접근 도착 후 재판별에 쓸 최대 대기 시간(초)과 최대 프레임 수. 그 안에 같은 사람을
# 한 번도 못 잡으면(회피 추정) 원거리 판정으로 대체한다 — verified_at_close_range=False로
# 표시해서 재확인 못 했다는 걸 남긴다.
APPROACH_VERIFY_TIMEOUT_SEC = 5.0
APPROACH_VERIFY_MAX_SAMPLES = 5

# bbox 폭 기반 거리 추정(라이다가 그 방향에 유효한 리턴이 없을 때 대체용)에 쓰는
# 성인 평균 얼굴 폭 근사치(m).
AVERAGE_FACE_WIDTH_M = 0.16
