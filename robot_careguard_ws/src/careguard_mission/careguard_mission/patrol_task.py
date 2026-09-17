"""careguard_mission/patrol_task.py

careguard_msgs/action/Patrol 액션 서버. careguard_agent/downlink_node.py가
서버의 "patrol_start" 명령을 받아 이 액션을 호출한다(goal에 이미 방문할
웨이포인트 좌표가 실려 있다 — route_id를 로봇이 다시 조회할 필요가 없다).
좌표 순서대로 Nav2로 이동시키고, 끝나면 충전소로 복귀한다.

진행률은 액션 feedback과 /careguard/patrol_status 토픽(JSON 문자열) 양쪽에
발행한다 — careguard_agent/uplink_node.py가 그 토픽을 구독해 서버로 relay한다
(careguard_agent만 서버/WS를 직접 만지도록 하는 "유일한 외부 통신구" 원칙을
지키기 위해, 이 노드는 서버를 직접 호출하지 않는다).

실제 순찰(return_to_charging=True — P6 "이동" 단일 지점 주행은 여기 해당 안 됨)일
때만, 시작 직후와 끝(성공/실패/예외 불문) 시점에 /careguard/perception/
patrol_recording으로 순찰 연속 녹화 시작/종료 신호도 발행한다.
careguard_perception/video_recorder.py가 그 신호를 구독해 순찰 전체를 이어
찍는다(이벤트 pre/post 클립과는 별개 — 동시에 진행될 수 있다). 이 토픽명은
careguard_perception 쪽 config.PATROL_RECORDING_TOPIC과 같은 문자열을 그대로
중복 정의한 것 — 이 코드베이스가 patrol_status 등에서 이미 쓰는 방식(패키지 간
공유 없이 토픽명을 각자 하드코딩)을 그대로 따른다.

같은 시점들(시작/웨이포인트 실패/충전소 복귀 실패/완료)에 /careguard/voice/
announce로 한국어 음성 안내 문장도 함께 발행한다. careguard_hmi/
audio_player.py가 그 문장을 그대로 TTS(Piper)로 합성해 스피커로 재생한다 —
"지금 촬영 중"이라는 걸 그 공간에 있는 사람에게 소리로도 알리는 역할을 겸한다.

이 노드는 Nav2가 이미 떠 있다고 가정한다(teleop_launch.py처럼 자동으로
띄우지 않음) — 순찰 전에 사전에 별도로
`ros2 launch careguard_bringup navigation_diff_launch.py map:=<등록된 맵 yaml 경로>`
를 실행해 amcl·costmap·bt_navigator 등을 먼저 올려둬야 한다.

"복약 전달" 경로(goal.medication_schedule_id > 0)일 때는 모든 웨이포인트를 다
돈 "다음"이 아니라, 순찰 도중 언제든 PERSON_DETECTED_TOPIC으로 사람을 찾는
즉시 — 남은 웨이포인트를 더 돌지 않고 — 음성 안내("복약을 권장합니다...")를
내보내고 medication_client.py를 통해 DeliverMedication 액션을 호출한다(끝까지
아무도 못 찾으면 배출을 시도하지 않고 그냥 순찰을 마친다 — _execute() 하단
person_found 분기 참고).

접근(approach) 끼어들기 — 2026-09-15 추가:
careguard_perception/patrol_intruder_watch_node.py가 순찰 중 사람을 감지하면(먼
거리라 인식 정확도가 낮을 수 있어), 더 가까이 가서 다시 확인하고 싶다는 요청을
APPROACH_REQUEST_TOPIC으로 보낸다. 이 노드는 그 요청을 받으면 지금 실행 중인
웨이포인트(또는 충전소 복귀) 이동을 "취소"가 아니라 "잠깐 다른 곳에 다녀오는 것"으로
처리한다:
  1. 지금 가던 목적지로의 Nav2 이동을 중단한다(cancel_check가 진짜 취소 요청과
     접근 요청을 함께 감시 — _interrupt_requested 참고).
  2. 접근 좌표로 이동한다(WaypointManager.navigate_to_point, 캐시 밖 임의 좌표).
  3. 도착 여부를 APPROACH_ARRIVED_TOPIC으로 알린다.
  4. 도착했으면, patrol_intruder_watch_node가 그 자리에서 다시 판정을 마치고
     APPROACH_RELEASE_TOPIC으로 "이제 가도 됨" 신호를 줄 때까지 그 자리에서
     기다린다(최대 APPROACH_RELEASE_WAIT_TIMEOUT_SEC — 응답이 안 와도 순찰이
     영원히 멈추지 않게 하는 안전장치).
  5. 원래 가던 목적지로 같은 웨이포인트를 다시 보낸다 — Nav2가 로봇의 지금(접근
     이동 후) 위치에서 새로 경로를 계산하므로, 중간 지점을 따로 저장/복원할 필요
     없이 "재개"가 자연스럽게 된다.

접근 요청은 한 번에 하나만 처리한다(_approaching 플래그) — 처리 중에 새 요청이
오면 무시한다(perception 쪽도 5분당 최대 2회로 스스로 제한하므로 정상 상황에서는
겹칠 일이 거의 없다).

빌드/실행 미검증.
"""
import json
import os
import threading
import time

