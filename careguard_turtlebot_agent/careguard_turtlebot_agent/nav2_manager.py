"""careguard_turtlebot_agent/nav2_manager.py

Nav2(nav2_bringup bringup_launch.py) 기동·종료, 서버에서 받은 맵 다운로드,
초기 위치 발행, 목표 지점 주행을 담당한다 — downlink_node.py에 몰려있던
책임을 분리했다(2026-09-15 리팩터링). LIMO(robot_careguard_ws/src/careguard_nav)
에 해당하는 역할이지만 그 패키지에 의존하지 않고 필요한 만큼만 자체
구현한다 — turtlebot3_navigation2가 실 로봇(R-05/R-06)에 빌드·설치돼 있지
않아(2026-09-13 SSH로 확인) nav2_bringup 표준 launch + turtlebot3 burger
파라미터를 대신 쓴다(환경변수 CAREGUARD_NAV2_PARAMS_FILE로 바꿀 수 있다).
"""
import math
import os
import signal
import subprocess
import time
from typing import Callable, Optional, Tuple

import requests
import yaml
from geometry_msgs.msg import PoseStamped, PoseWithCovarianceStamped
from nav2_simple_commander.robot_navigator import BasicNavigator, TaskResult
from rclpy.node import Node

from . import ws_config

# 서버에서 받아온 맵을 저장해둘 자리 — careguard_agent(LIMO)의 NAV_MAP_DIR과
# 같은 이유(SLAM 산출물이 아니라 순찰용으로 서버에서 도로 받아온 사본)로 이름을
# 분리했다. 워크스페이스 경로가 로봇마다 달라서(R-05: ~/turtlebot_ws, R-06:
# ~/careguard_turtlebot_agent_ws) 홈 디렉터리 기준 고정 경로로 둔다.
NAV_MAP_DIR = os.path.expanduser('~/careguard_turtlebot_nav_maps')

# nav2_bringup bringup_launch.py에 넘길 파라미터 파일 — turtlebot3_navigation2
# 소스 트리의 burger.yaml을 그대로 쓴다(모듈 docstring 참고). 로봇마다
# 워크스페이스 이름이 달라도 이 상대 경로는 R-05/R-06 둘 다 동일함을 실
# 로봇에서 확인했다.
NAV2_PARAMS_FILE = os.environ.get(
    'CAREGUARD_NAV2_PARAMS_FILE',
    os.path.expanduser('~/turtlebot3_ws/src/turtlebot3/turtlebot3_navigation2/param/burger.yaml'))

# amcl이 /amcl_pose를 advertise할 때까지 기다리는 최대 시간. careguard_agent와
# 같은 이유로(실 로봇 미검증) 넉넉히 잡는다.
NAV2_READY_TIMEOUT_SEC = 30.0

# AMCL의 /initialpose 구독이 실제로 맺어질 때까지 기다리는 최대 시간 —
# careguard_nav/initial_pose_setter.py와 같은 이유(디스커버리 직후 첫 메시지
# 유실 문제, 그 파일 docstring 참고)로 여러 번 나눠 발행한다.
INITIAL_POSE_PUBLISH_ROUNDS = 15
INITIAL_POSE_PUBLISH_INTERVAL_SEC = 0.2


