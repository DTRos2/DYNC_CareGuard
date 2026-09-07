"""careguard_turtlebot_agent/uplink_node.py

터틀봇 -> 서버 최소 업링크. 서버의 기존 /ws/robot?robot_code=... 에 접속만
유지한다 — 접속 즉시 서버(app/ws/robot_ws.py)가 대시보드에
{"type":"robot_status","robot_code":...,"online":True}를 브로드캐스트하고,
연결이 끊기면 online:False를 브로드캐스트한다. 그래서 이 노드는 별도
heartbeat 메시지를 보낼 필요가 없다 — TCP 연결을 유지하는 것 자체가 전부다.

연결 유지·재연결 로직은 robot_careguard_ws/src/careguard_agent/uplink_node.py
(LIMO용)의 _connect_forever를 그대로 재사용했다. 토픽 릴레이(SLAM 지도·카메라·
순찰 진행률 등)는 이번 범위 밖이라 구독하지 않는다 — 필요해지면
careguard_agent/uplink_node.py를 참고해 여기에 추가하면 된다.
"""
import asyncio
import threading

import rclpy
from rclpy.node import Node

from . import ws_config

RECONNECT_DELAY_SEC = 3.0


class UplinkNode(Node):
    def __init__(self):
        super().__init__('uplink_node')

        if not ws_config.ROBOT_CODE:
            self.get_logger().error(
                "CAREGUARD_ROBOT_CODE 환경변수가 비어 있습니다. 예: "
                "CAREGUARD_ROBOT_CODE=R-05 ros2 run careguard_turtlebot_agent uplink_node")
            raise SystemExit(1)

        self._loop = asyncio.new_event_loop()
        self._ws = None
        self._connected = False

        self._thread = threading.Thread(target=self._run_ws_loop, daemon=True)
        self._thread.start()

    def _run_ws_loop(self):
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect_forever())

    async def _connect_forever(self):
        import websockets

        url = f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}'
        while rclpy.ok():
            try:
                async with websockets.connect(url) as ws:
                    self._ws = ws
                    self._connected = True
                    self.get_logger().info(f'서버 업링크 연결됨: {url}')
                    await ws.wait_closed()
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                self.get_logger().warn(f'업링크 연결 실패, {RECONNECT_DELAY_SEC}s 후 재시도: {exc}')
            self._connected = False
            self._ws = None
            await asyncio.sleep(RECONNECT_DELAY_SEC)


def main(args=None):
    rclpy.init(args=args)
    node = UplinkNode()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
