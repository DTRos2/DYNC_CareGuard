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
import os
import signal
import subprocess
import threading

import rclpy
import yaml
from geometry_msgs.msg import Twist
from rclpy.action import ActionClient
from rclpy.node import Node

from careguard_msgs.action import Patrol
from careguard_msgs.msg import Waypoint as WaypointMsg

from . import media_uploader, ws_config

RECONNECT_DELAY_SEC = 3.0


class DownlinkNode(Node):
    def __init__(self):
        super().__init__('downlink_node')
        self._patrol_client = ActionClient(self, Patrol, 'patrol')
        self._slam_process = None
        self._ws = None
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
                    self._ws = ws
                    self.get_logger().info(f'서버 다운링크 연결됨: {url}')
                    async for raw in ws:
                        self._handle_command(raw)
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                self.get_logger().warn(f'다운링크 연결 실패, {RECONNECT_DELAY_SEC}s 후 재시도: {exc}')
            self._ws = None
            await asyncio.sleep(RECONNECT_DELAY_SEC)

    def _send(self, payload: dict):
        # save_map처럼 로봇이 자기 작업 결과를 서버에 알려야 하는 경우 이 연결(다운링크)
        # 그대로 되쏜다 — uplink_node의 별도 연결을 굳이 거칠 필요가 없다. _handle_command가
        # 이미 이 코루틴과 같은 스레드/이벤트루프에서 돌지만, uplink_node와 같은 방식
        # (run_coroutine_threadsafe)으로 통일해 어디서 불러도 안전하게 한다.
        if self._ws is None:
            self.get_logger().warn('다운링크 연결이 없어 메시지를 보낼 수 없습니다')
            return
        asyncio.run_coroutine_threadsafe(self._ws.send(json.dumps(payload)), self._loop)

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
        elif msg_type == 'save_map':
            self._save_map(message)
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
        # start_new_session=True로 이 프로세스를 새 프로세스 그룹의 리더로 만든다 —
        # _stop_slam이 SIGINT를 "그룹 전체"에 보낼 수 있어야 한다(아래 이유 참고).
        self._slam_process = subprocess.Popen(
            ['ros2', 'launch', 'careguard_bringup', 'cartographer_launch.py'], start_new_session=True)

    def _stop_slam(self, message: dict):
        # "매핑 취소"/"매핑 재시작"(재시작은 서버가 취소 API 먼저 부른 뒤 slam_start를
        # 다시 보낸다) — 지금 떠 있는 cartographer_launch.py를 실제로 끈다. 이걸 안 하면
        # _start_slam이 "이미 SLAM 작업이 진행 중입니다"로 다음 시작을 계속 거부한다.
        #
        # ros2 launch 프로세스 "하나"에만 SIGINT를 보내면(subprocess.send_signal) 정작
        # 그 밑에서 실제로 도는 cartographer_node/occupancy_grid_node/rviz2는 안 죽고
        # 고아 프로세스로 남는다(실 로봇에서 확인됨) — 터미널에서 Ctrl+C를 누르면
        # 셸이 "포그라운드 프로세스 그룹 전체"에 신호를 보내기 때문에 잘 죽는 것이지,
        # ros2 launch 자체가 자식들에게 신호를 잘 전달해줘서가 아니었다. 그래서
        # os.killpg로 프로세스 그룹 전체에 SIGINT를 보내 Ctrl+C와 같은 효과를 낸다.
        if self._slam_process is None or self._slam_process.poll() is not None:
            self.get_logger().warn('취소할 SLAM 작업이 없습니다')
            self._slam_process = None
            return

        pgid = os.getpgid(self._slam_process.pid)
        try:
            os.killpg(pgid, signal.SIGINT)
            self._slam_process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.get_logger().warn('SLAM 프로세스가 5초 내에 안 죽어 강제 종료합니다')
            try:
                os.killpg(pgid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self._slam_process.wait(timeout=5.0)
        except ProcessLookupError:
            pass  # 이미 죽어 있었음
        self._slam_process = None

    def _save_map(self, message: dict):
        # "SLAM 맵 저장·배포" 버튼(P5MapPage) -> POST /slam-jobs/{id}/save ->
        # 여기로 온다. cartographer가 아직 돌고 있어야(=/map이 살아있어야) map_saver_cli가
        # 지금까지 그린 지도를 저장할 수 있다 — 그래서 slam_stop보다 먼저 이걸 부르는
        # 순서를 대시보드/서버 쪽에서 지켜야 한다(이 노드는 순서를 강제하지 않는다).
        job_id = message.get('job_id')
        out_prefix = f'/tmp/careguard_map_job{job_id}'

        try:
            result = subprocess.run(
                ['ros2', 'run', 'nav2_map_server', 'map_saver_cli', '-f', out_prefix,
                 '--ros-args', '-p', 'save_map_timeout:=10.0'],
                capture_output=True, text=True, timeout=20,
            )
        except subprocess.TimeoutExpired:
            self.get_logger().error('map_saver_cli 실행이 20초를 넘어 중단했습니다')
            return
        if result.returncode != 0:
            self.get_logger().error(f'map_saver_cli 실패(rc={result.returncode}): {result.stderr}')
            return

        yaml_path = f'{out_prefix}.yaml'
        try:
            with open(yaml_path) as f:
                meta = yaml.safe_load(f)
            image_path = os.path.join(os.path.dirname(yaml_path), meta['image'])
            width, height = self._pgm_dimensions(image_path)
        except Exception as exc:  # noqa: BLE001 - 저장 직후 파싱 실패는 그대로 로그만 남긴다
            self.get_logger().error(f'저장된 맵 메타데이터 읽기 실패: {exc}')
            return

        try:
            file_id = media_uploader.upload_map_image(image_path)
        except Exception as exc:  # noqa: BLE001 - 업로드 실패해도 프로세스는 계속 떠 있어야 한다
            self.get_logger().error(f'맵 이미지 업로드 실패: {exc}')
            return

        origin = meta.get('origin', [0.0, 0.0, 0.0])
        self._send({
            'type': 'slam_saved',
            'job_id': job_id,
            'file_id': file_id,
            'resolution': meta.get('resolution'),
            'origin_x': origin[0],
            'origin_y': origin[1],
            'origin_yaw': origin[2] if len(origin) > 2 else 0.0,
            'width_px': width,
            'height_px': height,
            'raw_yaml': meta,
        })
        self.get_logger().info(f'맵 저장·업로드 완료: job_id={job_id} file_id={file_id}')

    @staticmethod
    def _pgm_dimensions(path: str):
        """PGM(P2/P5) 헤더에서 width/height만 읽는다 — 전체 픽셀을 읽을 필요는 없다."""
        with open(path, 'rb') as f:
            tokens = []
            while len(tokens) < 3:
                token = b''
                ch = f.read(1)
                while ch.isspace():
                    ch = f.read(1)
                if ch == b'#':
                    while ch not in (b'\n', b''):
                        ch = f.read(1)
                    continue
                while ch and not ch.isspace():
                    token += ch
                    ch = f.read(1)
                if token:
                    tokens.append(token)
            _magic, width, height = tokens
            return int(width), int(height)


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
