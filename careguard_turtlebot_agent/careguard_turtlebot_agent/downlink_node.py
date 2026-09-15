"""careguard_turtlebot_agent/downlink_node.py

서버 -> 터틀봇 다운링크. 배터리가 임계값(복귀 필요)을 넘었을 때
robot_ws.py::_handle_battery_state가 보내는 patrol_start(waypoints=[],
return_to_charging=True)를 받아 충전 스테이션 웨이포인트까지 Nav2로
주행하고, P6 "경로 주행"이 보내는 patrol_start(waypoints=[...])를 받으면
그 웨이포인트들을 순서대로 돈 뒤(2026-09-14부터, _run_patrol) 충전소로
복귀한다.

LIMO(robot_careguard_ws/src/careguard_agent/downlink_node.py)는 SLAM·순찰(다중
웨이포인트)·비디오 녹화·음성 안내까지 로봇 소프트웨어 전체를 관리하는 훨씬 큰
노드이고, careguard_bringup/careguard_mission/careguard_nav라는 LIMO 전용 워크스페이스
패키지에 의존한다. 이 패키지들은 터틀봇 워크스페이스엔 없고, 그걸 통째로 옮기는 건
이번 스코프가 아니다(2026-09-13 논의 — 우선 "배터리 낮으면 충전소로 실제로
돌아간다"만 되게 하고, 다중 웨이포인트 순찰은 나중에 필요해지면 별도로 붙인다).
그래서 이 노드는 careguard_nav를 가져다 쓰지 않고, 필요한 만큼만(맵 다운로드 +
Nav2 기동 + 초기 위치 + 목적지 주행) 자체적으로 구현한다.

Nav2 기동은 `nav2_bringup bringup_launch.py`(map_server+amcl+navigation을 한 번에
묶어 실행, autostart 기본값 true라 라이프사이클 수동 활성화도 필요 없다)를 쓴다 —
애초에 `turtlebot3_navigation2` launch로 하려 했는데, 2026-09-13 R-05/R-06 실 로봇에
SSH로 직접 확인해보니 그 패키지는 소스만 있고(`~/turtlebot3_ws/src/turtlebot3/
turtlebot3_navigation2`) 빌드·설치는 안 돼 있었다(`ros2 pkg list`에 없음) — 대신
`nav2_bringup`은 두 로봇 다 설치돼 있어서 이걸로 바꿨다. 파라미터 파일은 그
turtlebot3_navigation2 소스 트리에 있는 `param/burger.yaml`을 그대로 가리킨다(두
로봇 다 같은 상대 경로 `~/turtlebot3_ws/src/turtlebot3/turtlebot3_navigation2/param/
burger.yaml`에 있음을 실 로봇에서 확인함) — 환경변수 CAREGUARD_NAV2_PARAMS_FILE로
바꿀 수 있다.

2026-09-14: patrol_start에 실제 웨이포인트 목록이 든 "진짜 순찰" 요청 처리를
추가했다(_run_patrol) — LIMO(careguard_mission/patrol_task.py)는 이걸 별도
액션 서버로 구현하고 영상 녹화(careguard_perception)·음성 안내
(careguard_hmi)까지 같이 트리거하는데, 그 패키지들은 터틀봇 워크스페이스에
없다(위 문단 참고). 그래서 상태 기계(웨이포인트를 순서대로 돌고, 하나라도
실패하면 즉시 중단, 끝나면 충전소로 복귀, patrol_progress/patrol_done
보고)만 이 노드 안에 직접 옮겨왔고, 영상·음성 연동은 옮기지 않았다(터틀봇엔
그 기능 자체가 없어서 재현 불가능) — _drive_to_charge와 목적지 하나로
가는 로직(goToPose+대기+취소확인)을 공유하려고 _go_to_pose_and_wait로
빼냈다. "이동"(단일 지점, return_to_charging=False — P6 "이동"/SOS 자동
출동)은 여전히 스코프 밖이라 경고만 남기고 무시한다.

patrol_start 처리가 없어서 "명령이 어디로 갔는지 모르는" 상황이 재발하지
않도록(2026-09-13 트러블슈팅에서 발견된 문제), 아직 처리 못 하는 조합은
조용히 무시하지 않고 경고 로그를 남긴다.

2026-09-14: P6 "초기 위치 설정"(set_initial_pose)이 터틀봇에서 아무 반응이 없다는
문제로 nav_prepare/set_initial_pose 핸들러를 추가했다 — LIMO(careguard_agent)
쪽 구현은 careguard_nav/careguard_mission 같은 LIMO 전용 패키지에 의존해 그대로
옮길 수 없어서, 이미 이 노드에 있던 _ensure_navigation_running/
_publish_initial_pose(둘 다 외부 패키지 없이 자체 구현됨)를 그대로 재사용해
얇은 핸들러만 새로 달았다.

빌드/실행 미검증 — nav2_simple_commander.BasicNavigator API는 실 로봇(R-05/R-06)
에서 한 번 맞춰봐야 한다(robot_careguard_ws의 같은 표시가 있는 코드들과 같은
이유). nav2_bringup/nav2_simple_commander 패키지 자체가 설치돼 있는지와
burger.yaml 경로는 2026-09-13 SSH로 직접 확인했다.
"""
import asyncio
import json
import math
import os
import signal
import subprocess
import threading
import time
from typing import Optional, Tuple

