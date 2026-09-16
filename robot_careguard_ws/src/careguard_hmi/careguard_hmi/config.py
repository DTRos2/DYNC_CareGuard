import os

# audio_player.py/audio_capture_node.py/audio_downlink_player.py 공용 설정.
# face_display.py/contact_display.py가 나중에 추가되면 그쪽 설정도 이 파일에 같이
# 두거나, 필요해지면 파일을 나눈다.

VOICE_TOPIC = '/careguard/voice/announce'

# downlink_node.py(careguard_agent)가 call_start/call_end를 받을 때마다 True/False로
# 발행한다(같은 이름의 상수를 그 파일에도 따로 둔다 — VOICE_TOPIC/RECORD_TRIGGER_TOPIC과
# 같은 이 코드베이스의 관례, 패키지 간 의존 없이 문자열만 맞춘다). audio_player.py는
# 통화 중 TTS 안내가 통화 오디오와 겹치지 않도록 이 상태를 보고 안내를 건너뛰고,
# audio_capture_node.py/audio_downlink_player.py는 이 상태로 캡처/재생 프로세스 자체를
# 시작·종료한다.
CALL_STATE_TOPIC = '/careguard/call/state'

# 서버 주소/로봇 코드 — careguard_agent/ws_config.py와 같은 관례(환경변수 하나로 통일).
# audio_capture_node.py/audio_downlink_player.py가 서버의 오디오 릴레이 WebSocket에
# 붙을 때 쓴다.
SERVER_HTTP_URL = os.environ.get('CAREGUARD_SERVER_URL', 'http://127.0.0.1:8000')
SERVER_WS_URL = SERVER_HTTP_URL.replace('https://', 'wss://').replace('http://', 'ws://')
ROBOT_CODE = os.environ.get('CAREGUARD_ROBOT_CODE', 'R-01')

# Opus/오디오 파이프라인 공통 파라미터 — 캡처(arecord)와 재생(paplay --raw) 양쪽이
# 정확히 같은 값을 써야 한다(하나라도 다르면 재생 속도/피치가 어긋난다).
AUDIO_SAMPLE_RATE = 16000
AUDIO_CHANNELS = 1
AUDIO_FRAME_MS = 20
AUDIO_FRAME_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS // 1000  # 320 samples
AUDIO_FRAME_BYTES = AUDIO_FRAME_SAMPLES * 2  # S16_LE = 샘플당 2바이트

# 재생 지터 버퍼 목표치 — 60~100ms(3~5프레임) 사이. 너무 작으면 언더런(끊김)이 잦고
# 너무 크면 통화 지연이 커진다.
JITTER_PREBUFFER_FRAMES = 3
JITTER_MAX_QUEUE_FRAMES = 10  # 이 이상 쌓이면 오래된 프레임을 버려 지연 누적을 막는다

# 오디오 WebSocket이 끊겼을 때 재시도 간격 — downlink_node.py의 다운링크 재연결
# (RECONNECT_DELAY_SEC)과 같은 값을 그대로 맞춘다.
AUDIO_WS_RECONNECT_DELAY_SEC = 3.0

# Piper TTS 한국어 보이스 모델(.onnx) 경로. 같은 이름의 .onnx.json(보이스 설정)도
# 같은 위치에 있어야 한다(piper가 <모델경로>.json을 자동으로 찾는다). 로봇에
# 아래로 준비해둬야 한다:
#   pip install piper-tts
#   huggingface.co/rhasspy/piper-voices 의 ko/ko_KR/kss/medium/ 아래
#   ko_KR-kss-medium.onnx + ko_KR-kss-medium.onnx.json 을 이 경로에 받아두기
# (espeak-ng보다 한국어 음질이 훨씬 자연스럽다 — 2026-09-09 실 로봇 테스트에서
# espeak-ng 안내가 "어색하게 들린다"는 피드백을 받아 교체함.)
PIPER_MODEL_PATH = os.environ.get(
    'CAREGUARD_TTS_MODEL', os.path.expanduser('~/piper-voices/ko_KR-kss-medium.onnx')
)
