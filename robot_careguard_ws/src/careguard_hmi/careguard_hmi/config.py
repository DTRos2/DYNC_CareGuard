import os

# audio_player.py 전용 설정. face_display.py/contact_display.py가 나중에 추가되면
# 그쪽 설정도 이 파일에 같이 두거나, 필요해지면 파일을 나눈다.

VOICE_TOPIC = '/careguard/voice/announce'

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