class Nav2Manager:
    """Nav2 프로세스 하나를 소유하고 생명주기를 관리한다. node는 퍼블리셔
    생성(/initialpose)·로깅·/amcl_pose 퍼블리셔 존재 확인(count_publishers)에만
    쓰고, 주행 명령 자체는 별도 노드로 뜨는 BasicNavigator로 한다(원본
    downlink_node.py 구조를 그대로 유지) — on_status 콜백으로 Nav2 기동
    성공/실패를 호출부(서버 nav_status 보고 등)에 알린다.
    """

    def __init__(self, node: Node, on_status: Optional[Callable[[bool], None]] = None):
        self._node = node
        self._logger = node.get_logger()
        self._on_status = on_status
        self._navigator = BasicNavigator()
        self._process: Optional[subprocess.Popen] = None
        self._map_id = 0

    @property
    def is_running(self) -> bool:
        return self._process is not None and self._process.poll() is None

    def ensure_running(self, map_id: int, init_x: float, init_y: float, init_yaw: float) -> bool:
        """이 map_id로 이미 떠있는지 확인하고, 아니면 새로 띄운다."""
        try:
            yaml_path, changed = self._download_map(map_id)
        except Exception as exc:  # noqa: BLE001 - 다운로드 실패 원인이 다양해 전부 여기서 로그만 남긴다
            self._logger.error(f'맵 다운로드 실패: {exc}')
            self._report_status(False)
            return False

        if self.is_running:
            if self._map_id == map_id and not changed:
                self._report_status(True)
                return True  # 이미 이 맵으로, 내용도 그대로 떠있다 — 재사용
            self._logger.info('맵이 바뀌어 기존 Nav2를 종료하고 다시 띄웁니다')
            self._terminate_process_group(self._process, 'Nav2')
            self._process = None
            self._map_id = 0

        self._logger.info(f'Nav2 시작: map_id={map_id} ({yaml_path})')
        self._process = subprocess.Popen(
            ['ros2', 'launch', 'nav2_bringup', 'bringup_launch.py',
             f'map:={yaml_path}', f'params_file:={NAV2_PARAMS_FILE}', 'use_sim_time:=False'],
            start_new_session=True)
        self._map_id = map_id

        deadline = time.monotonic() + NAV2_READY_TIMEOUT_SEC
        ready = False
        while time.monotonic() < deadline:
            if self._node.count_publishers('/amcl_pose') > 0:
                ready = True
                break
            time.sleep(0.5)
        if not ready:
            self._logger.error(f'{NAV2_READY_TIMEOUT_SEC:.0f}초 안에 Nav2(amcl)가 안 떴습니다')
            self._report_status(False)
            return False

        self.publish_initial_pose(init_x, init_y, init_yaw)
        self._report_status(True)
        return True

    def go_to_pose_and_wait(self, x: float, y: float, yaw: float, cancel_requested) -> bool:
        """목표 지점까지 주행하고 끝날 때까지 대기한다. 성공하면 True, 취소되거나
        실패하면 False. cancel_requested는 threading.Event — 이 스레드 하나만
        self._navigator를 건드린다는 전제를 유지해야 한다(cancelTask/
        isTaskComplete를 여러 스레드에서 동시에 부르면 경쟁 상태가 생길 수
        있다) — patrol_controller.py가 이 전제를 지켜 호출한다.
        """
        self._navigator.goToPose(self._to_pose_stamped(x, y, yaw))
        while not self._navigator.isTaskComplete():
            if cancel_requested.is_set():
                self._navigator.cancelTask()
                return False
            time.sleep(0.2)
        return self._navigator.getResult() == TaskResult.SUCCEEDED

    def publish_initial_pose(self, x: float, y: float, yaw: float):
        """구독자가 실제로 잡힐 때까지 기다렸다가 여러 번 나눠 발행한다 —
        careguard_nav/initial_pose_setter.py::publish_when_ready와 같은 이유
        (디스커버리 직후 첫 메시지 유실 문제).
        """
        pub = self._node.create_publisher(PoseWithCovarianceStamped, '/initialpose', 10)
        deadline = time.monotonic() + 5.0
        while pub.get_subscription_count() == 0 and time.monotonic() < deadline:
            time.sleep(0.1)
        if pub.get_subscription_count() == 0:
            self._logger.warn('AMCL의 /initialpose 구독을 못 찾았습니다 — 그래도 발행은 시도합니다')

        msg = PoseWithCovarianceStamped()
        msg.header.frame_id = 'map'
        msg.pose.pose.position.x = x
        msg.pose.pose.position.y = y
        msg.pose.pose.orientation.z = math.sin(yaw / 2.0)
        msg.pose.pose.orientation.w = math.cos(yaw / 2.0)
        for _ in range(INITIAL_POSE_PUBLISH_ROUNDS):
            msg.header.stamp = self._node.get_clock().now().to_msg()
            pub.publish(msg)
            time.sleep(INITIAL_POSE_PUBLISH_INTERVAL_SEC)
        self._logger.info(f'초기 위치를 충전 스테이션 좌표({x:.2f}, {y:.2f})로 설정했습니다')
        self._node.destroy_publisher(pub)

    def _report_status(self, running: bool):
        if self._on_status is not None:
            self._on_status(running)

    def _to_pose_stamped(self, x: float, y: float, yaw: float) -> PoseStamped:
        pose = PoseStamped()
        pose.header.frame_id = 'map'
        pose.header.stamp = self._navigator.get_clock().now().to_msg()
        pose.pose.position.x = x
        pose.pose.position.y = y
        pose.pose.orientation.z = math.sin(yaw / 2.0)
        pose.pose.orientation.w = math.cos(yaw / 2.0)
        return pose

    def _download_map(self, map_id: int) -> Tuple[str, bool]:
        """GET /maps/active에서 이 로봇의 활성 맵 이미지+메타데이터를 받아
        로컬에 저장하고 (yaml 경로, 내용이 바뀌었는지)를 반환한다.
        """
        resp = requests.get(f'{ws_config.SERVER_HTTP_URL}/maps/active', timeout=10)
        resp.raise_for_status()
        entry = next((m for m in resp.json() if m.get('robot_code') == ws_config.ROBOT_CODE), None)
        if entry is None or not entry.get('image_url'):
            raise RuntimeError(f'서버에 robot_code={ws_config.ROBOT_CODE}의 활성 맵 이미지가 없습니다')
        if entry.get('id') != map_id:
            raise RuntimeError(
                f'요청받은 map_id={map_id}가 서버의 현재 활성 맵(id={entry.get("id")})과 다릅니다')

        os.makedirs(NAV_MAP_DIR, exist_ok=True)
        pgm_path = os.path.join(NAV_MAP_DIR, f'nav_map_{map_id}.pgm')
        yaml_path = os.path.join(NAV_MAP_DIR, f'nav_map_{map_id}.yaml')

        image_resp = requests.get(f"{ws_config.SERVER_HTTP_URL}{entry['image_url']}", timeout=30)
        image_resp.raise_for_status()
        new_image_bytes = image_resp.content

        yaml_content = {
            'image': os.path.basename(pgm_path),
            'mode': entry.get('mode') or 'trinary',
            'resolution': entry.get('resolution') or 0.05,
            'origin': [entry.get('origin_x', 0.0), entry.get('origin_y', 0.0), entry.get('origin_yaw', 0.0)],
            'negate': entry.get('negate', 0),
            'occupied_thresh': entry.get('occupied_thresh', 0.65),
            'free_thresh': entry.get('free_thresh', 0.196),
        }
        new_yaml_bytes = yaml.safe_dump(yaml_content, default_flow_style=None, sort_keys=False).encode()

        changed = True
        if os.path.exists(pgm_path) and os.path.exists(yaml_path):
            with open(pgm_path, 'rb') as f:
                old_image_bytes = f.read()
            with open(yaml_path, 'rb') as f:
                old_yaml_bytes = f.read()
            changed = new_image_bytes != old_image_bytes or new_yaml_bytes != old_yaml_bytes

        with open(pgm_path, 'wb') as f:
            f.write(new_image_bytes)
        with open(yaml_path, 'wb') as f:
            f.write(new_yaml_bytes)
        return yaml_path, changed

    def _terminate_process_group(self, process: subprocess.Popen, label: str):
        """ros2 launch 프로세스 하나에만 신호를 보내면 그 밑의 실제 자식
        노드들이 고아로 남는 문제를 피하려고, 프로세스 그룹 전체에 SIGINT ->
        (5초 안 죽으면) SIGKILL을 보낸다.
        """
        pgid = os.getpgid(process.pid)
        try:
            os.killpg(pgid, signal.SIGINT)
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self._logger.warn(f'{label} 프로세스가 5초 내에 안 죽어 강제 종료합니다')
            try:
                os.killpg(pgid, signal.SIGKILL)
                process.wait(timeout=5.0)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                self._logger.error(f'{label} 프로세스가 SIGKILL 후에도 안 죽었습니다')
        except ProcessLookupError:
            pass  # 이미 죽어 있었음
