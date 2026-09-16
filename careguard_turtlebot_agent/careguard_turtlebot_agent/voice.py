"""careguard_turtlebot_agent/voice.py

Piper TTS 음성 안내 — downlink_node.py에 몰려있던 책임을 분리했다(2026-09-15
리팩터링). LIMO(careguard_hmi/audio_player.py)의 핵심 로직(piper CLI로 wav
합성 -> paplay/aplay로 재생)과 같은 절차를, 별도 노드/토픽 없이 이 클래스가
동기 함수 호출로 직접 수행한다. 보이스 모델은 LIMO(R-07)가 쓰던 것과 같은
파일(ko_KR-kss-medium.onnx, 단일 화자)을 그대로 복사해 썼다 — 카메라
캘리브레이션과 달리 로봇마다 새로 구할 필요가 없는 순수 파일 자산이라 복사가
맞는 방법이라고 판단했다.
"""
import os
import shutil
import subprocess
import tempfile
import uuid

TTS_MODEL_PATH = os.environ.get(
    'CAREGUARD_TTS_MODEL', os.path.expanduser('~/piper-voices/ko_KR-kss-medium.onnx'))
TTS_PLAYER_CANDIDATES = ('paplay', 'aplay')


class VoiceAnnouncer:
    """텍스트를 Piper로 합성해 스피커로 재생한다. speak()는 동기(블로킹)다 —
    순찰/복귀 상태 기계가 스레드 하나로 순차 진행되므로, 문장 하나 재생 시간
    (보통 1~2초)만큼만 다음 주행 단계를 늦춘다.
    """

    def __init__(self, logger):
        self._logger = logger
        self._player_cmd = next((c for c in TTS_PLAYER_CANDIDATES if shutil.which(c)),
                                 TTS_PLAYER_CANDIDATES[0])
        if not os.path.isfile(TTS_MODEL_PATH):
            self._logger.warn(
                f'Piper 음성 모델을 못 찾았습니다: {TTS_MODEL_PATH} — 음성 안내가 나오지 않습니다')

    def speak(self, text: str):
        self._logger.info(f'음성 안내: {text}')
        if not os.path.isfile(TTS_MODEL_PATH):
            self._logger.warn(f'Piper 음성 모델이 없어 안내를 건너뜁니다: {TTS_MODEL_PATH}')
            return

        wav_path = os.path.join(tempfile.gettempdir(), f'careguard_voice_{uuid.uuid4().hex}.wav')
        try:
            synth = subprocess.run(
                ['piper', '--model', TTS_MODEL_PATH, '--output_file', wav_path],
                input=text, capture_output=True, text=True, timeout=20,
            )
            if synth.returncode != 0:
                self._logger.warn(f'piper 합성 실패(rc={synth.returncode}): {synth.stderr}')
                return
            play = subprocess.run([self._player_cmd, wav_path], capture_output=True, text=True, timeout=15)
            if play.returncode != 0:
                self._logger.warn(f'{self._player_cmd} 재생 실패(rc={play.returncode}): {play.stderr}')
        except FileNotFoundError as exc:
            self._logger.error(f'필요한 프로그램이 없습니다({exc.filename}) — pip install piper-tts')
        except subprocess.TimeoutExpired:
            self._logger.warn(f'음성 합성/재생이 시간 초과로 중단했습니다: {text!r}')
        finally:
            try:
                os.remove(wav_path)
            except OSError:
                pass
