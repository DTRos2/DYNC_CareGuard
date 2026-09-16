"""careguard_hmi/audio_downlink_player.py

통화 중 대시보드 음성을 받아 로봇 스피커로 재생하는 노드 — audio_player.py(문장
단위 TTS, 재생마다 wav 파일을 새로 만들어 1회성 subprocess.run)와 달리, 통화는
20ms 프레임이 계속 들어오므로 재생 프로세스를 통화 시작 시 한 번만 띄우고 stdin에
계속 흘려보내는 지속 프로세스 방식으로 구현한다.

파이프라인: 서버 오디오 릴레이 WebSocket(/ws/robot/{robot_code}/audio/downlink)에서
Opus 프레임 수신 -> 디코딩 -> JitterBuffer -> 20ms 주기로 paplay(--raw, 폴백 aplay)
stdin에 raw PCM 기록.

지터 버퍼: 재생 시작 전 JITTER_PREBUFFER_FRAMES개(약 60ms) 선버퍼링, 큐가 비면
무음 프레임을 대신 내보내고(끊김보다 무음이 덜 거슬린다), 큐가
JITTER_MAX_QUEUE_FRAMES를 넘으면 오래된 프레임을 버려 지연이 계속 쌓이지 않게 한다.

빌드/실행 미검증.
"""
import shutil
import subprocess
import threading
import time
from collections import deque
from typing import Optional

import rclpy
from rclpy.node import Node
from std_msgs.msg import Bool

from . import config

try:
    import opuslib
except ImportError:  # noqa: BLE001 - audio_player.py와 같은 이유로 미설치여도 노드는 뜨게 둔다
    opuslib = None

_SILENCE_FRAME = b'\x00' * config.AUDIO_FRAME_BYTES
_PLAYER_CANDIDATES = ('paplay', 'aplay')  # audio_player.py와 같은 폴백 순서


class JitterBuffer:
    """디코딩된 PCM 프레임(고정 크기 bytes)을 담아두는 스레드 안전 큐. push는
    수신 스레드(비동기 WS 콜백)에서, pop은 재생 스레드(20ms 주기)에서 부른다."""

    def __init__(self, max_frames: int) -> None:
        self._max_frames = max_frames
        self._frames: deque[bytes] = deque()
        self._lock = threading.Lock()

    def push(self, frame: bytes) -> None:
        with self._lock:
            self._frames.append(frame)
            while len(self._frames) > self._max_frames:
                self._frames.popleft()  # 오래된 프레임 드롭 — 지연 누적 방지

    def pop_or_silence(self) -> bytes:
        with self._lock:
            if self._frames:
                return self._frames.popleft()
        return _SILENCE_FRAME  # 언더런 — 끊기는 것보다 무음이 낫다

    def ready(self, prebuffer_frames: int) -> bool:
        with self._lock:
            return len(self._frames) >= prebuffer_frames

    def clear(self) -> None:
        with self._lock:
            self._frames.clear()


