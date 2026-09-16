"""careguard_turtlebot_agent/downlink_node.py

서버 -> 터틀봇 다운링크. 배터리가 임계값(복귀 필요)을 넘었을 때
robot_ws.py::_handle_battery_state가 보내는 patrol_start(waypoints=[],
return_to_charging=True)를 받아 충전 스테이션 웨이포인트까지 Nav2로
주행하고, P6 "경로 주행"이 보내는 patrol_start(waypoints=[...])를 받으면
그 웨이포인트들을 순서대로 돈 뒤 충전소로 복귀한다.

LIMO(robot_careguard_ws/src/careguard_agent/downlink_node.py)는 SLAM·순찰(다중
웨이포인트)·비디오 녹화·음성 안내까지 로봇 소프트웨어 전체를 관리하는 훨씬 큰
노드이고, careguard_bringup/careguard_mission/careguard_nav라는 LIMO 전용 워크스페이스
패키지에 의존한다. 이 패키지들은 터틀봇 워크스페이스엔 없고, 그걸 통째로 옮기는 건
이번 스코프가 아니다(2026-09-13 논의 — 우선 "배터리 낮으면 충전소로 실제로
돌아간다"만 되게 하고, 다중 웨이포인트 순찰은 나중에 필요해지면 별도로 붙인다).
그래서 이 노드는 careguard_nav를 가져다 쓰지 않고, 필요한 만큼만(맵 다운로드 +
Nav2 기동 + 초기 위치 + 목적지 주행) 자체적으로 구현한다.

patrol_start 처리가 없어서 "명령이 어디로 갔는지 모르는" 상황이 재발하지
않도록(2026-09-13 트러블슈팅에서 발견된 문제), 아직 처리 못 하는 조합은
조용히 무시하지 않고 경고 로그를 남긴다.

2026-09-15: 파일 하나(원래 844줄 — ArUco 도킹까지 얹혀있던 시절, 마커 코드
롤백 후 601줄)에 몰려있던 책임을 클래스별 파일로 나눴다:
  - server_link.ServerLink — 서버 WebSocket 연결(재연결 루프)
  - voice.VoiceAnnouncer — Piper TTS 음성 안내
  - waypoints.fetch_charge_waypoint — 충전 스테이션 좌표 조회
  - nav2_manager.Nav2Manager — 맵 다운로드 + Nav2 기동/종료 + 초기 위치 + 주행
  - patrol_controller.PatrolController — 순찰/충전 복귀 상태 기계
이 노드는 그것들을 엮어 서버 명령을 각 객체로 라우팅하는 얇은 ROS Node
껍데기만 남는다.

빌드/실행 미검증 — nav2_simple_commander.BasicNavigator API는 실 로봇(R-05/R-06)
에서 한 번 맞춰봐야 한다. nav2_bringup/nav2_simple_commander 패키지 자체가
설치돼 있는지와 burger.yaml 경로는 2026-09-13 SSH로 직접 확인했다.
"""
import json

import rclpy
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from std_msgs.msg import String

from . import ws_config
from .nav2_manager import Nav2Manager
from .patrol_controller import PatrolController
from .server_link import ServerLink
from .voice import VoiceAnnouncer
from .waypoints import fetch_charge_waypoint