import rclpy
from rclpy.action import ActionServer, CancelResponse, GoalResponse
from rclpy.node import Node
from std_msgs.msg import Bool, String

from nav2_simple_commander.robot_navigator import TaskResult

from careguard_msgs.action import Patrol
from careguard_nav.dock_return import return_to_charging
from careguard_nav.waypoint_manager import WaypointManager

from .medication_client import MedicationClient

# medication_task.py의 같은 이름 상수와 동일한 문자열이어야 한다 — 이 코드베이스가
# PATROL_RECORDING_TOPIC 등에서 이미 쓰는 방식(패키지 간 공유 없이 토픽명을 각자
# 하드코딩)을 그대로 따른다. 아직 이 토픽을 실제로 발행하는 실시간 인식 노드는
# 없다(medication_task.py 상단 주석 참고) — 이 구독은 "언젠가 생길 발행자"를
# 기다리는 자리다.
PERSON_DETECTED_TOPIC = '/careguard/perception/person_detected'

# careguard_perception/config.py의 APPROACH_REQUEST_TOPIC/APPROACH_ARRIVED_TOPIC/
# APPROACH_RELEASE_TOPIC과 같은 문자열 — PATROL_RECORDING_TOPIC 등과 같은 기존
# 방식(패키지 간 공유 없이 토픽명을 각자 하드코딩)을 그대로 따른다.
APPROACH_REQUEST_TOPIC = '/careguard/perception/approach_request'
APPROACH_ARRIVED_TOPIC = '/careguard/perception/approach_arrived'
APPROACH_RELEASE_TOPIC = '/careguard/perception/approach_release'

# 접근 후 재판별 완료(release) 신호를 이 시간 안에 못 받으면 그냥 순찰을 재개한다 —
# perception 쪽이 죽거나 버그가 있어도 로봇이 그 자리에 영원히 멈춰있지 않게 하는
# 안전장치.
APPROACH_RELEASE_WAIT_TIMEOUT_SEC = 20.0

# 2026-09-17 추가 — 복약 배출 시도(_deliver_medication)가 hw_error처럼 너무 빨리
# (6~12초 만에) 실패해버리면, 대상자가 미처 반응하기도 전에 로봇이 다음 목적지로
# 출발해버리는 문제가 실기에서 확인됐다. 결과(delivered/timeout/jammed/hw_error)와
# 무관하게 이 시간이 지날 때까지는 무조건 그 자리에 멈춰있게 한다 — medication_task.py의
# RETRIEVAL_TIMEOUT_SEC(정상 흐름의 최대 회수 대기시간)과 같은 값으로 맞춰서, "정상
# 흐름이었다면 어차피 이만큼은 기다렸을 시간"만큼만 보장한다. 취소 요청(cancel)이
# 오면 이 강제 대기도 즉시 풀어준다 — 사용자의 취소 의사를 억지로 무시하면 안 된다.
MIN_MEDICATION_STOP_SEC = 20.0


