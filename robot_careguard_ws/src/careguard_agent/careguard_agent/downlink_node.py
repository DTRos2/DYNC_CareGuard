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
from std_msgs.msg import Bool, String

from careguard_msgs.action import DeliverMedication, Patrol
from careguard_msgs.msg import Waypoint as WaypointMsg
from careguard_msgs.srv import PillboxSlot

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

# video_recorder가 떠서 patrol_recording 토픽 구독을 advertise할 때까지 기다리는
# 최대 시간. patrol_task와 같은 이유로 여유를 두되, ROS2 노드 하나 뜨는 정도라
# 짧게 잡는다.
VIDEO_RECORDER_READY_TIMEOUT_SEC = 10.0

# careguard_perception/config.py의 PATROL_RECORDING_TOPIC과 같은 문자열 —
# patrol_task.py가 이미 같은 이유로 중복 정의하고 있다(패키지 간 공유 없이
# 토픽명을 각자 하드코딩하는 이 코드베이스의 기존 방식). video_recorder가
# 이 토픽을 실제로 구독하기 시작했는지 확인하는 용도로만 쓴다 — 이 노드가
# 직접 이 토픽에 publish하지는 않는다(그건 patrol_task.py의 역할).
PATROL_RECORDING_TOPIC = '/careguard/perception/patrol_recording'

# audio_player(HMI 스피커 담당)가 떠서 안내 토픽 구독을 advertise할 때까지
# 기다리는 최대 시간.
VOICE_ANNOUNCER_READY_TIMEOUT_SEC = 10.0

# patrol_intruder_watch_node(careguard_perception)가 떠서 patrol_recording 토픽
# 구독을 advertise할 때까지 기다리는 최대 시간. video_recorder/audio_player보다
# 훨씬 길게 잡는다 — YOLO 얼굴검출 x2(완화③ 사람 전신 게이트 포함) + FaceNet +
# adapter를 순서대로 로딩하는 무거운 노드라 몇 초 안에는 안 뜬다.
PATROL_INTRUDER_WATCH_READY_TIMEOUT_SEC = 30.0

# careguard_hmi/config.py의 VOICE_TOPIC과 같은 문자열 — PATROL_RECORDING_TOPIC과
# 같은 이유·같은 방식으로 audio_player가 이 토픽을 구독하기 시작했는지
# 확인하는 용도로만 쓴다(이 노드가 직접 publish하지는 않음, patrol_task.py가 한다).
VOICE_TOPIC = '/careguard/voice/announce'

# careguard_perception/config.py의 TRIGGER_TOPIC과 같은 문자열 — PATROL_RECORDING_TOPIC과
# 같은 이유·같은 방식으로 중복 정의한다. PATROL_RECORDING_TOPIC과 달리 이건 이
# 노드가 직접 publish한다 — SOS 호출 버튼처럼 care_event_id/severity가 실려 온
# patrol_start(_start_patrol 참고)를 받으면, video_recorder.py가 이벤트 클립
# 녹화를 켜도록 이 토픽으로 신호를 보낸다.
RECORD_TRIGGER_TOPIC = '/careguard/perception/record_trigger'

# careguard_hw/pillbox_controller.py가 advertise하는 서비스 — medication_task.py의
# 같은 이름 상수와 동일한 문자열이어야 한다(패키지 간 공유 없이 각자 하드코딩하는
# 기존 방식). 이 노드가 직접 호출하지는 않는다 — _ensure_pillbox_controller_running이
# "서비스가 떴는지"만 확인하는 용도로 클라이언트를 하나 들고 있는다.
PILLBOX_SLOT_SERVICE = '/careguard/hw/pillbox_slot'

# pillbox_controller가 떠서 PILLBOX_SLOT_SERVICE를 advertise할 때까지 기다리는
# 최대 시간 — 시리얼 포트 열기 정도라 Nav2보다는 훨씬 빠르지만 여유를 둔다.
PILLBOX_CONTROLLER_READY_TIMEOUT_SEC = 10.0

# medication_task가 떠서 /deliver_medication 액션 서버를 advertise할 때까지
# 기다리는 최대 시간. patrol_task와 같은 이유로 여유를 둔다.
MEDICATION_TASK_READY_TIMEOUT_SEC = 15.0

# careguard_hmi/config.py의 CALL_STATE_TOPIC과 같은 문자열(같은 관례) — 서버가
# call_start/call_end를 보내면 이 노드가 대신 True/False로 바꿔 발행하고,
# audio_capture_node.py/audio_downlink_player.py(캡처·재생 시작/종료)와
# audio_player.py(통화 중 TTS 안내 스킵)가 구독한다.
CALL_STATE_TOPIC = '/careguard/call/state'