class DownlinkNode(Node):
    def __init__(self):
        super().__init__('downlink_node')

        self._link = ServerLink(
            url=f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}&role=downlink',
            on_message=self._handle_command,
            logger=self.get_logger())
        self._nav2 = Nav2Manager(self, on_status=self._send_nav_status)
        self._voice = VoiceAnnouncer(self.get_logger())
        # perception/video_recorder.py(PATROL_RECORDING_TOPIC)가 구독 — 순찰 중에도
        # 영상이 찍히게 하려면 PatrolController가 순찰 시작/종료 시점에 이걸 쏴줘야
        # 한다. PatrolController 자신은 rclpy.Node가 아니라서(순수 상태 기계) 여기
        # downlink_node가 퍼블리셔를 만들어 콜백으로 주입한다 — send(WS)를 주입하는
        # 것과 같은 패턴.
        self._patrol_recording_pub = self.create_publisher(
            String, '/careguard/perception/patrol_recording', 10)
        self._patrol = PatrolController(
            self._nav2, self._voice, self._link.send, self.get_logger(),
            publish_recording=self._publish_patrol_recording)

        self._link.start()

    def _send_nav_status(self, running: bool):
        self._link.send({'type': 'nav_status', 'running': running})

    def _publish_patrol_recording(self, payload: dict):
        self._patrol_recording_pub.publish(String(data=json.dumps(payload)))

    def _handle_command(self, raw: str):
        try:
            message = json.loads(raw)
        except ValueError:
            self.get_logger().warn(f'명령 JSON 파싱 실패: {raw!r}')
            return

        msg_type = message.get('type')
        if msg_type == 'patrol_start':
            self._patrol.start(message)
        elif msg_type == 'patrol_stop':
            self._patrol.stop(message)
        elif msg_type == 'nav_prepare':
            self._prepare_navigation(message)
        elif msg_type == 'set_initial_pose':
            self._handle_set_initial_pose(message)
        else:
            # SLAM/cmd_vel 등은 아직 이 노드가 처리하지 않는다 — 조용히 버리지
            # 않고 경고를 남겨서 "명령이 어디로 갔는지 모르겠다"는 상황(이번에
            # downlink_node 자체가 없어서 겪은 문제)이 재발하지 않게 한다.
            self.get_logger().warn(f'터틀봇 downlink가 아직 처리하지 않는 명령: {msg_type!r}')

    def _prepare_navigation(self, message: dict):
        """P6 페이지가 열리거나 다른 로봇의 맵으로 전환될 때 오는 명령 — 실제
        순찰/이동 목표 없이 Nav2만 미리 준비한다.
        careguard_agent/downlink_node.py::_prepare_navigation과 같은 목적이지만,
        video_recorder/voice_announcer/patrol_task 기동은 LIMO 전용 워크스페이스
        패키지(careguard_mission 등)에 의존해서 터틀봇엔 없다(모듈 docstring
        참고) — Nav2 준비까지만 한다.
        """
        map_id = int(message.get('map_id', 0) or 0)
        if map_id <= 0:
            self.get_logger().warn('nav_prepare에 유효한 map_id가 없습니다')
            return
        try:
            charge_x, charge_y, charge_yaw = fetch_charge_waypoint(map_id)
        except Exception as exc:  # noqa: BLE001 - 원인이 다양해(네트워크/DB/웨이포인트 없음 등) 전부 여기서 로그만 남긴다
            self.get_logger().error(f'nav_prepare용 충전 웨이포인트 조회 실패: {exc}')
            return
        self._nav2.ensure_running(map_id, charge_x, charge_y, charge_yaw)

    def _handle_set_initial_pose(self, message: dict):
        """P6 "위치 보정" — 지도 클릭+드래그로 사용자가 직접 지정한 위치·방향으로
        AMCL 초기 위치를 즉석에서 다시 잡는다.
        careguard_agent/downlink_node.py::_set_initial_pose와 같은 목적이지만,
        그쪽은 careguard_nav 패키지를 별도 프로세스(`ros2 run careguard_nav
        initial_pose_setter`)로 불러 처리하는데 터틀봇 워크스페이스엔 그 패키지가
        없다 — 대신 Nav2Manager.publish_initial_pose를 그대로 쓴다(patrol_start가
        충전 복귀 후 초기 위치를 잡을 때 쓰는 것과 동일한 경로). Nav2 자체는 이미
        떠 있어야 한다(여기서 새로 켜지 않음).
        """
        if not self._nav2.is_running:
            self.get_logger().warn('Nav2가 떠있지 않아 초기 위치를 보정할 수 없습니다')
            return
        x = float(message.get('x', 0.0) or 0.0)
        y = float(message.get('y', 0.0) or 0.0)
        yaw = float(message.get('yaw', 0.0) or 0.0)
        self._nav2.publish_initial_pose(x, y, yaw)


def main(args=None):
    rclpy.init(args=args)
    node = DownlinkNode()
    # rclpy.spin(node)를 인자 없이 쓰면 executor를 안 줬을 때의 기본값인
    # "프로세스 전체 공유 전역 executor"(rclpy.get_global_executor())를 쓴다.
    # 그런데 nav2_simple_commander.BasicNavigator의 goToPose/waitUntilNav2Active
    # 등도 executor를 안 넘기고 rclpy.spin_until_future_complete(self, ...)를
    # 부르므로 똑같이 그 전역 executor를 쓴다 — PatrolController가 백그라운드
    # 스레드에서 Nav2Manager.go_to_pose_and_wait()를 부르는 순간, 메인 스레드가
    # 이미 spin_once()로 돌리고 있는 바로 그 SingleThreadedExecutor를 다른
    # 스레드가 동시에 또 돌리려다 "ValueError: generator already executing"으로
    # 매번(운 나쁜 타이밍이 아니라 항상) 즉시 깨졌다 — 크래시한 스레드는
    # finally에서 활성 플래그를 바로 False로 되돌리는데, Nav2(bt_navigator)는
    # 이미 목표를 접수해 실제로는 계속 주행 중이라 "취소"를 눌러도(플래그가 이미
    # False라) 취소할 대상 자체가 없어 로봇이 멈추지 않는 문제로 실제
    # 재현·확인했다(2026-09-14, R-06).
    #
    # 이 노드(node) 전용 executor를 명시적으로 만들어 쓰면, 전역 executor는
    # BasicNavigator의 spin_until_future_complete 전용으로 남아 더 이상 두
    # 스레드가 같은 executor를 동시에 돌리지 않는다.
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    try:
        executor.spin()
    finally:
        executor.remove_node(node)
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