class PatrolTask(Node):
    def __init__(self):
        super().__init__('patrol_task')
        self._status_pub = self.create_publisher(String, '/careguard/patrol_status', 10)
        # careguard_perception/config.py의 PATROL_RECORDING_TOPIC과 같은 문자열.
        self._patrol_recording_pub = self.create_publisher(
            String, '/careguard/perception/patrol_recording', 10)
        # careguard_hmi/config.py의 VOICE_TOPIC과 같은 문자열.
        self._voice_pub = self.create_publisher(String, '/careguard/voice/announce', 10)
        self._waypoint_manager = WaypointManager()
        # DeliverMedication 액션 클라이언트(블로킹 래퍼) — medication_client.py 상단
        # 주석 참고. medication_task.py 준비 여부는 여기서 확인 안 하고 실제로 배출이
        # 필요한 순간(_deliver_medication)에 처음 wait_for_server를 건다.
        self._medication_client = MedicationClient()

        # "복약 전달" 경로에서 순찰 도중(웨이포인트 이동 중이든 각 지점 도착
        # 직후든) 사람을 찾았는지 — _execute()가 매 웨이포인트마다 확인해서, 찾는
        # 즉시 남은 지점을 더 돌지 않고 바로 음성 안내+배출로 넘어간다. 구독 자체는
        # 노드 수명 내내 켜둔다(매 goal마다 새로 만들 이유가 없다 — video_recorder
        # 구독 패턴과 같음) — _execute() 진입 시 clear()로 이전 goal의 신호가
        # 안 새어 들어오게만 막는다.
        self._person_detected = threading.Event()
        self.create_subscription(Bool, PERSON_DETECTED_TOPIC, self._on_person_detected, 10)

        # 순찰 경로 좌표는 매번 goal에 실려 오지만, 복귀 지점(충전 스테이션)은 goal에
        # 없다 — CAREGUARD_MAP_ID로 한 번 캐시를 채워 charging_waypoint()가 동작하게 한다.
        map_id = int(os.environ.get('CAREGUARD_MAP_ID', '0') or 0)
        if map_id > 0:
            try:
                self._waypoint_manager.fetch(map_id)
            except Exception as exc:  # noqa: BLE001 - 최소 구현, 실패해도 액션 서버는 띄운다
                self.get_logger().warn(f'웨이포인트 캐시 갱신 실패: {exc}')

        # rclpy ActionServer는 기본적으로 받아들인 목표마다 execute_callback을 별도
        # 스레드에서 동시에 실행한다 — goal_callback을 안 두면(예전 코드) 사용자가
        # 짧은 시간에 웨이포인트를 두세 번 클릭했을 때 각 요청이 전부 accept되어
        # 동시에 _execute가 돌기 시작한다. 그런데 self._waypoint_manager 안의
        # BasicNavigator 인스턴스는 하나뿐이고 goal_handle/result_future 같은 상태를
        # 인스턴스 변수로 들고 있어서, 두 스레드가 동시에 goToPose를 부르면 서로의
        # 목표를 가로채며 방해해 결국 아무 목표도 끝까지 못 간다 — 실제로 겪은 버그
        # (2026-09-08): "이동"을 짧은 간격으로 여러 번 눌렀더니 로봇이 아예 안 움직임.
        # _handle_goal_request가 이미 실행 중이면 새 요청을 거절해 한 번에 하나만
        # 돌게 막는다.
        self._executing = False
        self._action_server = ActionServer(
            self, Patrol, 'patrol', self._execute, goal_callback=self._handle_goal_request,
            cancel_callback=self._handle_cancel_request)

        # ── 접근(approach) 끼어들기 상태 — 클래스 docstring 참고. _pending_approach/
        # _approaching은 구독 콜백(spin 스레드)과 _execute(액션 실행 스레드)가 함께
        # 건드리므로 _approach_lock으로 보호한다.
        self._approach_lock = threading.Lock()
        self._pending_approach = None  # 아직 처리 시작 안 한 요청
        self._approaching = False  # _run_approach 실행 중(이동+재판별 대기 포함)
        self._pending_release_track_id = None
        self._release_event = threading.Event()
        self._approach_arrived_pub = self.create_publisher(String, APPROACH_ARRIVED_TOPIC, 10)
        self.create_subscription(String, APPROACH_REQUEST_TOPIC, self._on_approach_request, 10)
        self.create_subscription(String, APPROACH_RELEASE_TOPIC, self._on_approach_release, 10)

    def _handle_goal_request(self, goal_request):
        if self._executing:
            self.get_logger().warn('이미 순찰/이동 중이라 새 요청을 거부합니다 — 끝난 뒤 다시 시도하세요')
            return GoalResponse.REJECT
        return GoalResponse.ACCEPT

    def _handle_cancel_request(self, goal_handle):
        # 예전엔 cancel_callback을 아예 안 줘서 rclpy 기본값(모든 취소 요청 REJECT)이
        # 적용됐다 — "주행 취소" 자체가 구조적으로 불가능했다. 여기서 ACCEPT해도
        # _execute의 웨이포인트 루프가 goal_handle.is_cancel_requested를 직접 확인
        # (WaypointManager._go_through의 cancel_check)하지 않으면 여전히 안 멈춘다 —
        # 그 확인은 아래 _execute에서 한다.
        self.get_logger().info('취소 요청 수신')
        return CancelResponse.ACCEPT

    def _publish_status(self, **fields):
        msg = String()
        msg.data = json.dumps(fields)
        self._status_pub.publish(msg)

    def _publish_patrol_recording(self, action, run_id):
        msg = String()
        msg.data = json.dumps({'action': action, 'run_id': run_id})
        self._patrol_recording_pub.publish(msg)

    def _publish_voice(self, text):
        msg = String()
        msg.data = json.dumps({'text': text})
        self._voice_pub.publish(msg)

    def _on_person_detected(self, msg: Bool):
        if msg.data:
            self._person_detected.set()

    def _deliver_medication(self, goal_handle):
        """DeliverMedication 액션을 블로킹으로 호출하고 결과에 따라 음성 안내만
        얹는다 — 서버로의 relay(medication_done, /careguard/patrol_status)는
        medication_task.py._execute()의 finally에서 자신이 직접 발행한다(여기서
        또 발행하면 이중 발행이 된다). medication_task.py가 그쪽에서 직접 발행하는
        이유: 이 액션은 patrol_task.py를 안 거치는 경로(P3 수동 재안내,
        downlink_node.py가 직접 호출)도 있어서, 여기서만 relay하면 그 경로의
        결과가 서버에 아예 안 가기 때문이다.

        medication_client.py 상단 주석 참고 — 여기서 블로킹으로 기다려도
        _execute() 자체가 이미 goal 전용 스레드에서 돌고 있어 다른 goal 처리를
        막지 않는다(patrol_task는 어차피 goal 하나만 동시 실행 — _executing 가드).

        서버 쪽 ws/robot_ws.py는 아직 "medication_done" 타입을 처리하지 않는다 —
        받는 쪽(medication_log INSERT + 대시보드 broadcast)을 추가해야 이 값이
        실제로 medication_log에 'delivered'로 남는다(별도 TODO, patrol.py::
        start_patrol_run이 slot_no를 채워 보내는 서버쪽 변경과 짝을 맞춰야 함).
        """
        schedule_id = goal_handle.request.medication_schedule_id
        slot_no = goal_handle.request.slot_no
        self.get_logger().info(f'복약 전달 시작: schedule_id={schedule_id} slot_no={slot_no}')
        started_at = time.monotonic()
        # skip_person_check=True — 여기 도달했다는 것 자체가 _execute()의 웨이포인트
        # 루프가 이미 self._person_detected로 사람을 확인했다는 뜻이라(호출부 참고),
        # medication_task.py가 또 기다리게 하지 않는다(DeliverMedication.action 주석).
        status = self._medication_client.deliver(
            slot_no=slot_no, schedule_id=schedule_id, skip_person_check=True,
            cancel_check=lambda: goal_handle.is_cancel_requested)
        self.get_logger().info(f'복약 전달 결과: {status}')
        if status == 'delivered':
            self._publish_voice('약을 전달했습니다.')
        elif status == 'timeout':
            # hw_error는 여기서 안내하지 않는다 — 통신 자체가 끊긴 상황이라 로봇
            # 스피커까지 정상 동작할지 알 수 없고, 억지로 재생 시도하다 또 다른
            # 예외를 만들 이유가 없다(patrol_task._execute의 다른 실패 경로들과
            # 같은 "확실한 것만 안내한다" 원칙).
            self._publish_voice('약 전달에 실패했습니다. 담당자에게 알려드릴게요.')
        # status == 'jammed'는 여기서 일반 문구를 안내하지 않는다 — robot_ws.py::
        # _handle_medication_done이 오늘 미수령 횟수를 세어 더 구체적인 문구("약을
        # 복용하지 않았습니다"/"약을 N번 복용하지 않았습니다")를 downlink_node.py의
        # medication_alert 명령으로 곧 내려보낸다(2026-09-16). 여기서 일반 문구까지
        # 먼저 재생하면 몇 초 뒤 그 구체적 문구와 겹쳐서 두 번 안내하게 된다.

        # 2026-09-17 추가 — hw_error처럼 배출 시도 자체가 몇 초 만에 실패해버리면
        # 대상자가 반응할 시간도 없이 바로 다음 목적지로 출발해버리는 문제가 실기에서
        # 확인됐다. status와 무관하게(취소 제외) MIN_MEDICATION_STOP_SEC이 지날 때까지
        # 여기서 더 머문다 — _execute()의 웨이포인트 루프는 이 메서드가 반환해야만
        # 다음 이동을 시작하므로, 이 sleep이 끝나기 전까진 로봇이 물리적으로 못 움직인다.
        if status != 'cancelled':
            remaining = MIN_MEDICATION_STOP_SEC - (time.monotonic() - started_at)
            while remaining > 0 and not goal_handle.is_cancel_requested:
                step = min(0.5, remaining)
                time.sleep(step)
                remaining -= step

    # ---------- 접근(approach) 끼어들기 ----------

    def _on_approach_request(self, msg: String):
        if not self._executing:
            self.get_logger().warn('순찰/이동 중이 아니라 접근 요청을 무시합니다')
            return
        try:
            payload = json.loads(msg.data)
        except ValueError:
            self.get_logger().warn(f'접근 요청 JSON 파싱 실패, 무시합니다: {msg.data!r}')
            return
        with self._approach_lock:
            if self._approaching or self._pending_approach is not None:
                self.get_logger().warn('이미 접근 처리 중이라 새 요청을 무시합니다')
                return
            self._pending_approach = payload

    def _on_approach_release(self, msg: String):
        try:
            payload = json.loads(msg.data)
        except ValueError:
            return
        if payload.get('track_id') == self._pending_release_track_id:
            self._release_event.set()

    def _take_pending_approach(self):
        with self._approach_lock:
            approach = self._pending_approach
            self._pending_approach = None
            return approach

    def _interrupt_requested(self, goal_handle) -> bool:
        if goal_handle.is_cancel_requested:
            return True
        with self._approach_lock:
            return self._pending_approach is not None

    def _publish_approach_arrived(self, track_id, arrived: bool):
        msg = String()
        msg.data = json.dumps({'track_id': track_id, 'arrived': arrived})
        self._approach_arrived_pub.publish(msg)

    def _run_approach(self, approach: dict, goal_handle) -> None:
        track_id = approach.get('track_id')
        with self._approach_lock:
            self._approaching = True
        try:
            self.get_logger().info(f'[접근] track_id={track_id} — 확인을 위해 접근합니다')
            nav_result = self._waypoint_manager.navigate_to_point(
                float(approach.get('x', 0.0)), float(approach.get('y', 0.0)),
                float(approach.get('yaw', 0.0)),
                cancel_check=lambda: goal_handle.is_cancel_requested,
            )
            arrived = nav_result == TaskResult.SUCCEEDED
            self._publish_approach_arrived(track_id, arrived)
            if not arrived:
                self.get_logger().warn(
                    f'[접근] track_id={track_id} — 접근 이동 실패/취소, 판정 없이 순찰을 재개합니다')
                return

            self.get_logger().info(f'[접근] track_id={track_id} 도착 — 재판별 대기 중')
            self._pending_release_track_id = track_id
            self._release_event.clear()
            if not self._release_event.wait(timeout=APPROACH_RELEASE_WAIT_TIMEOUT_SEC):
                self.get_logger().warn(f'[접근] track_id={track_id} 재판별 응답 타임아웃 — 순찰을 재개합니다')
            self._pending_release_track_id = None
        finally:
            with self._approach_lock:
                self._approaching = False

    def _navigate_with_approach_support(self, goal_handle, nav_call):
        """nav_call()을 실행하다가 접근 요청이 오면(_interrupt_requested) 먼저
        _run_approach로 그 요청을 처리한 뒤 nav_call()을 처음부터 다시 부른다 —
        Nav2가 로봇의 현재 위치에서 다시 경로를 계산하므로 중간 지점을 따로
        기억해둘 필요 없이 "재개"가 된다. 진짜 취소 요청은 즉시 반환한다.
        """
        while True:
            nav_result = nav_call()
            if goal_handle.is_cancel_requested:
                return nav_result
            approach = self._take_pending_approach()
            if approach is None:
                return nav_result
            self._run_approach(approach, goal_handle)

    def _execute(self, goal_handle):
        self._executing = True
        # P6 "이동"(단일 웨이포인트 주행, run_id=0)은 녹화 대상이 아니다 — 원래
        # 순찰(여러 지점을 돌고 항상 충전소로 복귀)과 이 필드로 구분한다. 시작/끝
        # 양쪽에서 같은 값을 써야 하므로 try 진입 전에 한 번만 계산해둔다.
        is_patrol = goal_handle.request.return_to_charging
        run_id = goal_handle.request.run_id
        # "복약 전달" 경로인지 — 아래 루프가 매 웨이포인트마다 사람을 찾았는지
        # 확인할지(그리고 남은 지점을 더 안 돌고 바로 배출로 넘어갈지) 이 값 하나로
        # 게이팅한다. 일반 순찰/이동(P6 "이동" 포함)에서는 person_detected를 아예
        # 신경 쓰지 않는다 — 대상자 앞이 아닌 곳에서 사람을 잘못 인식해 순찰이
        # 조기 종료되는 걸 막기 위해서다.
        is_medication_route = goal_handle.request.medication_schedule_id > 0
        if is_medication_route:
            self._person_detected.clear()
        if is_patrol:
            self._publish_patrol_recording('start', run_id)
            self._publish_voice('순찰을 시작합니다. 지금부터 영상을 촬영합니다.')
        try:
            waypoints = goal_handle.request.waypoints
            waypoints_total = len(waypoints)
            self.get_logger().info(f'순찰 시작: run_id={run_id} 지점 {waypoints_total}개')

            person_found = False
            for i, wp in enumerate(waypoints, start=1):
                # 접근(approach) 요청이 오면 이 웨이포인트 이동을 잠깐 멈추고 다녀온 뒤
                # 같은 웨이포인트로 다시 시도한다 — _navigate_with_approach_support 참고.
                # is_medication_route일 때는 person_detected도 같이 cancel_check에
                # 건다 — Nav2가 이동하는 도중에 사람을 찾으면(꼭 지점에 "도착"할 때까지
                # 기다리지 않고) 그 즉시 멈춘다. _navigate_with_approach_support는 멈춘
                # 이유가 "접근 요청"이면 다녀온 뒤 같은 웨이포인트를 다시 시도하고, 그 외
                # (진짜 취소든 person_detected든)에는 바로 반환한다 — person_detected로
                # 멈춘 경우는 아래 person_found 분기가 이어서 처리한다.
                nav_result = self._navigate_with_approach_support(
                    goal_handle,
                    lambda wp=wp: self._waypoint_manager.navigate_poses(
                        [wp], cancel_check=lambda: self._interrupt_requested(goal_handle) or (
                            is_medication_route and self._person_detected.is_set())),
                )

                # WaypointManager._go_through가 cancel_check()에 걸려 중간에 cancelTask()로
                # 내리면 None을 반환한다 — nav_result != SUCCEEDED로도 걸리긴 하지만, 그러면
                # "실패"로 잘못 기록된다(아래에서 다시 안 걸리게 먼저 따로 처리).
                if goal_handle.is_cancel_requested:
                    self.get_logger().info(f'{i}번째 웨이포인트({wp.code}) 이동 중 취소됨')
                    self._publish_status(type='patrol_done', run_id=run_id, status='cancelled')
                    goal_handle.canceled()
                    result = Patrol.Result()
                    result.status = 'cancelled'
                    result.waypoints_done = i - 1
                    return result

                # 사람을 찾았으면(도착해서 성공했든, 이동 중 인식돼 중간에 멈췄든)
                # 남은 웨이포인트는 더 돌지 않는다 — 대상자를 이미 찾았는데 다른
                # 지점을 마저 도는 건 의미가 없고, 오히려 그사이 대상자가 자리를
                # 뜰 수 있다. 실패(nav_result != SUCCEEDED)로 아래에서 또 걸리기
                # 전에 먼저 처리한다.
                if is_medication_route and self._person_detected.is_set():
                    person_found = True
                    self.get_logger().info(f'{i}번째 웨이포인트 근처에서 사람을 인식했습니다 — 복약 안내를 시작합니다')
                    waypoints_done = i if nav_result == TaskResult.SUCCEEDED else i - 1
                    goal_handle.publish_feedback(
                        Patrol.Feedback(waypoints_done=waypoints_done, waypoints_total=waypoints_total))
                    self._publish_status(type='patrol_progress', run_id=run_id, waypoints_done=waypoints_done)
                    self._publish_voice('복약을 권장합니다. 약통에서 약을 배출해드릴게요.')
                    break

                # 예전엔 이 결과를 아예 확인 안 했다 — Nav2 goal이 아무리 실패(FAILED)/
                # 취소(CANCELED)돼도 그냥 다음 지점으로 넘어가거나 끝까지 다 돌고 나서
                # goal_handle.succeed()+status='completed'로 끝냈다. 그래서 bt_navigator가
                # 아직 안 떠서(예: AMCL 초기 위치 설정이 씹혀 costmap이 activating에 멈춘
                # 경우 — 2026-09-08 실 로봇에서 겪음) goToPose가 즉시 실패해도 대시보드/DB
                # 에는 "완료"로만 남고 로봇은 그 자리에서 전혀 안 움직인 채 순찰이 "성공"
                # 처리되는 버그가 있었다. 실패하면 여기서 바로 중단하고 실패를 알린다.
                if nav_result != TaskResult.SUCCEEDED:
                    self.get_logger().error(
                        f'{i}번째 웨이포인트({wp.code}) 이동 실패(result={nav_result}) — 순찰을 중단합니다')
                    self._publish_status(type='patrol_done', run_id=run_id, status='failed')
                    if is_patrol:
                        # P6 "이동"(단일 지점, is_patrol=False)까지 매번 안내하면 오히려
                        # 소음이라 순찰일 때만 안내한다 — 녹화 시작/종료 안내와 같은 게이팅.
                        self._publish_voice(
                            f'{i}번째 지점으로 이동 중 문제가 발생해 순찰을 중단합니다. 영상 촬영을 종료합니다.')
                    goal_handle.abort()
                    result = Patrol.Result()
                    result.status = 'failed'
                    result.waypoints_done = i - 1
                    return result
                goal_handle.publish_feedback(Patrol.Feedback(waypoints_done=i, waypoints_total=waypoints_total))
                self._publish_status(type='patrol_progress', run_id=run_id, waypoints_done=i)

            # 위 루프에서 실제로 사람을 찾았을 때만 배출한다(person_found) —
            # medication_schedule_id > 0이어도 끝까지 아무도 못 찾았으면 그냥
            # 지나간다: 확인도 안 된 상태로 약통을 억지로 열 이유가 없다. 그 경우
            # medication_log에 'delivered'가 안 남으므로 서버 scheduler.py::
            # _medication_tick()이 알아서 MED_MISSED로 잡아낸다(반응형 경로가 이미
            # 있으므로 로봇이 여기서 또 막힐 이유가 없다). 실패(timeout/jammed/
            # hw_error)해도 마찬가지로 순찰 자체를 중단하지는 않는다.
            if person_found:
                self._deliver_medication(goal_handle)

            # P6 "이동"(단일 웨이포인트 주행, run_id=0)은 도착한 자리에 그대로 머물러야
            # 한다 — 원래 순찰(여러 지점을 돌고 항상 충전소로 복귀)과 이 필드(is_patrol)로
            # 구분한다.
            if is_patrol:
                # 위 웨이포인트 루프와 같은 이유로 결과를 확인한다 — 예전엔 이 반환값을
                # 그냥 버려서, 복귀 이동이 Nav2에서 조용히 실패(aborted)해도 로그 한 줄
                # 안 남고 goal_handle.succeed()+'completed'로 끝났다("wp3에서 충전소로
                # 복귀를 안 함" 리포트, 2026-09-08). charge 웨이포인트 자체가 없는
                # 경우(None)와 실제 이동 실패(FAILED/CANCELED)를 구분해서 로그를 남긴다.
                charge_result = self._navigate_with_approach_support(
                    goal_handle,
                    lambda: return_to_charging(
                        self._waypoint_manager, cancel_check=lambda: self._interrupt_requested(goal_handle)),
                )
                if goal_handle.is_cancel_requested:
                    # charge_result도 취소되면 None이라 "충전 웨이포인트가 없음"과
                    # 구분이 안 된다 — 취소 여부를 먼저 확인해서 가린다.
                    self.get_logger().info('충전소 복귀 중 취소됨')
                    self._publish_status(type='patrol_done', run_id=run_id, status='cancelled')
                    goal_handle.canceled()
                    result = Patrol.Result()
                    result.status = 'cancelled'
                    result.waypoints_done = waypoints_total
                    return result
                if charge_result is None:
                    self.get_logger().warn('충전 스테이션 웨이포인트가 없어 복귀를 건너뜁니다')
                elif charge_result != TaskResult.SUCCEEDED:
                    self.get_logger().error(f'충전 스테이션 복귀 실패(result={charge_result})')
                    self._publish_status(type='patrol_done', run_id=run_id, status='failed')
                    # 이 분기는 항상 is_patrol=True일 때만 온다(위 if is_patrol: 블록
                    # 안이라 P6 "이동"은 여기 도달 안 함) — 별도 게이팅 불필요.
                    self._publish_voice('충전소로 복귀하는 중 문제가 발생했습니다. 영상 촬영을 종료합니다.')
                    goal_handle.abort()
                    result = Patrol.Result()
                    result.status = 'failed'
                    result.waypoints_done = waypoints_total
                    return result

            goal_handle.succeed()
            self._publish_status(type='patrol_done', run_id=run_id, status='completed')
            if is_patrol:
                self._publish_voice('순찰을 모두 마쳤습니다. 영상 촬영을 종료합니다.')

            result = Patrol.Result()
            result.status = 'completed'
            result.waypoints_done = waypoints_total
            return result
        finally:
            # 성공/실패(abort) 두 경로 모두, 그리고 위 로직이 던진 예상 못 한 예외
            # (Nav2/rclpy 호출 실패 등)까지 전부 여기서 한 번에 받는다 — 실패 경로마다
            # stop을 따로 넣으면 새 예외 경로가 추가될 때 빠뜨리기 쉽다. video_recorder.py
            # 쪽은 start 없이 stop만 와도(또는 중복으로 와도) 조용히 무시하도록 방어돼
            # 있어 여기서 무조건 한 번 더 보내는 게 안전하다.
            if is_patrol:
                self._publish_patrol_recording('stop', run_id)
            self._executing = False
            # 이 순찰이 끝나는 시점에 아직 소비 안 된 접근 요청이 남아있으면(막 끝나기
            # 직전에 도착한 경우 등) 다음 순찰 시작 때 엉뚱하게 재사용되지 않게 비운다.
            with self._approach_lock:
                self._pending_approach = None


