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
import time
from typing import Tuple

import rclpy
import requests
import yaml
from geometry_msgs.msg import Twist
from rclpy.action import ActionClient
from rclpy.node import Node

from careguard_msgs.action import Patrol
from careguard_msgs.msg import Waypoint as WaypointMsg

from . import media_uploader, ws_config

RECONNECT_DELAY_SEC = 3.0

# 순찰(Nav2) 전에 서버에서 내려받은 맵 이미지+yaml을 저장해둘 자리. slam_start의
# /tmp/careguard_map_job* 임시 파일과 헷갈리지 않게 별도 이름을 쓴다 — 이건 SLAM
# 산출물이 아니라 "순찰용으로 서버에서 도로 받아온" 사본이다.
NAV_MAP_DIR = os.path.expanduser('~/robot_careguard_ws/nav_maps')

# Nav2(amcl)가 /amcl_pose를 advertise할 때까지 기다리는 최대 시간 — 실 로봇에서
# 아직 안 맞춰본 값이라(빌드/실행 미검증) 넉넉하게 잡았다.
NAV2_READY_TIMEOUT_SEC = 30.0

# patrol_task가 떠서 /patrol 액션 서버를 advertise할 때까지 기다리는 최대 시간.
# rclpy 노드 하나 뜨는 정도라 Nav2보다는 훨씬 빠르지만 여유를 둔다.
PATROL_TASK_READY_TIMEOUT_SEC = 15.0