import cv2
import numpy as np
import rclpy
import requests
import websockets
import yaml
from geometry_msgs.msg import PoseStamped, PoseWithCovarianceStamped, Twist
from nav2_simple_commander.robot_navigator import BasicNavigator, TaskResult
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage

from . import ws_config

RECONNECT_DELAY_SEC = 3.0

# 충전소 도킹 정렬(ArUco) — 2026-09-15, R-06 충전소에 붙인 마커로 Nav2 도착 후
# 마지막 접근을 시각 서보로 정밀 정렬한다. 이 로봇의 카메라(/camera/camera_info)는
# 캘리브레이션이 안 돼 있어(k/d 전부 0) solvePnP 기반 3D 포즈 추정은 못 쓰고,
# 이미지 평면상 마커 중심의 좌우 오프셋(조향)과 마커가 보이는 픽셀 폭(거리 proxy,
# 가까울수록 커짐)만으로 정렬하는 image-based visual servo로 간다 — 카메라 캘리브레이션
# 이 스코프 밖이라는 판단(2026-09-15 논의).
ARUCO_DICT = cv2.aruco.DICT_4X4_50
ARUCO_MARKER_ID = 17
ARUCO_MAX_LINEAR_MPS = 0.08
ARUCO_MAX_ANGULAR_RADPS = 0.6
# 마커 바운딩박스 폭이 이 픽셀 수 이상으로 보이면 "충분히 접근했다"고 보고 멈춘다 —
# 카메라 캘리브레이션이 없어 실측 거리(cm)로 환산할 수 없으므로, 실제 로봇에서 충전
# 단자가 닿는 시점의 마커 크기를 보고 튜닝해야 하는 값이다(2026-09-15 시점 추정치,
# 첫 실기 테스트 후 조정 필요).
ARUCO_DOCK_CONTACT_WIDTH_PX = 220
# 마커가 화면 중심에서 이 비율(가로폭 대비) 이상 벗어나 있으면 전진하지 않고 제자리
# 회전으로 먼저 정렬한다 — 크게 틀어진 채로 전진하면 마커를 화면 밖으로 놓치기 쉽다.
ARUCO_CENTER_DEADBAND_RATIO = 0.35
ARUCO_LOST_MARKER_TIMEOUT_SEC = 6.0
ARUCO_DOCK_TIMEOUT_SEC = 25.0
ARUCO_CONTROL_INTERVAL_SEC = 0.1

# 서버에서 받아온 맵을 저장해둘 자리 — careguard_agent(LIMO)의 NAV_MAP_DIR과
# 같은 이유(SLAM 산출물이 아니라 순찰용으로 서버에서 도로 받아온 사본)로 이름을
# 분리했다. 워크스페이스 경로가 로봇마다 달라서(R-05: ~/turtlebot_ws, R-06:
# ~/careguard_turtlebot_agent_ws) 홈 디렉터리 기준 고정 경로로 둔다.
NAV_MAP_DIR = os.path.expanduser('~/careguard_turtlebot_nav_maps')