class DownlinkNode(Node):
    def __init__(self):
        super().__init__('downlink_node')
        self._patrol_client = ActionClient(self, Patrol, 'patrol')
        # P3 "복약 재안내" 버튼(서버 "medication_start" WS, 순찰과 무관한 즉시 호출)
        # 전용 — patrol_task.py를 거치는 자동 트리거는 medication_client.py(패키지
        # 내부)가 따로 쓰고, 여기서 만드는 클라이언트는 비동기(fire-and-forget)만
        # 한다는 점이 다르다(_patrol_client와 같은 이유 — 이 노드의 spin은
        # _run_ws_loop 스레드가 아니라 main()의 rclpy.spin이 돌리므로 블로킹 호출을
        # 넣으면 그동안 다른 모든 명령 처리가 멈춘다).
        self._medication_client = ActionClient(self, DeliverMedication, 'deliver_medication')
        # _ensure_pillbox_controller_running이 "서비스가 떴는지"만 확인하는 용도 —
        # 이 노드가 직접 PillboxSlot을 호출하지는 않는다(그건 medication_task.py의 역할).
        self._pillbox_client = self.create_client(PillboxSlot, PILLBOX_SLOT_SERVICE)
        # 지금 진행 중인 Patrol goal의 핸들 — patrol_stop이 취소할 대상을 찾는 데 쓴다.
        # send_goal_async()의 반환값(Future)을 예전엔 아무 데도 안 담아둬서, "취소"라는
        # 개념 자체가 구조적으로 불가능했다(patrol_task.py의 cancel_callback 부재와
        # 같은 근본 원인의 반쪽 — 서버가 취소를 받아줘도 클라이언트가 어느 goal을
        # 취소할지 모르면 의미가 없다). goal이 끝나면(성공/실패/취소 불문) None으로
        # 되돌린다(_on_patrol_result).
        self._patrol_goal_handle = None
        # _on_patrol_goal_response가 만든 result Future 그 자체 — get_result_async()는
        # 호출할 때마다 새 Future를 만들어 GetResult 요청을 새로 보내므로,
        # _on_patrol_result에서 "이 콜백이 지금 추적 중인 goal의 것인지" 확인하려면
        # get_result_async()를 다시 부르지 말고 이걸 저장해뒀다가 비교해야 한다.
        self._patrol_result_future = None
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
        # video_recorder(careguard_perception)는 patrol_task와 달리 map_id에
        # 묶이지 않는다(카메라 프레임을 계속 버퍼링/이벤트 클립·순찰 클립을 만들 뿐
        # 특정 맵 지식이 필요 없다) — 한 번 뜨면 계속 재사용하고, patrol_task처럼
        # map이 바뀔 때마다 재시작하지 않는다.
        self._video_recorder_process = None
        # audio_player(careguard_hmi)도 video_recorder와 같은 이유로 map_id에
        # 안 묶인다 — 한 번 뜨면 계속 재사용한다.
        self._voice_announcer_process = None
        # 통화 기능(audio_capture_node.py/audio_downlink_player.py)도 같은 이유로
        # map_id에 안 묶이고 한 번 뜨면 계속 재사용한다. 실제 마이크 캡처/스피커
        # 재생은 CALL_STATE_TOPIC이 True일 때만 시작되므로, 통화 중이 아닐 땐
        # 그냥 토픽 구독만 하고 대기한다.
        self._audio_capture_process = None
        self._audio_downlink_player_process = None
        # patrol_intruder_watch_node(careguard_perception)는 video_recorder/audio_player와
        # 달리 이 노드 기동 즉시(__init__) 띄우지 않는다 — YOLO x2/FaceNet/adapter를
        # 메모리에 올리는 무거운 노드라, P6 페이지를 열어보기만 하거나(_prepare_navigation)
        # SLAM만 하는 상황에서도 매번 로딩되면 불필요한 자원 낭비다. 실제 순찰이 시작될
        # 때(_start_patrol)만 띄운다 — _ensure_patrol_intruder_watch_running 참고.
        self._patrol_intruder_watch_process = None
        # pillbox_controller(careguard_hw)/medication_task(careguard_mission)도
        # video_recorder/audio_player와 같은 이유로 map_id에 안 묶이고, P3 "복약
        # 재안내"(순찰과 무관한 즉시 호출)를 위해서도 상시 대기가 맞다 — __init__
        # 시점에 같이 띄운다.
        self._pillbox_controller_process = None
        self._medication_task_process = None
        self._ws = None
        # P5 방향키 패널 -> 실제 구동. limo_base(/limo/limo_base)가 이미 /cmd_vel을
        # 구독하고 있다(실 로봇에서 topic info로 확인됨) — 네임스페이스 없이 그대로 쓴다.
        self._cmd_vel_pub = self.create_publisher(Twist, 'cmd_vel', 10)
        # SOS 호출 버튼 등 care_event_id가 실린 patrol_start를 받았을 때
        # video_recorder.py에 이벤트 클립 녹화를 켜라고 알리는 용도(_start_patrol 참고).
        self._record_trigger_pub = self.create_publisher(String, RECORD_TRIGGER_TOPIC, 10)
        # 서버가 "medication_alert" 명령(_handle_medication_alert 참고)으로 내려보내는
        # TTS 문구를 audio_player가 재생하도록 쏘는 용도 — patrol_task.py도 같은
        # 토픽에 발행하지만(그쪽은 순찰 중 안내), 이 명령은 순찰 여부와 무관하게 P3
        # 수동 재안내처럼 언제든 올 수 있어 이 노드가 직접 들고 있어야 한다.
        self._voice_pub = self.create_publisher(String, VOICE_TOPIC, 10)
        self._call_state_pub = self.create_publisher(Bool, CALL_STATE_TOPIC, 10)

        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run_ws_loop, daemon=True)
        self._thread.start()

        # video_recorder/audio_player는 map_id에 안 묶이는, "한 번 뜨면 계속 재사용"되는
        # 프로세스다(_ensure_video_recorder_running/_ensure_voice_announcer_running 주석
        # 참고) — 그래서 순찰 시작(nav_prepare/patrol_start)까지 기다릴 이유가 없다. 순찰
        # 전 대기 상태에서 긴급/주의 이벤트가 터져도 바로 녹화·음성 안내가 나가야 하므로
        # 이 노드가 뜨는 즉시 같이 띄워둔다. 이미 떠있으면 그대로 재사용하는 함수라 이후
        # nav_prepare/start_patrol에서 다시 불러도 중복 기동은 안 된다. 최대 각각
        # VIDEO_RECORDER_READY_TIMEOUT_SEC/VOICE_ANNOUNCER_READY_TIMEOUT_SEC초 블로킹되지만
        # 서버 웹소켓 연결(위 _thread)은 별도 스레드라 이 대기와 무관하게 계속 진행된다.
        self._ensure_video_recorder_running()
        self._ensure_voice_announcer_running()
        # pillbox_controller를 medication_task보다 먼저 띄운다 — medication_task._open_slot()
        # 자체에 재시도(HW_SERVICE_TIMEOUT_SEC)가 있어 순서가 엄격하진 않지만, 하드웨어
        # 브리지가 먼저 뜨는 게 자연스러운 의존 순서다.
        self._ensure_pillbox_controller_running()
        self._ensure_medication_task_running()
        self._ensure_audio_capture_running()
        self._ensure_audio_downlink_player_running()

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

    def _send_nav_status(self, running: bool):
        """P6 상단 상태 바가 "SLAM 중도 아니고 Nav2도 안 뜬 상태"를 구분해서 안내할 수
        있게 알린다 — 예전엔 _ensure_navigation_running이 성공/실패해도 그 결과를
        서버/PC 어디에도 알리지 않아서(로그만 남김), 대시보드는 이 로봇의 Nav2가 실제로
        떠 있는지 전혀 모른 채 순찰/이동 버튼을 계속 활성 상태로 뒀다. _ensure_navigation_
        running의 모든 반환 지점과, SLAM 시작이 Nav2를 강제로 내리는 지점(_start_slam)에서
        부른다.
        """
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
        elif msg_type == 'medication_start':
            self._start_medication(message)
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
        elif msg_type == 'medication_alert':
            self._handle_medication_alert(message)
        elif msg_type == 'call_start':
            self._call_state_pub.publish(Bool(data=True))
        elif msg_type == 'call_end':
            self._call_state_pub.publish(Bool(data=False))

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
        # video_recorder를 patrol_task보다 먼저 띄워야 한다 — patrol_task가 뜨자마자
        # patrol_start 신호를 patrol_recording 토픽으로 쏘는데, video_recorder가 아직
        # 구독을 시작하기 전이면(늦게 join한 구독자는 그 전에 발행된 메시지를 못 받는다)
        # 그 신호를 조용히 놓친다 — 순서를 꼭 지킨다.
        self._ensure_video_recorder_running()
        # video_recorder와 같은 이유(늦게 join한 구독자는 이전 메시지를 못 받음) —
        # audio_player도 patrol_task보다 먼저 떠 있어야 순찰 시작 안내를 놓치지 않는다.
        self._ensure_voice_announcer_running()
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
                # initial_pose_setter의 재시도 발행 구간(3번/0.6초 -> 15번/약 3초 ->
                # 5번/약 1초, 2026-09-14 — 파일 내 주석 참고: DDS 디스커버리 직후
                # 첫 메시지가 씹히는 문제 대 P6 대시보드 마커 흔들림, 둘 사이 절충)
                # 보다 여유 있게 잡은 타임아웃 — SUBSCRIBER_WAIT_TIMEOUT_SEC 5초 +
                # 발행 구간 약 1초 + 넉넉한 margin.
                capture_output=True, text=True, timeout=20,
            )
            if result.returncode != 0:
                self.get_logger().warn(f'초기 위치 수동 보정 실패(rc={result.returncode}): {result.stderr}')
        except subprocess.TimeoutExpired:
            self.get_logger().warn('초기 위치 수동 보정이 20초를 넘어 중단했습니다')

    def _start_patrol(self, message: dict):
        # override 없이 들어온 요청이 지금 실행 중인 goal을 밀어내면 안 된다 —
        # 서버가 이미 patrol.py::_reject_if_already_patrolling으로 막지만, 혹시
        # 그 검사와 이 메시지가 도착하는 사이에 경합이 생겨도 로봇 쪽에서 한 번
        # 더 막는다. 이게 없으면 _ensure_patrol_task_running이 "map_id가 같아도
        # 매번 무조건 재시작"하는 원래 설계 때문에 지금 돌고 있는 goal을 조용히
        # SIGINT로 죽이고 새로 띄웠다 — nav_prepare(P6 페이지 새로고침만 해도
        # 발사됨)가 그 계기가 될 수 있다는 걸 2026-09-15 R-07 스케줄 순찰이 원인
        # 불명으로 stuck된 사고에서 실제로 확인했다. override=True는 ESP32 SOS와
        # 저배터리 자동 복귀(robot_ws.py) 둘뿐이다 — 이 둘은 긴급이라 지금 뭘
        # 하고 있든 밀어내고 우선 실행돼야 한다.
        override = bool(message.get('override'))
        if self._patrol_goal_handle is not None and not override:
            self.get_logger().warn('이미 순찰/이동이 진행 중이라 새 요청을 무시합니다(override 없음)')
            return

        # amcl(Nav2)이 안 떠있으면 순찰 자체가 불가능하다 — 예전엔 사용자가 매번
        # navigation_diff_launch.py + initial_pose_setter를 직접 손으로 켜야 했다
        # (그걸 깜빡하면 로봇 위치가 대시보드에 하나도 안 뜨는 증상으로만 보였다).
        # map_id가 오면(patrol.py::start_patrol_run/drive_to_point 둘 다 보낸다)
        # 여기서 필요시 자동으로 다 켠다.
        map_id = int(message.get('map_id', 0) or 0)
        if map_id > 0 and not self._ensure_navigation_running(map_id):
            self.get_logger().error('Nav2 준비 실패로 순찰을 시작하지 않습니다')
            return

        # patrol_task가 뜨자마자(또는 이미 떠있는 채로 이 goal을 받자마자) patrol_start
        # 신호를 patrol_recording 토픽으로 쏘므로, 그 전에 video_recorder가 구독을
        # 시작해둬야 한다(늦게 join한 구독자는 이미 발행된 메시지를 못 받는다) — 아래
        # _ensure_patrol_task_running보다 반드시 먼저 부른다. 녹화 인프라 준비 실패가
        # 순찰 자체를 막을 이유는 없으므로(카메라 미연결 등으로도 실패할 수 있다)
        # 반환값은 로그만 남기고 무시한다 — 순찰은 그대로 진행된다.
        self._ensure_video_recorder_running()
        # 같은 이유로 audio_player도 patrol_task보다 먼저 떠 있어야 한다.
        self._ensure_voice_announcer_running()
        # patrol_intruder_watch_node도 같은 이유로 patrol_task보다 먼저 떠서
        # patrol_recording 구독을 시작해둬야 순찰 시작 신호를 놓치지 않는다 — 실제
        # 순찰이 시작될 때만 띄운다(_ensure_patrol_intruder_watch_running 문서 참고).
        self._ensure_patrol_intruder_watch_running()

        # patrol_task도 Nav2처럼 map_id 기준으로 (재)기동을 보장한다 — 아래 이유 참고.
        if map_id > 0:
            if not self._ensure_patrol_task_running(map_id, override=override):
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
        # 둘 다 서버가 안 보내면(기존 호출부 전부) 0 — "복약 전달 경로 아님"의 기본값과
        # 같다(Patrol.action 주석 참고). patrol.py::start_patrol_run이 schedule_id로
        # patrol_schedule.medication_schedule_id를 조회했을 때만 채워 보낸다.
        goal.medication_schedule_id = int(message.get('medication_schedule_id', 0) or 0)
        goal.slot_no = int(message.get('slot_no', 0) or 0)
        send_future = self._patrol_client.send_goal_async(goal)
        send_future.add_done_callback(self._on_patrol_goal_response)

        # SOS 호출 버튼 자동 출동(patrol.py::dispatch_drive_to_point)만 severity를
        # 실어 보낸다 — P6 "이동"/일반 순찰은 이 필드가 없으므로 녹화가 안 켜진다.
        # video_recorder.py가 이미(위에서) 뜬 상태라 구독도 이미 잡혀 있다(__init__에서
        # 노드 기동 즉시 먼저 띄워두는 이유 참고) — 여기서 또 late-joiner를 걱정할
        # 필요는 없다.
        #
        # 이 goal 전송 다음에 발행한다 — patrol_task를 죽였다 새로 띄우는 작업
        # (_ensure_patrol_task_running)과 녹화 시작(video_recorder의 VideoWriter
        # open)이 같은 순간에 겹치면 로봇 컴퓨터에 순간적으로 부하가 몰려 bt_navigator가
        # goal 응답을 제때 못 보내는 문제를 실 로봇에서 확인했다(2026-09-14). patrol_task
        # 재시작 + goal 전송이 다 끝난 뒤에 녹화/음성을 트리거해 그 부하 구간을 피한다.
        severity = message.get('severity')
        if severity:
            trigger_msg = String()
            trigger_msg.data = json.dumps({
                'care_event_id': message.get('care_event_id'),
                'severity': severity,
                'reason': message.get('reason'),
            })
            self._record_trigger_pub.publish(trigger_msg)

    def _start_medication(self, message: dict):
        """서버 mission.py::create_mission()이 보내는 "medication_start" — P3
        "복약 재안내" 버튼(순찰과 무관, 로봇이 이미 대상자 앞에 있다고 가정한
        즉시 호출). patrol_task.py를 거치는 자동 경로(medication_client.py)와 달리
        여기는 비동기(fire-and-forget)로만 보낸다 — 이 노드의 spin은 main()의
        rclpy.spin이 돌리므로, 여기서 블로킹하면 그동안 patrol_stop 등 다른 모든
        WS 명령 처리가 멈춘다.

        slot_no는 mission.py::create_mission이 payload.slot_no를 그대로 실어 보낸
        값 — 그 값 자체는 대시보드 p3_recipient_page.cpp가 카드에 이미 표시 중이던
        "미수령" 슬롯에서 찾아온 것(서버는 재조회하지 않고 그대로 전달만 함). schedule_id는
        이 버튼 경로엔 없다(특정 회차와 안 묶인 즉시 호출이라 0으로 감 — medication_log에
        안 남는다, medication_task.py 주석 참고).
        """
        slot_no = int(message.get('slot_no', 0) or 0)
        if slot_no <= 0:
            self.get_logger().warn(
                'medication_start에 유효한 slot_no가 없어 무시합니다(서버 쪽 mission.py가 아직 안 채움)')
            return

        if not self._medication_client.wait_for_server(timeout_sec=5.0):
            self.get_logger().error('DeliverMedication 액션 서버를 찾을 수 없습니다')
            return

        goal = DeliverMedication.Goal()
        goal.slot_no = slot_no
        goal.schedule_id = int(message.get('schedule_id', 0) or 0)
        goal.person_timeout_sec = float(message.get('person_timeout_sec', 0.0) or 0.0)
        send_future = self._medication_client.send_goal_async(goal)
        send_future.add_done_callback(self._on_medication_goal_response)

    def _on_medication_goal_response(self, future):
        goal_handle = future.result()
        if not goal_handle.accepted:
            self.get_logger().warn('medication goal이 거부됐습니다(이미 다른 배출이 진행 중일 수 있음)')
            return
        # patrol_start와 달리 이 goal은 서버가 취소를 보낼 경로가 아직 없어(P3
        # 버튼에 "재안내 취소"는 없음) goal_handle을 따로 안 들고 있는다 — 결과는
        # medication_task.py가 /careguard/patrol_status로 이미 relay하므로(patrol_
        # task.py._deliver_medication과 같은 토픽 재사용) 여기서 또 처리할 필요가
        # 없다(patrol_start의 _on_patrol_result와 같은 이유).
        goal_handle.get_result_async()

    def _handle_medication_alert(self, message: dict):
        """서버 robot_ws.py::_handle_medication_done이 'jammed'(약통은 열렸는데
        RETRIEVAL_TIMEOUT_SEC 안에 못 꺼내감)를 감지하고 오늘 recipient의 미수령
        횟수를 세어 care_event(MED_MISSED/MED_MISSED_STREAK)를 만든 직후 내려보낸다.
        로봇이 지금 그 자리에 있으므로(방금 그 배출을 직접 수행한 로봇이 이 명령을
        받는다) 두 가지를 바로 한다:
          1. text를 음성 안내로 재생(patrol_task.py의 _publish_voice와 같은 토픽 —
             audio_player가 재생) — "약을 복용하지 않았습니다"/"약을 N번 복용하지
             않았습니다".
          2. severity를 실어 record_trigger를 발행 — video_recorder.py가 이벤트
             pre/post 클립을 남긴다(_start_patrol의 SOS 경로와 같은 방식).
             skip_announce=True를 같이 실어서, video_recorder.py가 record_trigger
             마다 기본으로 트는 일반 문구("주의/긴급 이벤트 발생! 영상 녹화를
             시작합니다")는 끈다 — 1번에서 이미 더 구체적인 문구를 안내했으므로
             두 문구가 겹쳐 나오는 걸 막는다(video_recorder.py::_on_trigger 참고).
        """
        text = message.get('text')
        if text:
            self._voice_pub.publish(String(data=json.dumps({'text': text})))

        severity = message.get('severity')
        if severity:
            self._record_trigger_pub.publish(String(data=json.dumps({
                'care_event_id': message.get('care_event_id'),
                'severity': severity,
                'reason': text,
                'skip_announce': True,
            })))

    def _on_patrol_goal_response(self, future):
        """patrol_task의 액션 서버가 goal을 accept/reject한 직후 온다 — accept면 그
        goal_handle을 들고 있어야 나중에 patrol_stop이 취소를 요청할 수 있다.
        """
        goal_handle = future.result()
        if not goal_handle.accepted:
            # patrol_task._handle_goal_request가 이미 실행 중이라 거절한 경우 등 —
            # 취소할 대상 자체가 없다.
            self.get_logger().warn('patrol goal이 거부됐습니다(이미 다른 주행이 진행 중일 수 있음)')
            return
        self._patrol_goal_handle = goal_handle
        result_future = goal_handle.get_result_async()
        self._patrol_result_future = result_future
        result_future.add_done_callback(self._on_patrol_result)

    def _on_patrol_result(self, future):
        # 성공/실패/취소 무엇으로 끝났든(patrol_task.py가 patrol_done을 이미 별도
        # 토픽으로 쏘고 있어 여기서 결과 자체를 또 처리할 필요는 없다) 이 goal은
        # 더 이상 취소 대상이 아니다 — 다음 patrol_stop이 엉뚱한(이미 끝난) goal을
        # 취소 시도하지 않도록 비워둔다. 그 사이 새 goal이 이미 시작돼 다른 handle로
        # 덮어써져 있을 수 있으니, 지금 이 future가 그 handle의 것일 때만 지운다.
        # get_result_async()는 호출할 때마다 새 Future를 만들어 서버에 새
        # GetResult 요청을 보내므로, 여기서 다시 불러 비교하면 항상 다른 객체라
        # 절대 True가 안 된다 — _on_patrol_goal_response에서 저장해둔 그 Future와
        # 비교해야 한다.
        if self._patrol_result_future is future:
            self._patrol_goal_handle = None
            self._patrol_result_future = None

    def _stop_patrol(self, message: dict):
        """"주행 취소" — P6 웨이포인트 팝오버의 "주행 취소" 버튼 → 서버
        POST /patrol-runs/cancel → 여기로 온다. 취소할 goal_handle이 없으면(이미
        끝났거나 애초에 시작 응답을 못 받은 경우) 조용히 로그만 남긴다 — patrol_task.py
        쪽 cancel_callback이 ACCEPT해도, 여기서 어느 goal을 취소할지 몰라 요청 자체를
        못 보내면 똑같이 무의미하다(TS-NAV-08과 같은 "취소할 handle이 없어 무시됨"
        패턴 — _patrol_goal_handle 추적으로 이 경우를 없앴다).
        """
        if self._patrol_goal_handle is None:
            self.get_logger().warn('취소할 진행 중인 주행이 없습니다')
            return
        self._patrol_goal_handle.cancel_goal_async()

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
            # _ensure_navigation_running 바깥에서 Nav2를 내리는 유일한 지점이라, 여기서
            # 직접 알려야 한다 — 안 그러면 P6이 이 로봇의 Nav2가 여전히 떠 있는 줄 안다.
            self._send_nav_status(False)
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
        # P5 "초기화"(매핑 취소) — 지금 떠 있는 cartographer_launch.py를 실제로 끈다.
        # 이걸 안 하면 _start_slam이 "이미 SLAM 작업이 진행 중입니다"로 다음 시작을
        # 계속 거부한다. 예전엔 PC가 취소 응답을 받자마자 500ms 뒤 자동으로 slam_start를
        # 다시 보내는 "재시작" 경로가 있었는데(그러면 이 함수의 종료 대기가 실제로
        # 끝났는지가 레이스 컨디션이 됐다 — TS-SLAM-01/02류), 지금은 "초기화"가 취소
        # 하나로 끝나고 "매핑 시작"은 사용자가 나중에 별도로 눌러야 한다 — 자동 체인이
        # 없어졌다. 설령 사용자가 그 두 클릭을 순식간에 연달아 해도, _handle_command가
        # asyncio 이벤트루프에서 완전히 직렬로 돌고(_connect_forever의 `async for raw
        # in ws: self._handle_command(raw)`) 아래 _terminate_process_group이 프로세스
        # 종료를 동기적으로 기다리므로(끝나야 이 함수가 리턴 -> 다음 큐에 든
        # slam_start를 그제서야 처리), 레이스 자체가 구조적으로 발생하지 않는다.
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

        블로킹 트레이드오프: process.wait()가 진짜로 스레드를 막는(비-asyncio)
        호출이라, 이 함수가 리턴할 때까지(최악 SIGINT 5초 + SIGKILL 5초 = 최대
        ~10초) _handle_command를 부른 WS 수신 루프(_connect_forever의 `async for
        raw in ws`) 전체가 멈춘다 — 이 로봇에 오는 다른 어떤 다운링크 명령(cmd_vel,
        다음 slam_start, patrol_stop 등)도 그동안 처리되지 않고 큐에서 기다린다.
        의도한 트레이드오프다: 이 블로킹이 있어서 "취소 명령을 보내놓고 실제
        종료를 확인 안 한 채 다음 명령을 처리하는" 레이스(TS-SLAM-01/02류)가
        구조적으로 없다(_stop_slam 주석 참고) — 만약 나중에 반응성이 문제가 되어
        asyncio executor 등으로 비동기화한다면, 그 대가로 사라지는 이 직렬화
        보장을 다른 방식(예: PC가 "취소 처리 중" 상태를 서버 통해 조회)으로
        대체해야 같은 레이스가 재발하지 않는다.
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
        if self._video_recorder_process is not None and self._video_recorder_process.poll() is None:
            self._terminate_process_group(self._video_recorder_process, 'video_recorder')
            self._video_recorder_process = None
        if self._voice_announcer_process is not None and self._voice_announcer_process.poll() is None:
            self._terminate_process_group(self._voice_announcer_process, 'audio_player')
            self._voice_announcer_process = None
        if self._audio_capture_process is not None and self._audio_capture_process.poll() is None:
            self._terminate_process_group(self._audio_capture_process, 'audio_capture_node')
            self._audio_capture_process = None
        if (self._audio_downlink_player_process is not None
                and self._audio_downlink_player_process.poll() is None):
            self._terminate_process_group(self._audio_downlink_player_process, 'audio_downlink_player')
            self._audio_downlink_player_process = None
        if (self._patrol_intruder_watch_process is not None
                and self._patrol_intruder_watch_process.poll() is None):
            self._terminate_process_group(self._patrol_intruder_watch_process, 'patrol_intruder_watch_node')
            self._patrol_intruder_watch_process = None
        if self._pillbox_controller_process is not None and self._pillbox_controller_process.poll() is None:
            self._terminate_process_group(self._pillbox_controller_process, 'pillbox_controller')
            self._pillbox_controller_process = None
        if self._medication_task_process is not None and self._medication_task_process.poll() is None:
            self._terminate_process_group(self._medication_task_process, 'medication_task')
            self._medication_task_process = None

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
            self._send_nav_status(False)
            return False

        try:
            # map_id가 같아도 서버 쪽 내용이 바뀌었을 수 있어(재매핑 후 같은 이름으로
            # 저장 — _download_map 문서 참고) 재사용 여부를 판단하기 전에 항상 먼저
            # 최신 상태를 받아온다.
            yaml_path, changed = self._download_map(map_id)
        except Exception as exc:  # noqa: BLE001 - 다운로드 실패 원인이 다양해 전부 여기서 로그만 남긴다
            self.get_logger().error(f'맵 다운로드 실패: {exc}')
            self._send_nav_status(False)
            return False

        if self._nav_process is not None and self._nav_process.poll() is None:
            if self._nav_map_id == map_id and not changed:
                self._send_nav_status(True)
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
            self._send_nav_status(False)
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
                # 직후 첫 메시지 유실 문제로 재시도 구간을 늘렸다가(3번/0.6초 ->
                # 15번/약 3초), P6 대시보드 마커 흔들림 피드백으로 2026-09-14에
                # 5번/약 1초로 다시 줄였다. timeout(20s)은 원래 늘렸던 여유
                # (SUBSCRIBER_WAIT_TIMEOUT_SEC 5초 + 발행 구간 약 3초 + margin) 그대로
                # 뒀다 — 실제 소요 시간이 줄었을 뿐이니 그대로 둬도 여유는 더 늘어날
                # 뿐이다. 실 로봇에서 이 값이 짧아 초기 위치 설정이 조용히 실패 ->
                # global_costmap이 activating에 멈추고 bt_navigator가 inactive라
                # 순찰이 전부 무동작으로 끝나는 버그를 실제로 겪었다(2026-09-08).
                capture_output=True, text=True, timeout=20,
            )
            if result.returncode != 0:
                self.get_logger().warn(f'초기 위치 설정 실패(rc={result.returncode}): {result.stderr}')
        except subprocess.TimeoutExpired:
            self.get_logger().warn('초기 위치 설정이 20초를 넘어 중단했습니다')

        self._send_nav_status(True)
        return True

    def _ensure_patrol_task_running(self, map_id: int, override: bool = False) -> bool:
        """patrol_task(careguard_mission)가 이 map_id로 이미 떠있는지 확인하고,
        아니면 (재)시작한다 — CAREGUARD_MAP_ID를 정확히 넣어서.

        patrol_task는 시작 시점에 딱 한 번 WaypointManager.fetch(CAREGUARD_MAP_ID)로
        충전 스테이션 좌표를 캐시해두고, 순찰이 끝날 때 그 캐시로 복귀 지점을 찾는다
        (careguard_nav/dock_return.py). 이 env var를 사람이 SSH로 직접 맞춰서 켜야
        했는데, 지도를 바꾼 뒤 깜빡하거나 아예 안 넣고 켜면(2026-09-08 실 로봇에서
        실제로 겪음) 캐시가 비어(또는 예전 지도 것으로 남아) "순찰 후 충전소 복귀"가
        조용히 빈 동작으로 끝나거나 엉뚱한 좌표로 갔다 — _ensure_navigation_running과
        같은 이유·같은 패턴으로 이 노드가 대신 관리해 사람이 신경 안 써도 되게 한다.

        map_id가 같아도 무조건 재시작한다(override로 실제로 재시작할 때 한정) —
        _ensure_navigation_running과 달리 여기선 map_id 하나만 보고 "이미 이
        지도로 떠있다"고 재사용하면 안 된다. map(robot_id, name) UNIQUE 제약
        때문에 같은 이름으로 재매핑해도 map_id가 안 바뀌는데, 그러면 충전
        스테이션 좌표는 새로 매핑한 값으로 바뀌었어도 patrol_task는 최초 기동
        때 캐시한 예전 좌표를 계속 쓴다 — "지도를 바꿔도 이전 지도 기준으로
        동작한다"는 리포트(2026-09-09)와 같은 원인의 버그다.

        override=False(기본값)이고 지금 실제로 goal을 실행 중이면(_patrol_goal_handle이
        있으면) 아무것도 안 하고 그대로 True를 돌려준다 — "이 함수는 순찰이 실제로
        진행 중일 땐 호출되지 않는다"는 예전 가정이 실제로는 깨져서(nav_prepare가
        P6 페이지를 새로고침·맵 전환만 해도 호출된다) 진행 중인 goal이 조용히
        SIGINT로 죽는 사고가 있었다(2026-09-15, R-07 스케줄 순찰). override=True는
        ESP32 SOS·저배터리 자동 복귀처럼 지금 뭘 하고 있든 밀어내고 우선 실행돼야
        하는 호출부만 쓴다(_start_patrol 참고) — 이때는 예전처럼 무조건 kill+재시작한다.
        """
        if self._patrol_goal_handle is not None and not override:
            return True

        if self._patrol_task_process is not None and self._patrol_task_process.poll() is None:
            self._terminate_process_group(self._patrol_task_process, 'patrol_task')
            self._patrol_task_process = None
            self._patrol_task_map_id = 0
        # 이전 patrol_task 프로세스를 죽이면 그 액션 서버에 대해 들고 있던 goal_handle도
        # 더 이상 유효하지 않다 — 방어적으로 비워둔다(문서화된 대로 정상 흐름에서는
        # 순찰 진행 중엔 이 함수가 안 불리지만, 혹시 모를 경합에 대비).
        self._patrol_goal_handle = None
        self._patrol_result_future = None

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

    def _ensure_video_recorder_running(self) -> bool:
        """video_recorder(careguard_perception)가 이미 떠있는지 확인하고, 아니면
        새로 띄운다. _ensure_patrol_task_running과 같은 패턴이지만 map_id에는
        묶이지 않는다 — 카메라 프레임을 계속 버퍼링하다가 이벤트/순찰 트리거가
        오면 클립을 만들 뿐이라 특정 맵을 알 필요가 없다.

        이게 없으면(2026-09-09 실 로봇 테스트로 확인) patrol_task가 순찰 시작/종료
        신호를 patrol_recording 토픽으로 잘 보내도 구독하는 노드가 아예 없어 순찰
        영상이 하나도 안 찍혔다 — patrol_task를 이 노드가 대신 띄우는 것과 같은
        이유·같은 패턴으로 video_recorder도 대신 관리해 사람이 SSH로 따로 안
        켜도 되게 한다.

        실패해도(카메라 드라이버 미기동 등으로 노드 자체는 뜨지만 프레임이 안
        들어올 수 있다) 순찰 자체를 막지는 않는다 — 호출부가 반환값을 무시해도
        되도록 실패 시에도 False만 돌려주고 예외를 던지지 않는다.
        """
        if self._video_recorder_process is not None and self._video_recorder_process.poll() is None:
            return True  # 이미 떠있다 — 재사용

        # _nav_process/_patrol_task_process와 같은 이유 — 이 다운링크가 재시작된
        # 적이 있으면 이전 생애에 띄운 video_recorder가 고아로 남아있을 수 있다.
        self._kill_external_process_group('careguard_perception video_recorder', '(정리) 이전 video_recorder')

        self._video_recorder_process = subprocess.Popen(
            ['ros2', 'run', 'careguard_perception', 'video_recorder'], start_new_session=True)

        # count_subscribers로 "노드가 떴는지"가 아니라 "patrol_recording 토픽을 실제로
        # 구독하기 시작했는지"를 확인한다 — 호출부(_prepare_navigation/_start_patrol)가
        # 이 뒤에 곧바로 patrol_task를 띄우면 patrol_task가 시작하자마자 이 토픽에
        # publish하므로, 구독이 아직 안 잡힌 채로 그 메시지가 나가면 조용히 유실된다.
        deadline = time.monotonic() + VIDEO_RECORDER_READY_TIMEOUT_SEC
        while time.monotonic() < deadline:
            if self.count_subscribers(PATROL_RECORDING_TOPIC) > 0:
                return True
            time.sleep(0.3)
        self.get_logger().warn(
            f'{VIDEO_RECORDER_READY_TIMEOUT_SEC:.0f}초 안에 video_recorder가 patrol_recording 토픽을 '
            '구독하지 않았습니다 — 순찰은 계속 진행하지만 영상이 안 찍힐 수 있습니다')
        return False

    def _ensure_voice_announcer_running(self) -> bool:
        """audio_player(careguard_hmi)가 이미 떠있는지 확인하고, 아니면 새로
        띄운다. _ensure_video_recorder_running과 완전히 같은 패턴 — map_id에
        안 묶이고, 준비 실패해도(piper-tts/음성 모델 미설치·스피커 미연결 등) 순찰 자체를
        막지 않는다(호출부가 반환값을 무시해도 되게 False만 돌려준다).
        """
        if self._voice_announcer_process is not None and self._voice_announcer_process.poll() is None:
            return True  # 이미 떠있다 — 재사용

        self._kill_external_process_group('careguard_hmi audio_player', '(정리) 이전 audio_player')

        self._voice_announcer_process = subprocess.Popen(
            ['ros2', 'run', 'careguard_hmi', 'audio_player'], start_new_session=True)

        deadline = time.monotonic() + VOICE_ANNOUNCER_READY_TIMEOUT_SEC
        while time.monotonic() < deadline:
            if self.count_subscribers(VOICE_TOPIC) > 0:
                return True
            time.sleep(0.3)
        self.get_logger().warn(
            f'{VOICE_ANNOUNCER_READY_TIMEOUT_SEC:.0f}초 안에 audio_player가 안내 토픽을 '
            '구독하지 않았습니다 — 순찰은 계속 진행하지만 음성 안내가 안 나올 수 있습니다')
        return False

    def _ensure_audio_capture_running(self) -> bool:
        """audio_capture_node(careguard_hmi, 통화용 마이크 캡처)가 이미 떠있는지
        확인하고, 아니면 새로 띄운다. _ensure_voice_announcer_running과 같은
        패턴이지만, 여러 노드가 같은 CALL_STATE_TOPIC을 구독해 count_subscribers로는
        "이 노드가 구독했는지"를 구분할 수 없다 — 실제 캡처는 사람이 한참 뒤에
        "통화 시작"을 눌러야 시작되니, video_recorder처럼 즉시 publish되는 메시지와
        경쟁할 일이 없어 프로세스만 띄우고 바로 반환해도 안전하다.
        """
        if self._audio_capture_process is not None and self._audio_capture_process.poll() is None:
            return True  # 이미 떠있다 — 재사용

        self._kill_external_process_group('careguard_hmi audio_capture_node', '(정리) 이전 audio_capture_node')
        self._audio_capture_process = subprocess.Popen(
            ['ros2', 'run', 'careguard_hmi', 'audio_capture_node'], start_new_session=True)
        return True

    def _ensure_audio_downlink_player_running(self) -> bool:
        """audio_downlink_player(careguard_hmi, 통화용 스피커 재생)가 이미 떠있는지
        확인하고, 아니면 새로 띄운다. _ensure_audio_capture_running과 같은 이유로
        구독 확인 없이 프로세스만 띄운다."""
        if (self._audio_downlink_player_process is not None
                and self._audio_downlink_player_process.poll() is None):
            return True  # 이미 떠있다 — 재사용

        self._kill_external_process_group(
            'careguard_hmi audio_downlink_player', '(정리) 이전 audio_downlink_player')
        self._audio_downlink_player_process = subprocess.Popen(
            ['ros2', 'run', 'careguard_hmi', 'audio_downlink_player'], start_new_session=True)
        return True

    def _ensure_patrol_intruder_watch_running(self) -> bool:
        """patrol_intruder_watch_node(careguard_perception)가 이미 떠있는지 확인하고,
        아니면 새로 띄운다. _ensure_video_recorder_running과 같은 패턴이지만 map_id에는
        안 묶인다(video_recorder와 같은 이유 — 카메라 프레임 처리에 특정 맵 지식이
        필요 없다).

        이게 없으면 patrol_task가 순찰 시작 신호를 patrol_recording 토픽으로 잘 보내도
        구독하는 인식 노드가 없어 대상자/침입자 판별이 아예 안 됐다 — video_recorder를
        이 노드가 대신 띄우는 것과 같은 이유·같은 패턴이다.

        video_recorder/audio_player와 달리 __init__ 시점(상시 대기)이 아니라 여기
        (_start_patrol, 실제 순찰 시작 직전)에서만 부른다 — 클래스 docstring의
        _patrol_intruder_watch_process 주석 참고.

        준비 확인은 video_recorder와 같은 PATROL_RECORDING_TOPIC 구독 여부로 하되,
        video_recorder도 같은 토픽을 구독하므로 절대 개수(>0)가 아니라 "이 함수
        호출 직전 대비 늘었는지"(기준선 대비 증가)로 판단한다 — 안 그러면 이미
        떠있는 video_recorder의 구독만 보고 patrol_intruder_watch_node가 뜨기도
        전에 "준비됨"으로 잘못 판단할 수 있다.

        실패해도(GPU 메모리 부족 등으로 모델 로딩이 오래 걸리거나 실패할 수 있다)
        순찰 자체를 막지는 않는다.
        """
        if (self._patrol_intruder_watch_process is not None
                and self._patrol_intruder_watch_process.poll() is None):
            return True  # 이미 떠있다 — 재사용

        self._kill_external_process_group(
            'careguard_perception patrol_intruder_watch_node', '(정리) 이전 patrol_intruder_watch_node')

        baseline = self.count_subscribers(PATROL_RECORDING_TOPIC)
        self._patrol_intruder_watch_process = subprocess.Popen(
            ['ros2', 'run', 'careguard_perception', 'patrol_intruder_watch_node'], start_new_session=True)

        deadline = time.monotonic() + PATROL_INTRUDER_WATCH_READY_TIMEOUT_SEC
        while time.monotonic() < deadline:
            if self.count_subscribers(PATROL_RECORDING_TOPIC) > baseline:
                return True
            time.sleep(0.3)
        self.get_logger().warn(
            f'{PATROL_INTRUDER_WATCH_READY_TIMEOUT_SEC:.0f}초 안에 patrol_intruder_watch_node가 '
            'patrol_recording 토픽을 구독하지 않았습니다 — 순찰은 계속 진행하지만 대상자/침입자 '
            '판별이 안 될 수 있습니다')
        return False

    def _ensure_pillbox_controller_running(self) -> bool:
        """pillbox_controller(careguard_hw)가 이미 떠있는지 확인하고, 아니면 새로
        띄운다. _ensure_video_recorder_running과 같은 패턴 — map_id에 안 묶이고
        (아두이노 시리얼 브리지일 뿐 맵 지식이 필요 없다), __init__ 시점에 상시
        띄워둔다(video_recorder/audio_player와 같은 이유 — P3 "복약 재안내"는
        순찰과 무관하게 아무 때나 올 수 있다).

        준비 확인은 PillboxSlot 서비스가 실제로 advertise됐는지로 한다
        (self._pillbox_client.wait_for_service) — count_subscribers 기반 판단과
        달리 서비스는 클라이언트가 직접 물어볼 수 있어 더 정확하다.

        실패해도(아두이노 미연결/시리얼 포트 오류 등) 이 노드 자체는 계속 뜬다 —
        medication_task._open_slot()이 실제로 배출을 시도할 때 자기 타임아웃으로
        hw_error를 내는 게 맞는 실패 경로이지, 여기서 다운링크 전체를 막을 이유는
        없다.
        """
        if self._pillbox_controller_process is not None and self._pillbox_controller_process.poll() is None:
            return True  # 이미 떠있다 — 재사용

        self._kill_external_process_group('careguard_hw pillbox_controller', '(정리) 이전 pillbox_controller')

        self._pillbox_controller_process = subprocess.Popen(
            ['ros2', 'run', 'careguard_hw', 'pillbox_controller'], start_new_session=True)

        if not self._pillbox_client.wait_for_service(timeout_sec=PILLBOX_CONTROLLER_READY_TIMEOUT_SEC):
            self.get_logger().warn(
                f'{PILLBOX_CONTROLLER_READY_TIMEOUT_SEC:.0f}초 안에 pillbox_controller가 '
                'PillboxSlot 서비스를 advertise하지 않았습니다 — 복약 배출이 hw_error로 실패할 수 있습니다')
            return False
        return True

    def _ensure_medication_task_running(self) -> bool:
        """medication_task(careguard_mission)가 이미 떠있는지 확인하고, 아니면
        새로 띄운다. _ensure_pillbox_controller_running과 같은 이유로 __init__
        시점에 상시 띄워둔다 — patrol_task.py의 medication_client.py(순찰 중
        자동 경로)와 이 노드 자신의 self._medication_client(P3 수동 재안내) 둘 다
        이 액션 서버 하나를 공유하므로, 어느 경로가 먼저 오든 이미 떠있어야 한다.

        준비 확인은 patrol_task와 같은 방식 — 이 노드가 이미 들고 있는
        self._medication_client.wait_for_server()로 확인한다.
        """
        if self._medication_task_process is not None and self._medication_task_process.poll() is None:
            return True  # 이미 떠있다 — 재사용

        self._kill_external_process_group('careguard_mission medication_task', '(정리) 이전 medication_task')

        self._medication_task_process = subprocess.Popen(
            ['ros2', 'run', 'careguard_mission', 'medication_task'], start_new_session=True)

        if not self._medication_client.wait_for_server(timeout_sec=MEDICATION_TASK_READY_TIMEOUT_SEC):
            self.get_logger().warn(
                f'{MEDICATION_TASK_READY_TIMEOUT_SEC:.0f}초 안에 medication_task 액션 서버가 '
                '안 떴습니다 — 복약 배출 요청이 응답 없이 실패할 수 있습니다')
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
