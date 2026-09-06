"""careguard_agent/downlink_node.py

서버 -> 로봇 방향 다운링크. ws://.../ws/robot?robot_code=... 로 명령을 받아
ROS2 액션/subprocess로 전달하는 유일한 진입점이다. uplink_node.py와는 별개로
자기 자신의 WS 연결을 하나 더 연다 — 실 운영에서는 두 노드가 연결 하나를
공유하도록 합치는 편이 낫지만, 이번 스코프에서는 최소 구현으로 남긴다
(서버 ws/robot_ws.py는 robot_code당 마지막 연결만 유지하므로, 두 연결이 동시에
붙어 있으면 나중 연결이 이전 연결을 덮어쓴다는 점에 유의).

빌드/실행 미검증 — careguard_msgs/action/Patrol.action, careguard_mission
/patrol_task.py의 액션 이름("patrol")과 맞춰서 실 로봇에서 검증 필요.
"""
import asyncio
import json
import signal
import subprocess
import threading

import rclpy
from geometry_msgs.msg import Twist
from rclpy.action import ActionClient
from rclpy.node import Node

from careguard_msgs.action import Patrol
from careguard_msgs.msg import Waypoint as WaypointMsg

from . import ws_config

RECONNECT_DELAY_SEC = 3.0


class DownlinkNode(Node):
    def __init__(self):
        super().__init__('downlink_node')
        self._patrol_client = ActionClient(self, Patrol, 'patrol')
        self._slam_process = None
        # P5 방향키 패널 -> 실제 구동. limo_base(/limo/limo_base)가 이미 /cmd_vel을
        # 구독하고 있다(실 로봇에서 topic info로 확인됨) — 네임스페이스 없이 그대로 쓴다.
        self._cmd_vel_pub = self.create_publisher(Twist, 'cmd_vel', 10)

        self._loop = asyncio.new_event_loop()
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
                    self.get_logger().info(f'서버 다운링크 연결됨: {url}')
                    async for raw in ws:
                        self._handle_command(raw)
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                self.get_logger().warn(f'다운링크 연결 실패, {RECONNECT_DELAY_SEC}s 후 재시도: {exc}')
            await asyncio.sleep(RECONNECT_DELAY_SEC)

    def _handle_command(self, raw: str):
        try:
            message = json.loads(raw)
        except ValueError:
            self.get_logger().warn(f'명령 JSON 파싱 실패: {raw!r}')
            return

        msg_type = message.get('type')
        if msg_type == 'patrol_start':
            self._start_patrol(message)
        elif msg_type == 'slam_start':
            self._start_slam(message)
        elif msg_type == 'slam_stop':
            self._stop_slam(message)
        elif msg_type == 'cmd_vel':
            self._handle_cmd_vel(message)

    def _handle_cmd_vel(self, message: dict):
        twist = Twist()
        twist.linear.x = float(message.get('linear', 0.0) or 0.0)
        twist.angular.z = float(message.get('angular', 0.0) or 0.0)
        self._cmd_vel_pub.publish(twist)

    def _start_patrol(self, message: dict):
        if not self._patrol_client.wait_for_server(timeout_sec=2.0):
            self.get_logger().error('patrol_task 액션 서버를 찾을 수 없습니다')
            return

        goal = Patrol.Goal()
        goal.run_id = int(message.get('run_id', 0) or 0)
        goal.waypoints = [
            WaypointMsg(
                code=str(w.get('code', '')),
                x=float(w.get('x', 0.0)),
                y=float(w.get('y', 0.0)),
                yaw=float(w.get('yaw', 0.0)),
            )
            for w in message.get('waypoints', [])
        ]
        self._patrol_client.send_goal_async(goal)

    def _start_slam(self, message: dict):
        # SLAM 백엔드: Cartographer. 원래 실사용 검증된 명령은 `ros2 launch wego
        # cartographer_launch.py`였는데(.bash_history 확인), 프로젝트 요구사항상
        # wego_ws를 참조하지 않고 이 저장소 안에서 자체 완결되어야 해서 wego 패키지의
        # launch/config/rviz를 careguard_bringup으로 그대로 옮겨왔다(출처는
        # careguard_bringup/THIRD_PARTY_LICENSE_wego.Apache-2.0.txt 참고). 로직은
        # 원본과 동일하므로 검증 결과도 그대로 유효하다.
        #
        # 베이스 구동(limo_base+ydlidar+robot_localization, careguard_bringup
        # teleop_launch.py에 해당)은 이 노드가 새로 띄우지 않는다 — 이미 켜져 있다고
        # 가정한다. 안 켜진 상태에서 limo_base를 중복 실행하면 시리얼 포트 충돌이 날
        # 수 있어서, 여기서 자동으로 다시 띄우는 대신 사전에 별도로 켜져 있어야 한다.
        if self._slam_process is not None and self._slam_process.poll() is None:
            self.get_logger().warn('이미 SLAM 작업이 진행 중입니다')
            return
        self._slam_process = subprocess.Popen(['ros2', 'launch', 'careguard_bringup', 'cartographer_launch.py'])

    def _stop_slam(self, message: dict):
        # "매핑 취소"/"매핑 재시작"(재시작은 서버가 취소 API 먼저 부른 뒤 slam_start를
        # 다시 보낸다) — 지금 떠 있는 cartographer_launch.py를 실제로 끈다. 이걸 안 하면
        # _start_slam이 "이미 SLAM 작업이 진행 중입니다"로 다음 시작을 계속 거부한다.
        # ros2 launch는 SIGINT를 받으면 자기가 띄운 노드들까지 정상 종료시켜준다(터미널
        # 에서 Ctrl+C 누르는 것과 같은 경로) — 그래서 SIGTERM/kill 대신 SIGINT를 쓴다.
        if self._slam_process is None or self._slam_process.poll() is not None:
            self.get_logger().warn('취소할 SLAM 작업이 없습니다')
            self._slam_process = None
            return
        self._slam_process.send_signal(signal.SIGINT)
        try:
            self._slam_process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.get_logger().warn('SLAM 프로세스가 5초 내에 안 죽어 강제 종료합니다')
            self._slam_process.kill()
            self._slam_process.wait(timeout=5.0)
        self._slam_process = None


def main(args=None):
    rclpy.init(args=args)
    node = DownlinkNode()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