# nav2_bringup bringup_launch.py에 넘길 파라미터 파일 — turtlebot3_navigation2
# 소스 트리의 burger.yaml을 그대로 쓴다(모듈 docstring 참고). 로봇마다 워크스페이스
# 이름이 달라도 이 상대 경로는 R-05/R-06 둘 다 동일함을 실 로봇에서 확인했다.
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


class DownlinkNode(Node):
    def __init__(self):
        super().__init__('downlink_node')
        self._navigator = BasicNavigator()
        self._nav_process: Optional[subprocess.Popen] = None
        self._nav_map_id = 0
        # 지금 목적지로 가고 있는 중인지, 그리고 patrol_stop이 그 주행을 취소하라고
        # 요청했는지 — careguard_nav/waypoint_manager.py::_go_through와 같은 패턴
        # (단일 스레드만 self._navigator를 건드리게 해서, cancelTask()/isTaskComplete()
        # 를 여러 스레드에서 동시에 부르는 경쟁 상태를 원천적으로 피한다).
        self._nav_active = False
        self._cancel_requested = threading.Event()
        self._nav_thread: Optional[threading.Thread] = None

        self._ws = None
        self._loop = None
        self._thread = threading.Thread(target=self._run_ws_loop, daemon=True)
        self._thread.start()

        # 충전소 도킹(ArUco) — 카메라 프레임은 uplink_node도 독립적으로 같은 토픽을
        # 구독해 서버로 relay하고 있어 구독자가 둘이어도 문제없다(ROS2 토픽은 다대다).
        self._latest_frame_lock = threading.Lock()
        self._latest_frame: Optional[np.ndarray] = None
        self.create_subscription(CompressedImage, '/camera/image_raw/compressed',
                                  self._on_camera_frame_for_dock, qos_profile_sensor_data)
        self._cmd_vel_pub = self.create_publisher(Twist, '/cmd_vel', 10)
        self._aruco_dictionary = cv2.aruco.Dictionary_get(ARUCO_DICT)
        self._aruco_params = cv2.aruco.DetectorParameters_create()

    def _run_ws_loop(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect_forever())

    async def _connect_forever(self):
        # role=downlink — careguard_agent/downlink_node.py와 같은 이유로 명시한다
        # (생략해도 서버 기본값이 downlink라 동작은 하지만, uplink_node의 연결과
        # 명확히 구분해두는 편이 안전하다 — robot_ws.py의 _connections 주석 참고).
        url = f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}&role=downlink'
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
        if self._ws is None or self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(self._ws.send(json.dumps(payload)), self._loop)

    def _send_nav_status(self, running: bool):
        self._send({'type': 'nav_status', 'running': running})

    def _handle_command(self, raw: str):
        try:
            message = json.loads(raw)
        except ValueError:
            self.get_logger().warn(f'명령 JSON 파싱 실패: {raw!r}')
            return

        msg_type = message.get('type')
        if msg_type == 'patrol_start':
            self._start_patrol(message)
        elif msg_type == 'patrol_stop':
            self._stop_patrol(message)
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
            charge_x, charge_y, charge_yaw = self._fetch_charge_waypoint(map_id)
        except Exception as exc:  # noqa: BLE001 - 원인이 다양해(네트워크/DB/웨이포인트 없음 등) 전부 여기서 로그만 남긴다
            self.get_logger().error(f'nav_prepare용 충전 웨이포인트 조회 실패: {exc}')
            return
        self._ensure_navigation_running(map_id, charge_x, charge_y, charge_yaw)

    def _handle_set_initial_pose(self, message: dict):
        """P6 "위치 보정" — 지도 클릭+드래그로 사용자가 직접 지정한 위치·방향으로
        AMCL 초기 위치를 즉석에서 다시 잡는다.
        careguard_agent/downlink_node.py::_set_initial_pose와 같은 목적이지만,
        그쪽은 careguard_nav 패키지를 별도 프로세스(`ros2 run careguard_nav
        initial_pose_setter`)로 불러 처리하는데 터틀봇 워크스페이스엔 그 패키지가
        없다 — 대신 이 노드가 이미 자체 구현해둔 _publish_initial_pose를 그대로
        쓴다(patrol_start가 충전 복귀 후 초기 위치를 잡을 때 쓰는 것과 동일한
        경로). Nav2 자체는 이미 떠 있어야 한다(여기서 새로 켜지 않음).
        """
        if self._nav_process is None or self._nav_process.poll() is not None:
            self.get_logger().warn('Nav2가 떠있지 않아 초기 위치를 보정할 수 없습니다')
            return
        x = float(message.get('x', 0.0) or 0.0)
        y = float(message.get('y', 0.0) or 0.0)
        yaw = float(message.get('yaw', 0.0) or 0.0)
        self._publish_initial_pose(x, y, yaw)

    def _start_patrol(self, message: dict):
        waypoints = message.get('waypoints') or []
        return_to_charging = bool(message.get('return_to_charging', True))
        if not waypoints and not return_to_charging:
            # "이동"(단일 지점, return_to_charging=False) — P6 "이동" 버튼/SOS 자동
            # 출동이 이 조합을 보낸다. 여전히 스코프 밖(모듈 docstring 참고).
            self.get_logger().warn(
                '터틀봇은 아직 단일 지점 "이동"을 지원하지 않습니다 — '
                '웨이포인트 순찰 또는 배터리 자동 복귀(빈 웨이포인트+return_to_charging)만 처리합니다')
            return

        if self._nav_active:
            self.get_logger().warn('이미 주행 중입니다 — 새 요청을 무시합니다')
            return

        map_id = int(message.get('map_id', 0) or 0)
        if map_id <= 0:
            self.get_logger().error('patrol_start에 유효한 map_id가 없습니다')
            return

        try:
            charge_x, charge_y, charge_yaw = self._fetch_charge_waypoint(map_id)
        except Exception as exc:  # noqa: BLE001 - 원인이 다양해(네트워크/DB/웨이포인트 없음 등) 전부 여기서 로그만 남긴다
            self.get_logger().error(f'충전 웨이포인트 조회 실패: {exc}')
            return

        if not self._ensure_navigation_running(map_id, charge_x, charge_y, charge_yaw):
            self.get_logger().error('Nav2 준비 실패로 주행을 시작하지 않습니다')
            return

        self._cancel_requested.clear()
        self._nav_active = True
        run_id = int(message.get('run_id', 0) or 0)
        if waypoints:
            self._nav_thread = threading.Thread(
                target=self._run_patrol,
                args=(run_id, waypoints, return_to_charging, charge_x, charge_y, charge_yaw),
                daemon=True)
        else:
            self._nav_thread = threading.Thread(
                target=self._drive_to_charge, args=(charge_x, charge_y, charge_yaw), daemon=True)
        self._nav_thread.start()

    def _go_to_pose_and_wait(self, x: float, y: float, yaw: float) -> bool:
        """목표 지점까지 Nav2로 주행하고 끝날 때까지 대기한다. 성공하면 True,
        취소되거나 실패하면 False. careguard_nav/waypoint_manager.py::_go_through와
        같은 패턴 — 이 스레드 하나만 self._navigator를 건드린다(cancelTask/
        isTaskComplete를 여러 스레드에서 동시에 부르면 경쟁 상태가 생길 수 있어서
        patrol_stop은 직접 cancelTask()를 부르지 않고 이 스레드에 취소만
        요청한다). _drive_to_charge(충전 복귀 단독 주행)와 _run_patrol(여러
        웨이포인트 순찰)이 이 헬퍼를 공유한다.
        """
        self._navigator.goToPose(self._to_pose_stamped(x, y, yaw))
        while not self._navigator.isTaskComplete():
            if self._cancel_requested.is_set():
                self._navigator.cancelTask()
                return False
            time.sleep(0.2)
        return self._navigator.getResult() == TaskResult.SUCCEEDED

    def _drive_to_charge(self, x: float, y: float, yaw: float):
        try:
            if self._cancel_requested.is_set():
                return
            success = self._go_to_pose_and_wait(x, y, yaw)
            if self._cancel_requested.is_set():
                self.get_logger().info('충전소 복귀 주행이 취소됐습니다')
            else:
                self.get_logger().info(f'충전소 복귀 결과: {"성공" if success else "실패"}')
                if success:
                    self._dock_with_aruco()
        finally:
            self._nav_active = False

    def _run_patrol(self, run_id: int, waypoints: list, return_to_charging: bool,
                     charge_x: float, charge_y: float, charge_yaw: float):
        """careguard_mission/patrol_task.py::_execute(LIMO)와 같은 상태 기계를
        가져오되, 그쪽이 액션 서버 하나로 같이 처리하는 영상 녹화
        (careguard_perception)·음성 안내(careguard_hmi)는 뺐다 — 둘 다 LIMO 전용
        워크스페이스 패키지라 터틀봇엔 없다(모듈 docstring 참고). 웨이포인트를
        순서대로 돌다가 하나라도 실패하면 즉시 중단하고(LIMO도 같은 이유로
        2026-09-08에 고친 부분 — 실패해도 조용히 다음으로 넘어가면 "순찰 완료"로
        잘못 기록된다), 끝나면(return_to_charging이면) 충전소로 복귀한다. 진행률은
        patrol_progress/patrol_done으로 서버에 보고한다(robot_ws.py가 DB의
        patrol_run 행과 대시보드로 그대로 릴레이).
        """
        try:
            waypoints_done = 0
            for wp in waypoints:
                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self.get_logger().info('순찰이 취소됐습니다')
                    return

                success = self._go_to_pose_and_wait(
                    float(wp['x']), float(wp['y']), float(wp.get('yaw', 0.0) or 0.0))

                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self.get_logger().info(f"{wp.get('code', '?')} 이동 중 취소됨")
                    return
                if not success:
                    self.get_logger().error(f"{wp.get('code', '?')} 이동 실패 — 순찰을 중단합니다")
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'failed'})
                    return

                waypoints_done += 1
                self._send({'type': 'patrol_progress', 'run_id': run_id, 'waypoints_done': waypoints_done})

            if return_to_charging:
                success = self._go_to_pose_and_wait(charge_x, charge_y, charge_yaw)
                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self.get_logger().info('충전소 복귀 중 취소됨')
                    return
                if not success:
                    self.get_logger().error('충전소 복귀 실패')
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'failed'})
                    return
                self._dock_with_aruco()

            self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'completed'})
            self.get_logger().info(f'순찰 완료: run_id={run_id}')
        finally:
            self._nav_active = False

    def _stop_patrol(self, message: dict):
        if not self._nav_active:
            self.get_logger().warn('취소할 진행 중인 주행이 없습니다')
            return
        self._cancel_requested.set()

    def _on_camera_frame_for_dock(self, msg: CompressedImage):
        # uplink_node도 이 토픽을 독립적으로 구독해 relay한다 — 여기선 디코딩한
        # BGR 이미지를 도킹 루프가 쓸 수 있게 저장만 해둔다(relay와는 무관).
        if not msg.data:
            return
        image = cv2.imdecode(np.frombuffer(bytes(msg.data), dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            return
        with self._latest_frame_lock:
            self._latest_frame = image

    def _publish_cmd_vel(self, linear_x: float, angular_z: float):
        twist = Twist()
        twist.linear.x = linear_x
        twist.angular.z = angular_z
        self._cmd_vel_pub.publish(twist)

    def _dock_with_aruco(self):
        """Nav2가 충전 웨이포인트까지 도착한 뒤, ArUco 마커(DICT_4X4_50, id=ARUCO_MARKER_ID)로
        마지막 접근을 시각 서보로 정렬한다 — Nav2의 목표 도달 반경(기본 수십 cm)만으론
        충전 단자를 실제로 맞대기엔 부정확해서 이 단계가 필요하다(2026-09-15 요청).

        카메라 캘리브레이션이 없어(모듈 상단 상수 주석 참고) 이미지 평면 기준으로만
        제어한다: 마커 중심의 좌우 픽셀 오프셋으로 조향하고, 마커 바운딩박스 폭을
        거리 대용으로 써서 충분히 커지면(ARUCO_DOCK_CONTACT_WIDTH_PX) 접촉으로 보고
        멈춘다. 마커를 한 번도 못 보거나 놓친 채로 오래(ARUCO_LOST_MARKER_TIMEOUT_SEC)
        지나면, 또는 전체 시간이 ARUCO_DOCK_TIMEOUT_SEC를 넘으면 안전하게 정지하고
        포기한다 — 마커 없이 추측 주행(dead reckoning)으로 충전소에 부딪히는 상황을
        피하려는 것이다. patrol_stop(취소 요청)도 즉시 반영한다.
        """
        self.get_logger().info('ArUco 충전소 도킹 정렬을 시작합니다')
        start = time.monotonic()
        last_seen = start
        docked = False
        try:
            while True:
                now = time.monotonic()
                if self._cancel_requested.is_set():
                    self.get_logger().info('도킹 정렬이 취소됐습니다')
                    return
                if now - start > ARUCO_DOCK_TIMEOUT_SEC:
                    self.get_logger().error('도킹 정렬 제한 시간 초과 — 포기합니다')
                    return
                if now - last_seen > ARUCO_LOST_MARKER_TIMEOUT_SEC:
                    self.get_logger().error('충전소 ArUco 마커를 놓쳐 도킹 정렬을 포기합니다')
                    return

                with self._latest_frame_lock:
                    frame = None if self._latest_frame is None else self._latest_frame.copy()
                if frame is None:
                    time.sleep(ARUCO_CONTROL_INTERVAL_SEC)
                    continue

                gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                corners, ids, _ = cv2.aruco.detectMarkers(
                    gray, self._aruco_dictionary, parameters=self._aruco_params)

                marker_corners = None
                if ids is not None:
                    for c, marker_id in zip(corners, ids.flatten()):
                        if int(marker_id) == ARUCO_MARKER_ID:
                            marker_corners = c[0]  # (4, 2)
                            break

                if marker_corners is None:
                    self._publish_cmd_vel(0.0, 0.0)
                    time.sleep(ARUCO_CONTROL_INTERVAL_SEC)
                    continue

                last_seen = now
                center_x = float(np.mean(marker_corners[:, 0]))
                width_px = float(np.linalg.norm(marker_corners[0] - marker_corners[1]))
                image_width = frame.shape[1]
                offset_ratio = (center_x - image_width / 2.0) / (image_width / 2.0)  # -1..1

                if width_px >= ARUCO_DOCK_CONTACT_WIDTH_PX:
                    self.get_logger().info(f'충전소 마커 접근 완료(폭 {width_px:.0f}px) — 도킹 정렬 종료')
                    docked = True
                    return

                angular_z = max(-ARUCO_MAX_ANGULAR_RADPS,
                                 min(ARUCO_MAX_ANGULAR_RADPS, -offset_ratio * ARUCO_MAX_ANGULAR_RADPS))
                if abs(offset_ratio) > ARUCO_CENTER_DEADBAND_RATIO:
                    # 많이 틀어져 있으면 제자리 정렬부터 — 틀어진 채 전진하면 마커가
                    # 화면 밖으로 나가버리기 쉽다.
                    linear_x = 0.0
                else:
                    # 마커 폭이 목표에 가까워질수록(=가까워질수록) 속도를 줄인다.
                    approach_ratio = max(0.0, 1.0 - width_px / ARUCO_DOCK_CONTACT_WIDTH_PX)
                    linear_x = ARUCO_MAX_LINEAR_MPS * approach_ratio
                self._publish_cmd_vel(linear_x, angular_z)
                time.sleep(ARUCO_CONTROL_INTERVAL_SEC)
        finally:
            self._publish_cmd_vel(0.0, 0.0)
            self._send({'type': 'dock_result', 'docked': docked})

    def _to_pose_stamped(self, x: float, y: float, yaw: float) -> PoseStamped:
        pose = PoseStamped()
        pose.header.frame_id = 'map'
        pose.header.stamp = self._navigator.get_clock().now().to_msg()
        pose.pose.position.x = x
        pose.pose.position.y = y
        pose.pose.orientation.z = math.sin(yaw / 2.0)
        pose.pose.orientation.w = math.cos(yaw / 2.0)
        return pose

    def _fetch_charge_waypoint(self, map_id: int) -> Tuple[float, float, float]:
        """GET /waypoints?map_id=에서 kind='charge'인 웨이포인트 좌표를 받아온다.
        careguard_nav/waypoint_manager.py::charging_waypoint()와 같은 조회를,
        그 패키지를 가져다 쓰지 않고 이 노드 안에서 직접 한다(파일 상단 docstring
        참고 — 터틀봇 워크스페이스에 careguard_nav가 없다).
        """
        resp = requests.get(
            f'{ws_config.SERVER_HTTP_URL}/waypoints', params={'map_id': map_id}, timeout=10)
        resp.raise_for_status()
        charge = next((w for w in resp.json() if w.get('kind') == 'charge'), None)
        if charge is None:
            raise RuntimeError(f'map_id={map_id}에 충전 스테이션 웨이포인트가 없습니다')
        return float(charge['pos_x']), float(charge['pos_y']), float(charge['yaw'])

    def _download_map(self, map_id: int) -> Tuple[str, bool]:
        """careguard_agent/downlink_node.py::_download_map과 같은 절차 —
        GET /maps/active에서 이 로봇의 활성 맵 이미지+메타데이터를 받아 로컬에
        저장하고 (yaml 경로, 내용이 바뀌었는지)를 반환한다.
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

    def _ensure_navigation_running(self, map_id: int, init_x: float, init_y: float, init_yaw: float) -> bool:
        """nav2_bringup bringup_launch.py가 이 map_id로 이미 떠있는지 확인하고,
        아니면 새로 띄운다. careguard_agent/downlink_node.py
        ::_ensure_navigation_running과 같은 구조이지만, LIMO 전용 런치
        (careguard_bringup navigation_diff_launch.py) 대신 nav2_bringup의
        표준 launch + turtlebot3 burger 파라미터를 쓴다(모듈 docstring 참고).
        """
        try:
            yaml_path, changed = self._download_map(map_id)
        except Exception as exc:  # noqa: BLE001 - 다운로드 실패 원인이 다양해 전부 여기서 로그만 남긴다
            self.get_logger().error(f'맵 다운로드 실패: {exc}')
            self._send_nav_status(False)
            return False

        if self._nav_process is not None and self._nav_process.poll() is None:
            if self._nav_map_id == map_id and not changed:
                self._send_nav_status(True)
                return True  # 이미 이 맵으로, 내용도 그대로 떠있다 — 재사용
            self.get_logger().info('맵이 바뀌어 기존 Nav2를 종료하고 다시 띄웁니다')
            self._terminate_process_group(self._nav_process, 'Nav2')
            self._nav_process = None
            self._nav_map_id = 0

        self.get_logger().info(f'Nav2 시작: map_id={map_id} ({yaml_path})')
        self._nav_process = subprocess.Popen(
            ['ros2', 'launch', 'nav2_bringup', 'bringup_launch.py',
             f'map:={yaml_path}', f'params_file:={NAV2_PARAMS_FILE}', 'use_sim_time:=False'],
            start_new_session=True)
        self._nav_map_id = map_id

        deadline = time.monotonic() + NAV2_READY_TIMEOUT_SEC
        ready = False
        while time.monotonic() < deadline:
            if self.count_publishers('/amcl_pose') > 0:
                ready = True
                break
            time.sleep(0.5)
        if not ready:
            self.get_logger().error(f'{NAV2_READY_TIMEOUT_SEC:.0f}초 안에 Nav2(amcl)가 안 떴습니다')
            self._send_nav_status(False)
            return False

        self._publish_initial_pose(init_x, init_y, init_yaw)
        self._send_nav_status(True)
        return True

    def _publish_initial_pose(self, x: float, y: float, yaw: float):
        """careguard_nav/initial_pose_setter.py::publish_when_ready와 같은
        절차(구독자가 실제로 잡힐 때까지 기다렸다가 여러 번 나눠 발행) — 별도
        프로세스로 안 부르고 이 노드 안에서 직접 한다(터틀봇엔 careguard_nav가
        없어서 그 실행 파일을 재사용할 수 없다).
        """
        pub = self.create_publisher(PoseWithCovarianceStamped, '/initialpose', 10)
        deadline = time.monotonic() + 5.0
        while pub.get_subscription_count() == 0 and time.monotonic() < deadline:
            time.sleep(0.1)
        if pub.get_subscription_count() == 0:
            self.get_logger().warn('AMCL의 /initialpose 구독을 못 찾았습니다 — 그래도 발행은 시도합니다')

        msg = PoseWithCovarianceStamped()
        msg.header.frame_id = 'map'
        msg.pose.pose.position.x = x
        msg.pose.pose.position.y = y
        msg.pose.pose.orientation.z = math.sin(yaw / 2.0)
        msg.pose.pose.orientation.w = math.cos(yaw / 2.0)
        for _ in range(INITIAL_POSE_PUBLISH_ROUNDS):
            msg.header.stamp = self.get_clock().now().to_msg()
            pub.publish(msg)
            time.sleep(INITIAL_POSE_PUBLISH_INTERVAL_SEC)
        self.get_logger().info(f'초기 위치를 충전 스테이션 좌표({x:.2f}, {y:.2f})로 설정했습니다')
        self.destroy_publisher(pub)

    def _terminate_process_group(self, process: subprocess.Popen, label: str):
        """careguard_agent/downlink_node.py::_terminate_process_group과 동일 —
        ros2 launch 프로세스 하나에만 신호를 보내면 그 밑의 실제 자식 노드들이
        고아로 남는 문제(그 파일 문서 참고)를 피하려고 프로세스 그룹 전체에
        SIGINT -> (5초 안 죽으면) SIGKILL을 보낸다.
        """
        pgid = os.getpgid(process.pid)
        try:
            os.killpg(pgid, signal.SIGINT)
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.get_logger().warn(f'{label} 프로세스가 5초 내에 안 죽어 강제 종료합니다')
            try:
                os.killpg(pgid, signal.SIGKILL)
                process.wait(timeout=5.0)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                self.get_logger().error(f'{label} 프로세스가 SIGKILL 후에도 안 죽었습니다')
        except ProcessLookupError:
            pass  # 이미 죽어 있었음


def main(args=None):
    rclpy.init(args=args)
    node = DownlinkNode()
    # rclpy.spin(node)를 인자 없이 쓰면 executor를 안 줬을 때의 기본값인
    # "프로세스 전체 공유 전역 executor"(rclpy.get_global_executor())를 쓴다.
    # 그런데 nav2_simple_commander.BasicNavigator의 goToPose/waitUntilNav2Active
    # 등도 executor를 안 넘기고 rclpy.spin_until_future_complete(self, ...)를
    # 부르므로 똑같이 그 전역 executor를 쓴다 — _run_patrol/_drive_to_charge가
    # 백그라운드 스레드에서 self._navigator.goToPose()를 부르는 순간, 메인
    # 스레드가 이미 spin_once()로 돌리고 있는 바로 그 SingleThreadedExecutor를
    # 다른 스레드가 동시에 또 돌리려다 "ValueError: generator already
    # executing"으로 매번(운 나쁜 타이밍이 아니라 항상) 즉시 깨졌다 — 크래시한
    # 스레드는 finally에서 self._nav_active를 바로 False로 되돌리는데, Nav2
    # (bt_navigator)는 이미 목표를 접수해 실제로는 계속 주행 중이라 "취소"를
    # 눌러도(_nav_active가 이미 False라) 취소할 대상 자체가 없어 로봇이 멈추지
    # 않는 문제로 실제 재현·확인했다(2026-09-14, R-06).
    #
    # 이 노드(node) 전용 executor를 명시적으로 만들어 쓰면, 전역 executor는
    # self._navigator의 spin_until_future_complete 전용으로 남아 더 이상
    # 두 스레드가 같은 executor를 동시에 돌리지 않는다.
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
