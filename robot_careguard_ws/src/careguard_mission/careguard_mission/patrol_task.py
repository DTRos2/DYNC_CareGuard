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

import rclpy
from rclpy.action import ActionServer, CancelResponse, GoalResponse
from rclpy.node import Node
from std_msgs.msg import String

from nav2_simple_commander.robot_navigator import TaskResult

from careguard_msgs.action import Patrol
from careguard_nav.dock_return import return_to_charging
from careguard_nav.waypoint_manager import WaypointManager

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
        if is_patrol:
            self._publish_patrol_recording('start', run_id)
            self._publish_voice('순찰을 시작합니다. 지금부터 영상을 촬영합니다.')
        try:
            waypoints = goal_handle.request.waypoints
            waypoints_total = len(waypoints)
            self.get_logger().info(f'순찰 시작: run_id={run_id} 지점 {waypoints_total}개')

            for i, wp in enumerate(waypoints, start=1):
                # 접근(approach) 요청이 오면 이 웨이포인트 이동을 잠깐 멈추고 다녀온 뒤
                # 같은 웨이포인트로 다시 시도한다 — _navigate_with_approach_support 참고.
                nav_result = self._navigate_with_approach_support(
                    goal_handle,
                    lambda wp=wp: self._waypoint_manager.navigate_poses(
                        [wp], cancel_check=lambda: self._interrupt_requested(goal_handle)),
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