class CallPlaybackSession:
    """통화 한 번(call_start~call_end) 동안의 재생 상태 — 재생 프로세스, Opus
    디코더, 지터 버퍼, 20ms 주기 재생 스레드를 한 객체에 묶는다."""

    def __init__(self, logger) -> None:
        self._logger = logger
        self._process: Optional[subprocess.Popen] = None
        self._decoder = None
        self._buffer = JitterBuffer(config.JITTER_MAX_QUEUE_FRAMES)
        self._playback_thread: Optional[threading.Thread] = None
        self._stop_event: Optional[threading.Event] = None
        self._player_cmd = self._detect_player()

    @property
    def is_running(self) -> bool:
        return self._playback_thread is not None and self._playback_thread.is_alive()

    @staticmethod
    def _detect_player() -> str:
        for candidate in _PLAYER_CANDIDATES:
            if shutil.which(candidate):
                return candidate
        return _PLAYER_CANDIDATES[0]

    def start(self) -> None:
        if self.is_running:
            self._logger.warn('통화 재생이 이미 진행 중입니다 — 새 시작 요청을 무시합니다')
            return
        if opuslib is None:
            self._logger.error('opuslib가 설치돼 있지 않아 통화 재생을 시작할 수 없습니다 (pip install opuslib)')
            return

        self._decoder = opuslib.Decoder(config.AUDIO_SAMPLE_RATE, config.AUDIO_CHANNELS)
        self._buffer.clear()
        self._process = self._spawn_player()
        self._stop_event = threading.Event()
        self._playback_thread = threading.Thread(target=self._playback_loop, daemon=True)
        self._playback_thread.start()
        self._logger.info(f'통화 재생 시작 (player={self._player_cmd})')

    def stop(self) -> None:
        if not self.is_running:
            return
        self._stop_event.set()
        self._playback_thread.join(timeout=2.0)
        self._playback_thread = None
        self._terminate_player()
        self._buffer.clear()
        self._logger.info('통화 재생 종료')

    def on_frame_received(self, encoded: bytes) -> None:
        """오디오 WS 수신 스레드가 부른다 — 디코딩해서 지터 버퍼에 넣기만 하고,
        실제 재생 타이밍은 _playback_loop가 독립적으로 맞춘다(수신이 몰리거나
        끊겨도 재생 주기가 흔들리지 않게 하기 위함)."""
        if self._decoder is None:
            return
        try:
            pcm = self._decoder.decode(encoded, config.AUDIO_FRAME_SAMPLES)
        except Exception as exc:  # noqa: BLE001 - 손상된 프레임 하나 때문에 통화 전체가 끊기면 안 된다
            self._logger.warn(f'Opus 디코딩 실패, 이 프레임을 건너뜁니다: {exc}')
            return
        self._buffer.push(pcm)

    def _spawn_player(self) -> subprocess.Popen:
        # --raw: 헤더 없는 순수 PCM을 그대로 stdin으로 받는다. paplay는 포맷/레이트를
        # 인자로 명시해야 하고(자동 감지 불가), aplay 폴백도 같은 옵션 이름을 쓴다.
        rate = str(config.AUDIO_SAMPLE_RATE)
        channels = str(config.AUDIO_CHANNELS)
        if self._player_cmd == 'paplay':
            cmd = ['paplay', '--raw', f'--rate={rate}', f'--channels={channels}', '--format=s16le']
        else:
            cmd = ['aplay', '-t', 'raw', '-f', 'S16_LE', '-r', rate, '-c', channels, '-q']
        return subprocess.Popen(cmd, stdin=subprocess.PIPE)

    def _terminate_player(self) -> None:
        if self._process is None:
            return
        try:
            if self._process.stdin:
                self._process.stdin.close()
            self._process.terminate()
            self._process.wait(timeout=2.0)
        except Exception:  # noqa: BLE001 - 정리 경로
            try:
                self._process.kill()
            except Exception:  # noqa: BLE001
                pass
        self._process = None

    def _playback_loop(self) -> None:
        """20ms마다 정확히 한 프레임씩 stdin에 쓴다 — sleep 누적 오차를 막기 위해
        절대 시각(next_tick) 기준으로 다음 깨어날 시각을 계산한다(time.sleep(0.02)를
        반복하면 매 반복의 처리 시간만큼 계속 밀린다)."""
        frame_sec = config.AUDIO_FRAME_MS / 1000.0

        # 선버퍼링 — 통화 시작 직후 몇 프레임을 모을 때까지 무음 대신 잠깐 기다린다.
        deadline = time.monotonic() + 1.0  # 1초 안에 안 차면 그냥 무음 재생 시작(끝없이 기다리지 않는다)
        while not self._buffer.ready(config.JITTER_PREBUFFER_FRAMES) and time.monotonic() < deadline:
            if self._stop_event.is_set():
                return
            time.sleep(0.005)

        next_tick = time.monotonic()
        while not self._stop_event.is_set():
            pcm = self._buffer.pop_or_silence()
            try:
                if self._process is not None and self._process.stdin is not None:
                    self._process.stdin.write(pcm)
                    self._process.stdin.flush()
            except (BrokenPipeError, ValueError):
                self._logger.warn('재생 프로세스 파이프가 끊겨 재생을 중단합니다')
                return

            next_tick += frame_sec
            sleep_for = next_tick - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
            else:
                next_tick = time.monotonic()  # 너무 밀렸으면(디버거 정지 등) 기준 시각을 다시 잡는다


class AudioDownlinkPlayerNode(Node):
    def __init__(self):
        super().__init__('audio_downlink_player')
        self._session = CallPlaybackSession(self.get_logger())
        self._ws_thread: Optional[threading.Thread] = None
        self._ws_stop_event: Optional[threading.Event] = None
        self.create_subscription(Bool, config.CALL_STATE_TOPIC, self._on_call_state, 10)
        self.get_logger().info(f'audio_downlink_player 시작 (topic={config.CALL_STATE_TOPIC})')

    def _on_call_state(self, msg: Bool):
        if msg.data:
            self._session.start()
            self._start_ws_receiver()
        else:
            self._stop_ws_receiver()
            self._session.stop()

    def _start_ws_receiver(self) -> None:
        if self._ws_thread is not None and self._ws_thread.is_alive():
            return
        self._ws_stop_event = threading.Event()
        self._ws_thread = threading.Thread(target=self._run_ws_receiver, daemon=True)
        self._ws_thread.start()

    def _stop_ws_receiver(self) -> None:
        if self._ws_stop_event is not None:
            self._ws_stop_event.set()
        if self._ws_thread is not None:
            self._ws_thread.join(timeout=5.0)
        self._ws_thread = None

    def _run_ws_receiver(self) -> None:
        import asyncio
        asyncio.run(self._receive_forever())

    async def _receive_forever(self) -> None:
        import websockets

        url = f'{config.SERVER_WS_URL}/ws/robot/{config.ROBOT_CODE}/audio/downlink'
        while not self._ws_stop_event.is_set():
            try:
                async with websockets.connect(url) as ws:
                    self.get_logger().info(f'통화 다운링크 연결됨: {url}')
                    async for frame in ws:
                        if self._ws_stop_event.is_set():
                            break
                        self._session.on_frame_received(frame)
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                if self._ws_stop_event.is_set():
                    break
                self.get_logger().warn(
                    f'통화 다운링크 연결 실패, {config.AUDIO_WS_RECONNECT_DELAY_SEC}s 후 재시도: {exc}'
                )
                import asyncio as _asyncio
                await _asyncio.sleep(config.AUDIO_WS_RECONNECT_DELAY_SEC)

    def destroy_node(self):
        self._stop_ws_receiver()
        self._session.stop()
        super().destroy_node()


def main(args=None):
    rclpy.init(args=args)
    node = AudioDownlinkPlayerNode()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
