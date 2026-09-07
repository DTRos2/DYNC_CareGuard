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


class DownlinkNode(Node):
    def __init__(self):
        super().__init__('downlink_node')
        self._patrol_client = ActionClient(self, Patrol, 'patrol')
        self._slam_process = None
        self._nav_process = None
        self._nav_map_id = 0
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
        # start_new_session=True로 이 프로세스를 새 프로세스 그룹의 리더로 만든다 —
        # _stop_slam이 SIGINT를 "그룹 전체"에 보낼 수 있어야 한다(아래 이유 참고).
        self._slam_process = subprocess.Popen(
            ['ros2', 'launch', 'careguard_bringup', 'cartographer_launch.py'], start_new_session=True)

    def _stop_slam(self, message: dict):
        # "매핑 취소"/"매핑 재시작"(재시작은 서버가 취소 API 먼저 부른 뒤 slam_start를
        # 다시 보낸다) — 지금 떠 있는 cartographer_launch.py를 실제로 끈다. 이걸 안 하면
        # _start_slam이 "이미 SLAM 작업이 진행 중입니다"로 다음 시작을 계속 거부한다.
        if self._slam_process is None or self._slam_process.poll() is not None:
            self.get_logger().warn('취소할 SLAM 작업이 없습니다')
            self._slam_process = None
            return
        self._terminate_process_group(self._slam_process, 'SLAM')
        self._slam_process = None

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

        if self._nav_process is not None and self._nav_process.poll() is None:
            if self._nav_map_id == map_id:
                return True  # 이미 이 맵으로 떠있다 — 재사용
            self.get_logger().info(f'다른 맵으로 떠있던 Nav2를 종료하고 map_id={map_id}로 다시 띄웁니다')
            self._terminate_process_group(self._nav_process, 'Nav2')
            self._nav_process = None
            self._nav_map_id = 0

        try:
            yaml_path = self._download_map(map_id)
        except Exception as exc:  # noqa: BLE001 - 다운로드 실패 원인이 다양해 전부 여기서 로그만 남긴다
            self.get_logger().error(f'맵 다운로드 실패: {exc}')
            return False

        self.get_logger().info(f'Nav2 시작: map_id={map_id} ({yaml_path})')
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
                capture_output=True, text=True, timeout=10,
            )
            if result.returncode != 0:
                self.get_logger().warn(f'초기 위치 설정 실패(rc={result.returncode}): {result.stderr}')
        except subprocess.TimeoutExpired:
            self.get_logger().warn('초기 위치 설정이 10초를 넘어 중단했습니다')

        return True

    def _download_map(self, map_id: int) -> str:
        """서버 GET /maps/active에서 이 로봇의 활성 맵 메타데이터+이미지를 받아
        로봇 로컬에 저장하고, 만들어진 .yaml 경로를 반환한다. write_map_yaml_sidecar
        (server/app/api/v1/media.py)가 서버 쪽에 만드는 것과 같은 포맷을 로봇
        쪽에서 다시 만드는 셈이다 — 서버가 raw_yaml을 GET /maps/active로는 안
        내려주므로(이미지 서빙용 최소 필드만 응답), negate/occupied_thresh/
        free_thresh는 map_saver_cli 기본값을 그대로 쓴다.

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
        with open(pgm_path, 'wb') as f:
            f.write(image_resp.content)

        yaml_content = {
            'image': os.path.basename(pgm_path),
            'resolution': entry.get('resolution') or 0.05,
            'origin': [entry.get('origin_x', 0.0), entry.get('origin_y', 0.0), entry.get('origin_yaw', 0.0)],
            'negate': 0,
            'occupied_thresh': 0.65,
            'free_thresh': 0.196,
        }
        with open(yaml_path, 'w') as f:
            yaml.safe_dump(yaml_content, f, default_flow_style=None, sort_keys=False)
        return yaml_path

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