class DownlinkNode(Node):
    def __init__(self):
        super().__init__('downlink_node')
        self._patrol_client = ActionClient(self, Patrol, 'patrol')
        self._slam_process = None
        self._nav_process = None
        self._nav_map_id = 0
        # patrol_task는 CAREGUARD_MAP_ID 환경변수로 시작 시점에 한 번만 충전 스테이션
        # 좌표를 캐시한다(WaypointManager.fetch) — 지도를 바꿀 때마다 사람이 직접 SSH로
        # 맞는 map_id를 넣어 재시작해야 했고, 깜빡하면 캐시가 비어 "순찰 후 충전소 복귀"가
        # 조용히 빈 동작으로 끝났다(2026-09-08 실 로봇에서 확인). _nav_process와 같은
        # 패턴으로 이 노드가 대신 관리한다.
        self._patrol_task_process = None
        self._patrol_task_map_id = 0
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

        # role=downlink — 서버가 이 연결을 명령 라우팅 전용 딕셔너리에 담는다(생략해도
        # 서버 기본값이 downlink라 동작은 하지만, uplink_node와 명시적으로 구분해두는
        # 편이 안전하다). server/app/ws/robot_ws.py의 _connections 주석 참고.
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
        elif msg_type == 'nav_prepare':
            self._prepare_navigation(message)
        elif msg_type == 'set_initial_pose':
            self._set_initial_pose(message)
        elif msg_type == 'shutdown_all':
            self._shutdown_everything(message)

    def _handle_cmd_vel(self, message: dict):
        twist = Twist()
        twist.linear.x = float(message.get('linear', 0.0) or 0.0)
        twist.angular.z = float(message.get('angular', 0.0) or 0.0)
        self._cmd_vel_pub.publish(twist)

    def _prepare_navigation(self, message: dict):
        """P6 페이지가 열리거나(showEvent) 다른 로봇의 맵으로 전환될 때 오는 명령 —
        실제 순찰/이동 목표 없이 Nav2(맵 다운로드+런치+초기 위치)만 미리 준비한다.
        "웨이포인트를 보내기 전에도 로봇 현재 위치부터 보고 싶다"는 요청에 대응 —
        이게 없으면 patrol_start(순찰 시작/이동)를 실제로 눌러야만 Nav2가 떠서,
        그 전까지는 대시보드에 로봇 위치가 하나도 안 보였다.
        """
        map_id = int(message.get('map_id', 0) or 0)
        if map_id <= 0:
            self.get_logger().warn('nav_prepare에 유효한 map_id가 없습니다')
            return
        self._ensure_navigation_running(map_id)
        # patrol_task도 같이 미리 띄워둔다 — 그래야 첫 "이동" 클릭 때 patrol_task
        # 기동을 기다리는 지연이 없고, CAREGUARD_MAP_ID도 항상 지금 보고 있는 맵과
        # 맞는 상태로 미리 맞춰진다.
        self._ensure_patrol_task_running(map_id)

    def _set_initial_pose(self, message: dict):
        """P6 "위치 보정" — 지도 클릭+드래그로 사용자가 직접 지정한 위치·방향으로
        AMCL 초기 위치를 즉석에서 다시 잡는다. _ensure_navigation_running이 하는
        자동 세팅(충전 스테이션 웨이포인트 좌표 가정)과 달리 여기선 map_id로 DB를
        다시 조회하지 않고 대시보드가 보낸 x/y/yaw를 그대로 쓴다 — 로봇이 실제로
        충전 스테이션이 아닌 곳에서 Nav2가 켜져 AMCL이 엉뚱한 위치로 수렴해 있을 때
        쓰는 수동 보정 경로다. Nav2 자체는 이미 떠 있어야 한다(여기서 새로 켜지 않음).
        """
        if self._nav_process is None or self._nav_process.poll() is not None:
            self.get_logger().warn('Nav2가 떠있지 않아 초기 위치를 보정할 수 없습니다')
            return

        x = float(message.get('x', 0.0) or 0.0)
        y = float(message.get('y', 0.0) or 0.0)
        yaw = float(message.get('yaw', 0.0) or 0.0)
        # initial_pose_setter를 재사용한다(careguard_agent는 서버/WS만 다루고 실제
        # Nav2 조작은 careguard_nav에 맡긴다는 기존 원칙 — _ensure_navigation_running과
        # 같은 이유). CAREGUARD_INIT_*이 있으면 그 노드가 DB 조회 대신 이 값을 그대로 쓴다.
        try:
            result = subprocess.run(
                ['ros2', 'run', 'careguard_nav', 'initial_pose_setter'],
                env={**os.environ, 'CAREGUARD_INIT_X': str(x), 'CAREGUARD_INIT_Y': str(y),
                     'CAREGUARD_INIT_YAW': str(yaw)},
                # initial_pose_setter가 재시도 발행 구간을 3번/0.6초 -> 15번/약 3초로
                # 늘렸다(DDS 디스커버리 직후 첫 메시지가 씹히는 문제 — 아래 이유 참고,
                # SUBSCRIBER_WAIT_TIMEOUT_SEC 5초 + 발행 구간 약 3초 + 여유).
                capture_output=True, text=True, timeout=20,
            )
            if result.returncode != 0:
                self.get_logger().warn(f'초기 위치 수동 보정 실패(rc={result.returncode}): {result.stderr}')
        except subprocess.TimeoutExpired:
            self.get_logger().warn('초기 위치 수동 보정이 20초를 넘어 중단했습니다')

    def _start_patrol(self, message: dict):
        # amcl(Nav2)이 안 떠있으면 순찰 자체가 불가능하다 — 예전엔 사용자가 매번
        # navigation_diff_launch.py + initial_pose_setter를 직접 손으로 켜야 했다
        # (그걸 깜빡하면 로봇 위치가 대시보드에 하나도 안 뜨는 증상으로만 보였다).
        # map_id가 오면(patrol.py::start_patrol_run/drive_to_point 둘 다 보낸다)
        # 여기서 필요시 자동으로 다 켠다.
        map_id = int(message.get('map_id', 0) or 0)
        if map_id > 0 and not self._ensure_navigation_running(map_id):
            self.get_logger().error('Nav2 준비 실패로 순찰을 시작하지 않습니다')
            return

        # patrol_task도 Nav2처럼 map_id 기준으로 (재)기동을 보장한다 — 아래 이유 참고.
        if map_id > 0:
            if not self._ensure_patrol_task_running(map_id):
                self.get_logger().error('patrol_task 준비 실패로 순찰을 시작하지 않습니다')
                return
        elif not self._patrol_client.wait_for_server(timeout_sec=2.0):
            # map_id가 없는 예전 호출부를 위한 폴백 — 이미 떠있는 patrol_task를
            # 그대로 쓴다(자동 기동은 안 함, map_id를 몰라 CAREGUARD_MAP_ID를
            # 정할 수 없다).
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
        # 서버가 안 보내면(기존 patrol_start 호출부들) True가 기본 — 원래 순찰처럼 다
        # 돌고 충전소로 복귀한다. P6 "이동"만 명시적으로 False를 실어 보낸다.
        goal.return_to_charging = bool(message.get('return_to_charging', True))
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
        if self._nav_process is not None and self._nav_process.poll() is None:
            # SLAM(Cartographer)과 Nav2(순찰용 위치추정)는 동시에 못 돈다 — 순찰
            # 하다가 매핑 시작을 누르면 여기서 먼저 Nav2를 내려준다.
            self.get_logger().info('SLAM 시작 전, 실행 중이던 Nav2를 먼저 종료합니다')
            self._terminate_process_group(self._nav_process, 'Nav2')
            self._nav_process = None
            self._nav_map_id = 0
        # _ensure_navigation_running과 같은 이유 — 이 노드가 재시작된 적이 있다면
        # 이전 생애에 띄운 SLAM/Nav2가 고아로 남아있을 수 있다. 새로 띄우기 전에
        # 항상 먼저 정리한다(_kill_external_process_group 문서 참고).
        self._kill_external_process_group('careguard_bringup cartographer_launch.py', '(정리) 이전 SLAM')
        self._kill_external_process_group('careguard_bringup navigation_diff_launch.py', '(정리) 이전 Nav2')
        # start_new_session=True로 이 프로세스를 새 프로세스 그룹의 리더로 만든다 —
        # _stop_slam이 SIGINT를 "그룹 전체"에 보낼 수 있어야 한다(아래 이유 참고).
        self._slam_process = subprocess.Popen(
            ['ros2', 'launch', 'careguard_bringup', 'cartographer_launch.py'], start_new_session=True)

    def _stop_slam(self, message: dict):
        # "매핑 취소"/"매핑 재시작"(재시작은 서버가 취소 API 먼저 부른 뒤 slam_start를
        # 다시 보낸다) — 지금 떠 있는 cartographer_launch.py를 실제로 끈다. 이걸 안 하면
        # _start_slam이 "이미 SLAM 작업이 진행 중입니다"로 다음 시작을 계속 거부한다.
        if self._slam_process is not None and self._slam_process.poll() is None:
            self._terminate_process_group(self._slam_process, 'SLAM')
            self._slam_process = None
            return
        self._slam_process = None
        # self._slam_process가 없다고 cartographer가 안 떠있다는 뜻은 아니다 — 이
        # 다운링크가 SLAM 도중 재시작된 적이 있으면(오늘 실 로봇 테스트로 여러 번
        # 겪었다) Popen 핸들 자체가 사라져 이 함수가 조용히 아무 것도 안 죽이고
        # "취소할 작업이 없습니다"만 남기고 끝났다 — 화면상 "매핑 취소"를 눌렀는데
        # cartographer는 실제로 계속 돌아가는 버그. _ensure_navigation_running과 같은
        # 이유로, 명령줄 패턴으로 찾아 정리하는 경로로 한 번 더 확인한다(없으면
        # _kill_external_process_group이 조용히 아무 일도 안 한다).
        self._kill_external_process_group('careguard_bringup cartographer_launch.py', 'SLAM(고아 프로세스)')

    def _terminate_process_group(self, process: subprocess.Popen, label: str):
        """process(start_new_session=True로 띄운 프로세스 그룹 리더)를 그룹
        전체에 SIGINT -> (5초 안 죽으면) SIGKILL 순서로 종료한다. _stop_slam·
        Nav2 종료(_start_slam/_ensure_navigation_running) 둘 다 이걸 쓴다.

        ros2 launch 프로세스 "하나"에만 신호를 보내면(subprocess.send_signal)
        정작 그 밑에서 실제로 도는 자식 노드들(cartographer_node, rviz2,
        amcl 등)은 안 죽고 고아 프로세스로 남는다(실 로봇에서 확인됨) —
        터미널에서 Ctrl+C를 누르면 셸이 "포그라운드 프로세스 그룹 전체"에
        신호를 보내기 때문에 잘 죽는 것이지, ros2 launch 자체가 자식들에게
        신호를 잘 전달해줘서가 아니었다. 그래서 os.killpg로 그룹 전체에 SIGINT를
        보내 Ctrl+C와 같은 효과를 낸다.
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
                # SIGKILL 뒤에도 안 죽는 경우는 정상적으론 거의 없지만(커널 D-state
                # 등), 여기서 예외가 그대로 새 나가면 _handle_command를 부른 WS
                # 수신 루프까지 깨져서(_connect_forever의 바깥 except로 흡수되며
                # 재연결) 호출부의 프로세스 참조가 정리 안 된 채 "취소가 안
                # 끝난 것처럼" 보인다(실제로 SLAM 쪽에서 겪음) — 로그만 남기고
                # 호출부가 항상 자기 상태를 정리하게 한다(finally).
                self.get_logger().error(f'{label} 프로세스가 SIGKILL 후에도 안 죽었습니다')
        except ProcessLookupError:
            pass  # 이미 죽어 있었음

    def _find_process_groups(self, pattern: str) -> set:
        """명령줄에 pattern이 들어간 프로세스들을 찾아 그 프로세스 그룹 id 집합을
        반환한다(중복 제거됨). 없으면 빈 집합.
        """
        try:
            result = subprocess.run(['pgrep', '-f', pattern], capture_output=True, text=True, timeout=5)
            pids = [int(p) for p in result.stdout.split()]
        except (subprocess.TimeoutExpired, ValueError):
            return set()
        pgids = set()
        for pid in pids:
            try:
                pgids.add(os.getpgid(pid))
            except ProcessLookupError:
                pass
        return pgids

    def _kill_external_process_group(self, pattern: str, label: str, wait_timeout_sec: float = 8.0):
        """이 노드가 PID를 들고 있지 않은(사용자가 다른 터미널에서 직접 켰거나,
        이 노드 자신이 재시작되기 전 생애에 띄워서 지금은 기억을 못 하는) 프로세스를
        명령줄 패턴(pgrep -f)으로 찾아 그 프로세스 그룹 전체에 SIGINT를 보낸다 —
        _terminate_process_group과 같은 이유(그룹 리더 하나만 죽이면 자식들이 고아로
        남는다. 예: `ros2 run` 래퍼만 죽이면 그 밑의 실제 노드 프로세스가 안 죽는다).
        base bringup(teleop_launch.py)·uplink_node·Nav2/SLAM launch처럼 "다른
        터미널에서 이미 켜져 있다고 가정하고 이 노드가 직접 시작하지 않는(또는
        직접 시작했지만 그 사실을 더는 기억 못 하는)" 프로세스를 정리할 때 쓴다.

        _terminate_process_group과 달리 이 경로로 정리하는 프로세스는 Popen 핸들이
        없어 process.wait()을 못 쓴다 — 대신 pgrep으로 폴링해서 실제로 다 죽을
        때까지 기다린다. 안 기다리고 바로 돌아가면(예전 구현), 호출부가 "정리
        요청은 보냈으니 새로 띄워도 되겠지"라고 바로 새 프로세스를 띄우다가 아직
        안 죽은 이전 인스턴스와 잠깐 두 벌이 겹쳐 뜨는 경우가 실제로 있었다
        (2026-09-08 — Nav2가 두 벌 떠서 amcl·costmap이 충돌, 라이다가 지도와 안
        맞아 보이는 버그로 이어짐). 이미 꺼져 있으면 조용히 넘어간다.
        """
        pgids = self._find_process_groups(pattern)
        if not pgids:
            self.get_logger().info(f'{label}: 실행 중이 아닙니다')
            return

        for pgid in pgids:
            try:
                os.killpg(pgid, signal.SIGINT)
            except ProcessLookupError:
                pass
        self.get_logger().info(f'{label} 종료 신호를 보냈습니다 — 완전히 끝날 때까지 기다립니다')

        deadline = time.monotonic() + wait_timeout_sec
        while time.monotonic() < deadline:
            if not self._find_process_groups(pattern):
                return
            time.sleep(0.3)

        remaining = self._find_process_groups(pattern)
        if remaining:
            self.get_logger().warn(f'{label}이(가) {wait_timeout_sec:.0f}초 내에 안 죽어 강제 종료합니다')
            for pgid in remaining:
                try:
                    os.killpg(pgid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            time.sleep(1.0)

    def _shutdown_everything(self, message: dict):
        """대시보드 "로봇 종료" 버튼 — 이 로봇에서 켜져 있는 소프트웨어를 전부
        내린다: Nav2/SLAM(+그 안의 rviz2), 베이스 구동(teleop_launch.py), 그리고
        이 명령을 처리하는 다운링크 자신과 uplink_node까지. 지금까지는 이걸 전부
        사용자가 손으로 하나씩 켜고 꺼야 했다.

        Nav2/SLAM은 이미 이 노드가 PID를 들고 있어 기존 _terminate_process_group
        으로 정리한다(_stop_slam과 동일한 경로). 베이스 구동·uplink_node는 이
        노드가 시작하지 않아 PID를 모르므로 _kill_external_process_group으로
        명령줄 패턴을 찾아 정리한다.

        마지막에 다운링크 프로세스 자신을 os._exit()으로 즉시 끝낸다 — 이 메서드가
        rclpy.spin()이 도는 메인 스레드가 아니라 WS 수신 전용 스레드(asyncio
        이벤트루프)에서 돌기 때문에 정상적인 rclpy 종료 경로(main()의 finally)를
        타지 않고, 애초에 "전부 끄기"가 목적이라 이 프로세스 자체의 정상 종료
        절차를 굳이 지킬 이유가 없다. WS 연결이 끊기는 것 자체가 서버/대시보드에
        "이 로봇이 완전히 꺼졌다"는 신호가 된다.
        """
        if self._nav_process is not None and self._nav_process.poll() is None:
            self._terminate_process_group(self._nav_process, 'Nav2')
            self._nav_process = None
            self._nav_map_id = 0
        if self._slam_process is not None and self._slam_process.poll() is None:
            self._terminate_process_group(self._slam_process, 'SLAM')
            self._slam_process = None
        if self._patrol_task_process is not None and self._patrol_task_process.poll() is None:
            self._terminate_process_group(self._patrol_task_process, 'patrol_task')
            self._patrol_task_process = None
            self._patrol_task_map_id = 0

        self._kill_external_process_group('careguard_bringup teleop_launch.py', '베이스 구동')
        self._kill_external_process_group('careguard_agent uplink_node', '업링크')

        self.get_logger().info('종료 명령 처리 완료 — 다운링크를 종료합니다')
        os._exit(0)

    def _ensure_navigation_running(self, map_id: int) -> bool:
        """순찰(patrol_start)/이동 명령 전에 Nav2가 이 map_id로 이미 떠있는지
        확인하고, 아니면 새로 띄운다 — 맵 다운로드 -> Nav2 런치 -> amcl이 뜬 것
        확인 -> 초기 위치 설정까지 한 번에 처리한다. 예전엔 이 네 단계를 전부
        사람이 SSH로 직접 순서대로 실행해야 했다(안 하면 대시보드에 로봇 위치가
        하나도 안 뜨는 증상으로만 보였다). 실패하면 False — 호출부는 순찰을
        시작하지 않는다.

        빌드/실행 미검증 — 실 로봇에서 Nav2 기동 시간·amcl 준비 판정 기준을
        다시 맞춰봐야 할 수 있다.

        이 함수는 _handle_command와 같은 asyncio 이벤트루프 스레드에서 동기적으로
        돈다(time.sleep 포함, 최대 NAV2_READY_TIMEOUT_SEC초) — 그동안 이 다운링크
        연결로 들어오는 다른 명령(cmd_vel, slam_stop 등)은 처리가 지연된다.
        _save_map의 subprocess.run(timeout=20)도 같은 방식이라 이 코드베이스의
        기존 패턴과 일관되긴 하지만, 실 사용에서 불편하면 스레드/executor로 옮기는
        걸 고려할 것.
        """
        if self._slam_process is not None and self._slam_process.poll() is None:
            # 매핑 중엔 순찰을 자동으로 끼어들게 하지 않는다 — 지금 그리고 있는
            # 지도를 멋대로 버리면 안 되니, 여기서 자동으로 SLAM을 끄는 대신
            # 사용자가 명시적으로 매핑을 취소하게 한다(반대 방향은 _start_slam이
            # 자동으로 처리한다 — 순찰 중 매핑을 시작하는 쪽은 안전).
            self.get_logger().error('SLAM 매핑 중에는 순찰을 시작할 수 없습니다 — 먼저 매핑을 취소하세요')
            return False

        try:
            # map_id가 같아도 서버 쪽 내용이 바뀌었을 수 있어(재매핑 후 같은 이름으로
            # 저장 — _download_map 문서 참고) 재사용 여부를 판단하기 전에 항상 먼저
            # 최신 상태를 받아온다.
            yaml_path, changed = self._download_map(map_id)
        except Exception as exc:  # noqa: BLE001 - 다운로드 실패 원인이 다양해 전부 여기서 로그만 남긴다
            self.get_logger().error(f'맵 다운로드 실패: {exc}')
            return False

        if self._nav_process is not None and self._nav_process.poll() is None:
            if self._nav_map_id == map_id and not changed:
                return True  # 이미 이 맵으로, 내용도 그대로 떠있다 — 재사용
            reason = (f'map_id={map_id}는 그대로지만 내용이 바뀐 지도' if self._nav_map_id == map_id
                      else f'다른 맵(map_id={map_id})')
            self.get_logger().info(f'{reason}로 전환 — 기존 Nav2를 종료하고 다시 띄웁니다')
            self._terminate_process_group(self._nav_process, 'Nav2')
            self._nav_process = None
            self._nav_map_id = 0

        self.get_logger().info(f'Nav2 시작: map_id={map_id} ({yaml_path})')
        # self._nav_process가 None인 건 "Nav2가 안 떠있다"가 아니라 "이 다운링크가
        # 그걸 기억 못 한다"는 뜻일 수도 있다 — 다운링크 자신이 재시작되면(업링크/
        # 다운링크만 다시 켠 경우 등) _nav_process는 항상 새로 None으로 시작하지만,
        # 이전 생애에 띄운 Nav2는 고아로 계속 떠 있을 수 있다. 확인 없이 새로
        # 띄우면 두 벌의 amcl·costmap이 같은 /scan·/amcl_pose·TF를 놓고 서로
        # 충돌해서 위치 추정이 엉망이 된다 — 실제로 겪은 버그(2026-09-08): 라이다와
        # 지도가 안 맞아 보여서 봤더니 navigation_diff_launch.py가 두 개 떠 있었음.
        # 새로 띄우기 전에 항상 먼저 같은 패턴의 고아 프로세스를 정리한다(없으면
        # _kill_external_process_group이 조용히 아무 일도 안 한다).
        self._kill_external_process_group('careguard_bringup navigation_diff_launch.py', '(정리) 이전 Nav2')
        # start_new_session=True — SLAM과 같은 이유(_terminate_process_group가
        # 그룹 전체를 죽일 수 있어야 한다).
        self._nav_process = subprocess.Popen(
            ['ros2', 'launch', 'careguard_bringup', 'navigation_diff_launch.py', f'map:={yaml_path}'],
            start_new_session=True)
        self._nav_map_id = map_id

        # amcl이 /amcl_pose를 advertise할 때까지 기다린다 — 메시지가 오기
        # 시작했는지가 아니라 "그 퍼블리셔가 생겼는지"만 본다. 실제 위치 추정
        # (메시지 발행)은 초기 위치를 넣어줘야 시작되므로 이 시점엔 아직 안 온다.
        deadline = time.monotonic() + NAV2_READY_TIMEOUT_SEC
        ready = False
        while time.monotonic() < deadline:
            if self.count_publishers('/amcl_pose') > 0:
                ready = True
                break
            time.sleep(0.5)
        if not ready:
            self.get_logger().error(f'{NAV2_READY_TIMEOUT_SEC:.0f}초 안에 Nav2(amcl)가 안 떴습니다')
            return False

        # 초기 위치 설정 — careguard_nav 패키지의 기존 노드를 그대로 재사용한다
        # (careguard_agent는 서버/WS만 다루고 실제 Nav2 조작은 careguard_nav에
        # 맡긴다는 원칙 — patrol_task.py가 이미 이 원칙을 따르고 있다. 여기서
        # 직접 /initialpose를 publish하는 대신 별도 프로세스로 그 노드를 부른다).
        try:
            result = subprocess.run(
                ['ros2', 'run', 'careguard_nav', 'initial_pose_setter'],
                env={**os.environ, 'CAREGUARD_MAP_ID': str(map_id)},
                # initial_pose_setter.py::publish_when_ready 참고 — DDS 디스커버리
                # 직후 첫 메시지 유실 문제로 재시도 구간을 늘려서(3번/0.6초 ->
                # 15번/약 3초) 여기 timeout도 같이 늘렸다(SUBSCRIBER_WAIT_TIMEOUT_SEC
                # 5초 + 발행 구간 약 3초 + 여유). 실 로봇에서 이 값이 짧아 초기 위치
                # 설정이 조용히 실패 -> global_costmap이 activating에 멈추고
                # bt_navigator가 inactive라 순찰이 전부 무동작으로 끝나는 버그를
                # 실제로 겪었다(2026-09-08).
                capture_output=True, text=True, timeout=20,
            )
            if result.returncode != 0:
                self.get_logger().warn(f'초기 위치 설정 실패(rc={result.returncode}): {result.stderr}')
        except subprocess.TimeoutExpired:
            self.get_logger().warn('초기 위치 설정이 20초를 넘어 중단했습니다')

        return True

    def _ensure_patrol_task_running(self, map_id: int) -> bool:
        """patrol_task(careguard_mission)가 이 map_id로 이미 떠있는지 확인하고,
        아니면 (재)시작한다 — CAREGUARD_MAP_ID를 정확히 넣어서.

        patrol_task는 시작 시점에 딱 한 번 WaypointManager.fetch(CAREGUARD_MAP_ID)로
        충전 스테이션 좌표를 캐시해두고, 순찰이 끝날 때 그 캐시로 복귀 지점을 찾는다
        (careguard_nav/dock_return.py). 이 env var를 사람이 SSH로 직접 맞춰서 켜야
        했는데, 지도를 바꾼 뒤 깜빡하거나 아예 안 넣고 켜면(2026-09-08 실 로봇에서
        실제로 겪음) 캐시가 비어(또는 예전 지도 것으로 남아) "순찰 후 충전소 복귀"가
        조용히 빈 동작으로 끝나거나 엉뚱한 좌표로 갔다 — _ensure_navigation_running과
        같은 이유·같은 패턴으로 이 노드가 대신 관리해 사람이 신경 안 써도 되게 한다.

        map_id가 같아도 매번 무조건 재시작한다 — _ensure_navigation_running과 달리
        여기선 map_id 하나만 보고 "이미 이 지도로 떠있다"고 재사용하면 안 된다.
        map(robot_id, name) UNIQUE 제약 때문에 같은 이름으로 재매핑해도 map_id가
        안 바뀌는데, 그러면 충전 스테이션 좌표는 새로 매핑한 값으로 바뀌었어도
        patrol_task는 최초 기동 때 캐시한 예전 좌표를 계속 쓴다 — "지도를 바꿔도
        이전 지도 기준으로 동작한다"는 리포트(2026-09-09)와 같은 원인의 버그다.
        patrol_task는 Nav2와 달리 재시작 비용이 아주 작은 `ros2 run` 노드 하나라
        (런치 트리·amcl·costmap 없음), Nav2처럼 "내용이 바뀌었는지" 따로 비교하는
        대신 이 함수가 불릴 때마다 항상 새로 띄우는 쪽이 더 단순하고 안전하다 —
        이 함수는 순찰이 실제로 진행 중일 땐 호출되지 않는다(nav_prepare로 미리
        띄우거나, 새 순찰을 시작하기 직전에만 부른다 — _start_patrol 참고).
        """
        if self._patrol_task_process is not None and self._patrol_task_process.poll() is None:
            self._terminate_process_group(self._patrol_task_process, 'patrol_task')
            self._patrol_task_process = None
            self._patrol_task_map_id = 0

        # _nav_process와 같은 이유 — 이 다운링크가 재시작된 적이 있으면 이전 생애에
        # 띄운 patrol_task가 고아로 남아 잘못된(또는 비어있는) 캐시를 계속 들고 있을
        # 수 있다. 새로 띄우기 전에 항상 먼저 정리한다.
        self._kill_external_process_group('careguard_mission patrol_task', '(정리) 이전 patrol_task')

        self._patrol_task_process = subprocess.Popen(
            ['ros2', 'run', 'careguard_mission', 'patrol_task'],
            env={**os.environ, 'CAREGUARD_MAP_ID': str(map_id)}, start_new_session=True)
        self._patrol_task_map_id = map_id

        if not self._patrol_client.wait_for_server(timeout_sec=PATROL_TASK_READY_TIMEOUT_SEC):
            self.get_logger().error(f'{PATROL_TASK_READY_TIMEOUT_SEC:.0f}초 안에 patrol_task 액션 서버가 안 떴습니다')
            return False
        return True

    def _download_map(self, map_id: int) -> Tuple[str, bool]:
        """서버 GET /maps/active에서 이 로봇의 활성 맵 메타데이터+이미지를 받아
        로봇 로컬에 저장하고, (만들어진 .yaml 경로, 내용이 실제로 바뀌었는지)를
        반환한다. write_map_yaml_sidecar(server/app/api/v1/media.py)가 서버
        쪽에 만드는 것과 같은 포맷을 로봇 쪽에서 다시 만드는 셈이다.

        두 번째 반환값(changed)이 필요한 이유: map(robot_id, name)엔 UNIQUE
        제약이 있어서, 같은 이름으로 재매핑하면 서버는 새 map 행을 만들지 않고
        기존 행을 그대로 UPDATE한다(map.py::create_map의 ON CONFLICT — map.id가
        안 바뀌어야 그 맵에 이미 등록된 웨이포인트가 계속 유효하다). 그래서
        map_id만 보고 "이미 이 지도로 떠있으니 그대로 둔다"고 판단하면, 같은
        이름으로 재매핑해도 로봇은 예전 이미지·resolution·origin으로 계속 Nav2를
        띄운 채로 남는다 — "순찰 페이지에서 지도를 바꿔도 이전 지도의 nav2가
        여전히 켜져있다" 리포트(2026-09-09)의 원인이었다. _ensure_navigation_running이
        이 값으로 map_id가 같아도 재기동이 필요한지 판단한다.

        mode/negate/occupied_thresh/free_thresh는 예전엔 GET /maps/active가 안
        내려줘서 map_saver_cli 기본값(trinary/0/0.65/0.196)을 무조건 하드코딩했다
        — 실제 DB의 raw_yaml엔 free_thresh=0.25로 저장된 맵도 있어서, 그 맵으로
        순찰하는 로봇은 원본과 다른 값으로 점유/자유 공간을 해석하고 있었다
        (스케일 문제와는 별개의 버그, 2026-09-09 확인). 이제 서버가 map.raw_yaml에서
        뽑아 내려주는 값을 그대로 쓴다 — 옛날에 등록돼 raw_yaml에 이 키가 없는
        맵은 서버가 이미 map_saver_cli 기본값으로 채워 보내므로 여기서 또 폴백할
        필요는 없다.

        robot_code로만 찾지 않고 응답의 id가 요청받은 map_id와 실제로 같은지도
        확인한다 — robot_code당 활성 맵은 보통 하나뿐이라 대부분은 어차피 같지만,
        방금 재매핑해서 서버의 활성 맵이 막 바뀐 직후처럼 대시보드가 들고 있던
        map_id와 서버의 "지금 활성 맵"이 어긋나는 순간엔 이 확인이 없으면 조용히
        엉뚱한 맵으로(그 맵의 웨이포인트로 초기 위치까지) 순찰을 시작하게 된다.
        """
        resp = requests.get(f'{ws_config.SERVER_HTTP_URL}/maps/active', timeout=10)
        resp.raise_for_status()
        entry = next((m for m in resp.json() if m.get('robot_code') == ws_config.ROBOT_CODE), None)
        if entry is None or not entry.get('image_url'):
            raise RuntimeError(f'서버에 robot_code={ws_config.ROBOT_CODE}의 활성 맵 이미지가 없습니다')
        if entry.get('id') != map_id:
            raise RuntimeError(
                f'요청받은 map_id={map_id}가 서버의 현재 활성 맵(id={entry.get("id")})과 다릅니다 — '
                '대시보드가 오래된 맵 정보를 들고 있을 수 있습니다. 새로고침 후 다시 시도하세요.')

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
        # SIGINT 시 rclpy가 내부적으로 먼저 shutdown()을 부를 수 있어, 여기서
        # 무조건 또 부르면 "rcl_shutdown already called"로 비정상 종료한다
        # (slam_progress_publisher.py에서 실제로 겪은 버그, 같은 패턴이라 방어).
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