def main(args=None):
    rclpy.init(args=args)
    node = PatrolTask()
    # rclpy.spin(node)(기본 SingleThreadedExecutor)를 쓰면 안 된다 — medication_
    # task.py와 똑같은 버그(그쪽 main()의 주석 참고, 2026-09-11 최소 재현으로 확인):
    # _execute()가 ActionServer 콜백 그룹을 점유한 채(웨이포인트 루프가 도는 내내)
    # 도는 동안, 같은 노드의 _on_person_detected 구독 콜백이 그 그룹이 안 풀릴
    # 때까지 전혀 실행되지 않는다 — 그러면 "복약 전달 경로" 순찰이 person_detected를
    # 아무리 pub해도 절대 못 멈추고 매번 웨이포인트를 다 돌아버린다(이 파일의 핵심
    # 기능이 조용히 죽어있었다는 뜻). MultiThreadedExecutor로 액션 실행과 구독
    # 콜백을 다른 스레드로 동시에 돌려야 한다.
    executor = rclpy.executors.MultiThreadedExecutor()
    executor.add_node(node)
    try:
        executor.spin()
    finally:
        node.destroy_node()
        # SIGINT 시 rclpy가 내부적으로 먼저 shutdown()을 부를 수 있어, 여기서
        # 무조건 또 부르면 "rcl_shutdown already called"로 비정상 종료한다
        # (slam_progress_publisher.py에서 실제로 겪은 버그, 같은 패턴이라 방어).
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
