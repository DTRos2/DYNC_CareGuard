"""careguard_hmi/audio_capture_node.py

통화 중 로봇 마이크 소리를 서버로 올려보내는 노드 — audio_player.py(스피커, 완전
단방향 안내 방송)와는 별개 모듈이다. 상시 캡처가 아니라 downlink_node.py가
CALL_STATE_TOPIC(std_msgs/Bool)으로 통화 시작/종료를 알릴 때만 캡처 프로세스를
띄우고 내린다.

파이프라인: arecord(subprocess, raw PCM S16LE/16kHz/mono stdout) -> 20ms 프레임
(config.AUDIO_FRAME_BYTES바이트)씩 읽기 -> Opus 인코딩 -> 서버 오디오 릴레이
WebSocket(/ws/robot/{robot_code}/audio/uplink)으로 바이너리 전송.

서버 연결은 downlink_node.py의 _connect_forever()와 같은 패턴(자체 asyncio 이벤트
루프를 별도 스레드에서 돌리는 방식)을 그대로 따른다 — ROS 콜백 스레드(CALL_STATE_TOPIC
구독)와 오디오 스레드를 분리해서, 오디오 처리가 늦어져도 ROS 스핀에 영향을 주지 않고
그 반대도 마찬가지다.

빌드/실행 미검증 — 실 로봇에서 arecord 장치 이름(USB PnP Audio Device)과 opus 인코딩
파라미터를 맞춰봐야 한다.
"""
import asyncio
import subprocess
import threading

import rclpy
from rclpy.node import Node
from std_msgs.msg import Bool

from . import config

try:
    import opuslib
except ImportError:  # noqa: BLE001 - 모델 파일처럼(audio_player.py 참고) 설치 전에도 노드는 뜨게 둔다
    opuslib = None

# arecord가 못 찾을 때를 대비해 카드 지정 없이 기본 캡처 장치를 쓴다 — LIMO(R-07)에서
# 실측한 카드 번호(USB PnP Audio Device = card 0)를 고정하면 다른 로봇에서 깨질 수
# 있어, 환경변수로 오버라이드 가능하게만 해둔다.
import os  # noqa: E402 - config 임포트 이후 표준 라이브러리 위치를 맞추기보다 관련 상수 옆에 둔다

CAPTURE_DEVICE = os.environ.get('CAREGUARD_MIC_DEVICE', 'default')


class MicCaptureSession:
    """통화 한 번(call_start~call_end) 동안의 캡처 상태 — arecord 프로세스, Opus
    인코더, 서버로 보내는 asyncio 루프를 한 객체에 묶는다. AudioCaptureNode는 이
    객체의 start()/stop()만 호출하고, 내부 구현(프로세스 관리·재연결)은 몰라도 된다.
    """

    def __init__(self, logger) -> None:
        self._logger = logger
        self._process: subprocess.Popen | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self._stop_event: threading.Event | None = None
        self._encoder = None

    @property
    def is_running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self) -> None:
        if self.is_running:
            self._logger.warn('마이크 캡처가 이미 진행 중입니다 — 새 시작 요청을 무시합니다')
            return
        if opuslib is None:
            self._logger.error('opuslib가 설치돼 있지 않아 마이크 캡처를 시작할 수 없습니다 (pip install opuslib)')
            return

        self._stop_event = threading.Event()
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._logger.info('마이크 캡처 시작')

    def stop(self) -> None:
        if not self.is_running:
            return
        self._stop_event.set()
        if self._loop is not None:
            self._loop.call_soon_threadsafe(lambda: None)  # sleep 중인 코루틴을 깨워 stop_event를 즉시 보게 한다
        self._thread.join(timeout=5.0)
        self._terminate_arecord()
        self._logger.info('마이크 캡처 종료')
        self._thread = None
        self._loop = None

    def _terminate_arecord(self) -> None:
        if self._process is None:
            return
        try:
            self._process.terminate()
            self._process.wait(timeout=2.0)
        except Exception:  # noqa: BLE001 - 정리 경로라 실패해도 계속 진행
            try:
                self._process.kill()
            except Exception:  # noqa: BLE001
                pass
        self._process = None

    def _run(self) -> None:
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._capture_and_send())
        except Exception as exc:  # noqa: BLE001 - 백그라운드 스레드, 예외를 삼키고 로그만 남긴다
            self._logger.error(f'마이크 캡처 스레드가 예외로 종료했습니다: {exc}')

    async def _capture_and_send(self) -> None:
        import websockets

        self._encoder = opuslib.Encoder(config.AUDIO_SAMPLE_RATE, config.AUDIO_CHANNELS, opuslib.APPLICATION_VOIP)
        self._encoder.complexity = 10  # Orin NX는 CPU 여유가 있어 최대 복잡도를 쓴다(음질 우선)

        self._process = subprocess.Popen(
            [
                'arecord', '-D', CAPTURE_DEVICE, '-f', 'S16_LE',
                '-r', str(config.AUDIO_SAMPLE_RATE), '-c', str(config.AUDIO_CHANNELS),
                '-t', 'raw', '-q',
            ],
            stdout=subprocess.PIPE,
        )

        url = f'{config.SERVER_WS_URL}/ws/robot/{config.ROBOT_CODE}/audio/uplink'
        while not self._stop_event.is_set():
            try:
                async with websockets.connect(url) as ws:
                    self._logger.info(f'마이크 업링크 연결됨: {url}')
                    await self._pump_frames(ws)
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                if self._stop_event.is_set():
                    break
                self._logger.warn(
                    f'마이크 업링크 연결 실패, {config.AUDIO_WS_RECONNECT_DELAY_SEC}s 후 재시도: {exc}'
                )
                await asyncio.sleep(config.AUDIO_WS_RECONNECT_DELAY_SEC)

        self._terminate_arecord()

    async def _pump_frames(self, ws) -> None:
        """arecord stdout에서 20ms 프레임을 정확히 읽어(짧게 읽히면 다음 조각을
        기다린다) Opus로 인코딩해 보낸다. asyncio 표준 스트림이 아니라 subprocess.PIPE라
        블로킹 read를 스레드풀로 넘겨(to_thread) 이벤트 루프를 막지 않는다."""
        loop = asyncio.get_running_loop()
        while not self._stop_event.is_set():
            pcm = await loop.run_in_executor(None, self._read_exact, config.AUDIO_FRAME_BYTES)
            if pcm is None:
                break  # arecord가 죽었거나 EOF
            frame = self._encoder.encode(pcm, config.AUDIO_FRAME_SAMPLES)
            await ws.send(frame)

    def _read_exact(self, n: int) -> bytes | None:
        buf = bytearray()
        while len(buf) < n:
            if self._process is None or self._process.stdout is None:
                return None
            chunk = self._process.stdout.read(n - len(buf))
            if not chunk:
                return None
            buf.extend(chunk)
        return bytes(buf)


class AudioCaptureNode(Node):
    def __init__(self):
        super().__init__('audio_capture_node')
        self._session = MicCaptureSession(self.get_logger())
        self.create_subscription(Bool, config.CALL_STATE_TOPIC, self._on_call_state, 10)
        self.get_logger().info(f'audio_capture_node 시작 (topic={config.CALL_STATE_TOPIC})')

    def _on_call_state(self, msg: Bool):
        if msg.data:
            self._session.start()
        else:
            self._session.stop()

    def destroy_node(self):
        self._session.stop()
        super().destroy_node()


def main(args=None):
    rclpy.init(args=args)
    node = AudioCaptureNode()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
