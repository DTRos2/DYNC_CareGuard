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

# 트리거 전/후 기본 녹화 구간(초). ros2 파라미터로 덮어쓸 수 있다.
PRE_EVENT_SEC_DEFAULT = 5.0
POST_EVENT_SEC_DEFAULT = 15.0
