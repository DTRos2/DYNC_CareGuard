"""careguard_hmi/audio_player.py

HMI 계층의 스피커 담당. 지금은 로봇 동작(순찰 시작/실패/완료 등)에 맞춘 한국어
음성 안내만 구현돼 있다 — face_display.py(LCD 표정)·contact_display.py(관제
연락망 노출)와 나란히 이 패키지에 속하고, 등록 영상/음성 재생·복약 안내처럼
이 파일 설명에 있는 다른 스피커 용도는 아직 스코프 밖이다(빈 상태).

/careguard/voice/announce(std_msgs/String, JSON: {"text": "..."})로 안내할
문장을 그대로 받아 Piper TTS로 합성 후 스피커로 재생한다.

    ros2 topic pub --once /careguard/voice/announce std_msgs/msg/String \
        "data: '{\"text\": \"순찰을 시작합니다\"}'"

완성된 문장을 그대로 받는 방식이라(사전 정의된 key가 아니라) 웨이포인트 번호
같은 동적인 내용도 발행하는 쪽(patrol_task.py)이 문자열에 이미 채워서 보내면
그대로 반영된다.

합성 엔진: 처음엔 espeak-ng(포먼트 합성)로 구현했는데, 한국어가 음절 단위로
뚝뚝 끊기고 종결어미 억양이 없어 "어색하게 들린다"는 실 로봇 테스트 피드백을
받았다(2026-09-09) — Piper(오프라인 경량 신경망 TTS, ONNX)로 교체해 사람
목소리에 훨씬 가깝게 만들었다. 로봇에 아래가 준비돼 있어야 한다:
    pip install piper-tts
    config.PIPER_MODEL_PATH 위치에 한국어 보이스(.onnx + .onnx.json) 다운로드
    (config.py 주석 참고)

재생 경로: Piper로 임시 wav 파일을 만든 뒤 paplay(PulseAudio/PipeWire)로
재생한다 — 합성 엔진이 오디오 장치에 직접 재생하는 기능에 기대지 않는다. 이
로봇(Jetson)은 GUI 사운드 설정으로 출력 장치(HDMI/USB 스피커 등)를 고르는
PulseAudio/PipeWire 환경인데, 합성 엔진이 그 설정을 무시하고 엉뚱한 ALSA
장치로 바로 나가버릴 수 있다(espeak-ng에서 실제로 겪음 — 2026-09-09) —
paplay는 시스템 기본 출력 장치(`pactl set-default-sink`로 고른 것)를 항상
그대로 따라가므로 이 문제가 없다. paplay가 없는 환경(PulseAudio/PipeWire
미설치) 대비로 aplay를 폴백으로 둔다.

재생은 subprocess.run으로 동기 실행한다 — 이 노드가 하는 일이 안내 방송뿐이라
비동기 큐잉이 필요 없고, 오히려 동기 실행이 "문장 두 개가 겹쳐 재생되는"
사고를 자동으로 막아준다(다음 문장은 지금 문장이 끝난 뒤에야 처리됨 — 같은
이유로, 아주 촘촘하게 안내가 몰리면 뒤쪽 안내가 그만큼 늦게 나올 수 있다는
트레이드오프는 있다).

빌드/실행 미검증.
"""
import json
import os
import shutil
import subprocess
import tempfile
import uuid

import rclpy
from rclpy.node import Node
from std_msgs.msg import Bool, String

from . import config

# paplay가 없는 환경(순수 ALSA만 있는 최소 이미지 등) 대비 폴백.
_PLAYER_CANDIDATES = ('paplay', 'aplay')


