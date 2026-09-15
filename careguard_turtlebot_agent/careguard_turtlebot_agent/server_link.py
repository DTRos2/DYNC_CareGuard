"""careguard_turtlebot_agent/server_link.py

서버(FastAPI)와의 WebSocket 연결(재연결 루프 포함)을 관리한다 —
downlink_node.py에 몰려있던 책임을 분리했다(2026-09-15 리팩터링). 수신
메시지는 raw 문자열 그대로 on_message 콜백에 넘기고(파싱은 호출부 책임),
발신은 send()로 큐잉한다. uplink_node.py와 같은 재연결 패턴을 쓴다.
"""
import asyncio
import json
import threading
from typing import Callable, Optional

import rclpy
import websockets

RECONNECT_DELAY_SEC = 3.0


class ServerLink:
    def __init__(self, url: str, on_message: Callable[[str], None], logger, role_label: str = '다운링크'):
        self._url = url
        self._on_message = on_message
        self._logger = logger
        self._role_label = role_label
        self._ws = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread = threading.Thread(target=self._run_loop, daemon=True)

    def start(self):
        self._thread.start()

    def send(self, payload: dict):
        if self._ws is None or self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(self._ws.send(json.dumps(payload)), self._loop)

    def _run_loop(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect_forever())

    async def _connect_forever(self):
        while rclpy.ok():
            try:
                async with websockets.connect(self._url) as ws:
                    self._ws = ws
                    self._logger.info(f'서버 {self._role_label} 연결됨: {self._url}')
                    async for raw in ws:
                        self._on_message(raw)
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                self._logger.warn(f'{self._role_label} 연결 실패, {RECONNECT_DELAY_SEC}s 후 재시도: {exc}')
            self._ws = None
            await asyncio.sleep(RECONNECT_DELAY_SEC)
