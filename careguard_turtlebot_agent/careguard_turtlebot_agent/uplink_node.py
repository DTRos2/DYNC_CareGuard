"""careguard_turtlebot_agent/uplink_node.py

터틀봇 -> 서버 업링크. 서버의 기존 /ws/robot?robot_code=... 에 접속을 유지하며
{"type":"position",...}를 보낸다 — 서버(app/ws/robot_ws.py::_handle_message)가
그대로 {"type":"robot_position",...}로 대시보드에 브로드캐스트한다. 접속/해제는
서버가 자동으로 robot_status(online True/False)를 브로드캐스트하므로 별도
heartbeat 메시지는 필요 없다.

연결 유지·재연결 로직은 robot_careguard_ws/src/careguard_agent/uplink_node.py
(LIMO용)의 _connect_forever를 그대로 재사용했다.

position 소스는 /odom(nav_msgs/Odometry)이다 — 실제 파이 조사 결과
(2026-09-07) 이 로봇은 기본 bringup만으로는 AMCL/SLAM이 켜져있지 않아
map->base_link TF도, /amcl_pose도 존재하지 않는다. 그래서 map 프레임 절대
위치가 아니라 /odom 기준 상대 위치(로봇 부팅 시점이 원점)를 보낸다 — 범위를
좁게 잡은 의도적 1단계 결정이다. AMCL/맵 정렬 위치가 필요해지면 LIMO의
_on_slam_overlay_tick처럼 map->base_link TF 구독으로 교체하면 된다.

지도·카메라 등 다른 토픽 릴레이는 이번 범위 밖이라 구독하지 않는다 — 필요해지면
careguard_agent/uplink_node.py를 참고해 여기에 추가하면 된다. /battery_state만
"배터리 주의/복귀" 임계값 판단(서버 쪽, robot_ws.py::_handle_battery_state)에
써야 해서 예외적으로 relay한다.
"""
import asyncio
import json
import math
import threading

import rclpy
import requests
from nav_msgs.msg import Odometry
from rclpy.node import Node
from sensor_msgs.msg import BatteryState

from . import ws_config

DEFAULT_RECONNECT_DELAY_SEC = 3.0


def _fetch_reconnect_delay_sec(logger) -> float:
    """대시보드 "시스템 설정 > 임계값·알림"에서 저장한 재연결 재시도 주기를 가져온다.

    robot_id를 모르니 /robots에서 코드로 먼저 찾고 나서 /robots/{id}/thresholds를
    부른다 — 시작할 때 한 번만 하는 동기 호출이라 순서대로 두 번 부르는 걸로 충분하다.
    서버가 아직 안 떠 있거나 이 로봇이 등록 전이면 기본값(3.0s)으로 조용히 넘어간다.
    """
    try:
        robots = requests.get(f'{ws_config.SERVER_HTTP_URL}/robots', timeout=5).json()
        robot = next((r for r in robots if r['code'] == ws_config.ROBOT_CODE), None)
        if robot is None:
            logger.warn(f'{ws_config.ROBOT_CODE} 미등록 — 재연결 주기 기본값({DEFAULT_RECONNECT_DELAY_SEC}s) 사용')
            return DEFAULT_RECONNECT_DELAY_SEC
        thresholds = requests.get(
            f'{ws_config.SERVER_HTTP_URL}/robots/{robot["id"]}/thresholds', timeout=5).json()
        return float(thresholds['reconnect_retry_sec'])
    except Exception as exc:  # noqa: BLE001 - 시작을 막으면 안 되니 실패해도 기본값으로 계속 진행
        logger.warn(f'재연결 주기 조회 실패, 기본값({DEFAULT_RECONNECT_DELAY_SEC}s) 사용: {exc}')
        return DEFAULT_RECONNECT_DELAY_SEC


def _yaw_from_quaternion(q) -> float:
    siny_cosp = 2 * (q.w * q.z + q.x * q.y)
    cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z)
    return math.atan2(siny_cosp, cosy_cosp)


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
        self._reconnect_delay_sec = _fetch_reconnect_delay_sec(self.get_logger())

        self.create_subscription(Odometry, '/odom', self._on_odom, 10)
        self.create_subscription(BatteryState, '/battery_state', self._on_battery_state, 10)

        self._thread = threading.Thread(target=self._run_ws_loop, daemon=True)
        self._thread.start()

    def _on_odom(self, msg: Odometry):
        yaw = _yaw_from_quaternion(msg.pose.pose.orientation)
        self._send({
            'type': 'position',
            'x': msg.pose.pose.position.x,
            'y': msg.pose.pose.position.y,
            'yaw': yaw,
        })

    def _on_battery_state(self, msg: BatteryState):
        # 드라이버에 따라 percentage를 0.0~1.0(REP 표준) 또는 0~100 그대로 발행하는
        # 경우가 섞여 있어(turtlebot3_node가 후자인 경우가 실제로 관찰됨), 1.0 이하면
        # 비율로 보고 100을 곱한다.
        percentage = msg.percentage * 100.0 if msg.percentage <= 1.0 else msg.percentage
        self._send({'type': 'battery_state', 'percentage': percentage})

    def _send(self, payload: dict):
        if not self._connected or self._ws is None:
            return
        asyncio.run_coroutine_threadsafe(self._ws.send(json.dumps(payload)), self._loop)

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
                self.get_logger().warn(f'업링크 연결 실패, {self._reconnect_delay_sec}s 후 재시도: {exc}')
            self._connected = False
            self._ws = None
            await asyncio.sleep(self._reconnect_delay_sec)


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
