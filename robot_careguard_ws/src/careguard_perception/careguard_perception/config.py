import os
from pathlib import Path

# 로봇 로컬 영상 저장 경로. 환경변수로 뺀다 — careguard_agent/ws_config.py와 같은 방식.
VIDEO_OUTPUT_DIR = Path(
    os.environ.get('CAREGUARD_VIDEO_DIR', os.path.expanduser('~/careguard_data/videos'))
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