class AudioPlayer(Node):
    def __init__(self):
        super().__init__('audio_player')
        self._player_cmd = self._detect_player()
        self._call_active = False
        self.create_subscription(String, config.VOICE_TOPIC, self._on_announce, 10)
        # 통화 중에는 이 노드도 같은 스피커 장치를 쓰는 audio_downlink_player.py와
        # 출력이 겹치면 안 된다 — TTS는 겹칠 바엔 스킵한다(재생 큐를 새로 만들기보다
        # 최소 침습적인 선택, config.CALL_STATE_TOPIC 주석 참고).
        self.create_subscription(Bool, config.CALL_STATE_TOPIC, self._on_call_state, 10)
        self.get_logger().info(
            f'audio_player 시작: model={config.PIPER_MODEL_PATH} '
            f'player={self._player_cmd} topic={config.VOICE_TOPIC}'
        )
        if not os.path.isfile(config.PIPER_MODEL_PATH):
            # 여기서 막지는 않는다 — 모델을 나중에 받아 넣고 이 노드를 재시작하면
            # 되므로, 노드 자체는 뜨게 두고 실제 안내가 올 때마다 계속 에러 로그로
            # 알려준다(아래 _speak 참고).
            self.get_logger().warn(
                f'Piper 음성 모델을 아직 못 찾았습니다: {config.PIPER_MODEL_PATH} — '
                '다운로드 전까지는 음성 안내가 나오지 않습니다'
            )

    @staticmethod
    def _detect_player() -> str:
        for candidate in _PLAYER_CANDIDATES:
            if shutil.which(candidate):
                return candidate
        return _PLAYER_CANDIDATES[0]  # 못 찾아도 일단 시도 — 실패하면 _speak()가 로그로 알려줌

    def _on_call_state(self, msg: Bool):
        self._call_active = msg.data

    def _on_announce(self, msg: String):
        if self._call_active:
            self.get_logger().info('통화 중이라 음성 안내를 건너뜁니다')
            return
        try:
            payload = json.loads(msg.data) if msg.data else {}
        except ValueError:
            self.get_logger().warn(f'안내 메시지 JSON 파싱 실패, 무시합니다: {msg.data!r}')
            return

        text = (payload.get('text') or '').strip()
        if not text:
            self.get_logger().warn('안내할 텍스트가 비어 있어 무시합니다')
            return

        self._speak(text)

    def _speak(self, text: str):
        self.get_logger().info(f'음성 안내: {text}')

        if not os.path.isfile(config.PIPER_MODEL_PATH):
            self.get_logger().error(
                f'Piper 음성 모델을 찾을 수 없어 안내를 건너뜁니다: {config.PIPER_MODEL_PATH} '
                '(config.py 상단 주석 참고해 다운로드하세요)'
            )
            return

        wav_path = os.path.join(tempfile.gettempdir(), f'careguard_voice_{uuid.uuid4().hex}.wav')
        try:
            # piper는 텍스트를 stdin으로 받는다(예: `echo "..." | piper --model ... --output_file ...`).
            # subprocess의 input=으로 같은 효과를 낸다 — 셸 파이프를 새로 열 필요가 없다.
            synth = subprocess.run(
                ['piper', '--model', config.PIPER_MODEL_PATH, '--output_file', wav_path],
                input=text, capture_output=True, text=True, timeout=20,
            )
            if synth.returncode != 0:
                self.get_logger().warn(f'piper 합성 실패(rc={synth.returncode}): {synth.stderr}')
                return

            play = subprocess.run(
                [self._player_cmd, wav_path], capture_output=True, text=True, timeout=15,
            )
            if play.returncode != 0:
                self.get_logger().warn(f'{self._player_cmd} 재생 실패(rc={play.returncode}): {play.stderr}')
        except FileNotFoundError as exc:
            self.get_logger().error(
                f'필요한 프로그램이 설치돼 있지 않습니다({exc.filename}) — '
                'pip install piper-tts (재생기: sudo apt install pulseaudio-utils)'
            )
        except subprocess.TimeoutExpired:
            self.get_logger().warn(f'음성 합성/재생이 시간 초과로 중단했습니다: {text!r}')
        finally:
            # 실패해서 파일이 없어도(unlink 대상 없음) 조용히 넘어간다 — 재생 성공/실패
            # 어느 쪽이든 임시 파일이 쌓이면 안 되므로 항상 정리한다.
            try:
                os.remove(wav_path)
            except OSError:
                pass


def main(args=None):
    rclpy.init(args=args)
    node = AudioPlayer()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        # SIGINT 시 rclpy가 내부적으로 먼저 shutdown()을 부를 수 있어, 여기서
        # 무조건 또 부르면 "rcl_shutdown already called"로 비정상 종료한다
        # (slam_progress_publisher.py에서 실제로 겪은 버그, 같은 패턴이라 방어).
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
