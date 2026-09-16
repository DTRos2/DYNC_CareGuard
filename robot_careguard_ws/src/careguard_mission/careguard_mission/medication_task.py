"""careguard_mission/medication_task.py

careguard_msgs/action/DeliverMedication 액션 서버. 두 클라이언트가 이 서버 하나를
공유한다(DeliverMedication.action 상단 주석 참고):
  - patrol_task.py의 medication_client.py(블로킹, "복약 전달" 순찰 경로 마지막
    웨이포인트 도착 후)
  - careguard_agent/downlink_node.py(비동기, 서버 "medication_start" WS — P3 수동
    "복약 재안내" 버튼)

patrol_task.py와 달리 이 노드는 map_id/Nav2에 안 묶인다 — video_recorder/
audio_player처럼 한 번 뜨면 계속 재사용되는 노드로 다룰 것(downlink_node.py에
_ensure_medication_task_running 추가는 아직 TODO, 지금은 launch에서 수동으로
같이 띄운다고 가정).

하드웨어 쪽(careguard_hw/pillbox_controller.py + pillbox_firmware.ino)은 실제
프로토콜을 반영했다 — 중요한 건 "닫기"가 이 액션의 책임이 아니라는 것: 펌웨어가
IR 감지 1초 후 스스로 닫으므로(pillbox_controller.py 상단 주석 참고), 이 액션은
"열기"만 명령하고 IR_TRIGGERED_TOPIC으로 오는 "그 채널이 닫혔다" 알림을 그대로
"회수 확인+배출 완료"로 받아들인다(waiting_retrieval 다음에 별도 closing 단계가
없다).

의존하는 한 조각은 이 스코프에서 아직 없다 — "사람 인식": careguard_perception에
실시간 추론 노드가 없다(있는 건 auto_face_capture_node.py, YOLO 학습용 데이터
수집기일 뿐 추론 아님). 여기서는 PERSON_DETECTED_TOPIC(std_msgs/Bool)을 구독하는
것까지만 만들어두고, 그 토픽을 실제로 발행하는 노드는 별도로 만들어야 한다.

빌드/실행 미검증.
"""
import json
import threading

import rclpy
from rclpy.action import ActionServer, CancelResponse, GoalResponse
from rclpy.node import Node
from std_msgs.msg import Bool, String

from careguard_msgs.action import DeliverMedication
from careguard_msgs.srv import PillboxSlot

# careguard_perception 쪽에 아직 없는 실시간 사람 인식 노드가 발행해야 할 토픽.
# True가 한 번이라도 오면 "지금 앞에 사람이 있다"로 본다. auto_face_capture_node.py
# 처럼 패키지 간 공유 없이 토픽명을 각자 하드코딩하는 이 코드베이스의 기존 방식을
# 따른다 — 실제 추론 노드를 만들 때 이 문자열을 그대로 맞춰야 한다.
PERSON_DETECTED_TOPIC = '/careguard/perception/person_detected'

# careguard_hw/pillbox_controller.py가 떠 있어야 advertise되는 서비스/토픽 이름.
# IR_TRIGGERED_TOPIC의 payload는 std_msgs/String이고 값은 'A'/'B'(닫힌 채널) —
# Bool이 아닌 이유는 pillbox_controller.py 상단 주석 참고(여러 채널을 구분해야 함).
PILLBOX_SLOT_SERVICE = '/careguard/hw/pillbox_slot'
IR_TRIGGERED_TOPIC = '/careguard/hw/ir_triggered'

# careguard_hw/pillbox_controller.py의 같은 이름 함수와 동일한 매핑이어야 한다 —
# 패키지 간 공유 없이 각자 하드코딩하는 이 코드베이스의 기존 방식(토픽명과 같은
# 이유). 여기서 필요한 이유: IR_TRIGGERED_TOPIC이 채널 문자('A'/'B')로만 오므로,
# "지금 내가 기다리는 slot_no가 그 채널이 맞는지" 판단하려면 이 매핑이 있어야 한다.
_SLOT_TO_CHANNEL = {1: 'A', 2: 'B', 3: 'A'}

# medication_client.py DEFAULT_PERSON_TIMEOUT_SEC와 맞출 필요는 없다(주석 참고) —
# 호출부가 goal.person_timeout_sec을 항상 채워 보내므로 이 값은 "호출부가 0 이하를
# 보낸 경우"에 대한 방어용 기본값일 뿐이다.
DEFAULT_PERSON_TIMEOUT_SEC = 60.0

# IR 센서로 "꺼냄"(=펌웨어가 스스로 닫음)이 감지되길 기다리는 최대 시간 —
# person_timeout_sec과 별개 상수인 이유: 이건 "이미 약통을 연 뒤" 단계라 사람이
# 눈앞에 없어서 실패하는 person_timeout_sec과는 다른 실패 모드(jammed)로
# 구분해야 한다. 2026-09-16: 5초로 낮춤 — "약통을 열었는데 못 먹었다"를 그 자리에서
# 바로 실패로 잡아 robot_ws.py::_handle_medication_done의 실시간 미수령 경보
# (MED_MISSED/MED_MISSED_STREAK, TTS+녹화)로 즉시 이어지게 하려는 의도적 변경 —
# 예전 30초는 "혹시 몰라 넉넉히 기다린다"는 취지였지만, 그만큼 실패 판단과 보호자
# 알림이 늦어졌다.
RETRIEVAL_TIMEOUT_SEC = 5.0

# hw 서비스(OPEN 명령) 응답을 기다리는 최대 시간. pillbox_controller.py의
# ACK_TIMEOUT_SEC(6초 — 실 서보가 0->150도 움직이는 데 최악의 경우 ≈4.5초 걸림,
# 2026-09-11 실 하드웨어로 확인)보다 반드시 커야 한다 — 안 그러면 서보가 아직
# 움직이는 중인데 이 타임아웃이 먼저 끊겨 hw_error로 잘못 끝난다(실제로 겪은 버그).
HW_SERVICE_TIMEOUT_SEC = 8.0


class MedicationTask(Node):
    def __init__(self):
        super().__init__('medication_task')

        self._person_detected = threading.Event()
        self._ir_triggered = threading.Event()
        # 지금 처리 중인 goal의 채널('A'/'B') — IR_TRIGGERED_TOPIC이 여러 채널을
        # 구분 없이 다 보내므로, 내가 기다리는 채널이 닫혔을 때만 반응하려고 둔다.
        self._waiting_channel = None

        self.create_subscription(Bool, PERSON_DETECTED_TOPIC, self._on_person_detected, 10)
        self.create_subscription(String, IR_TRIGGERED_TOPIC, self._on_ir_triggered, 10)

        # patrol_task.py의 _status_pub과 같은 토픽 문자열을 그대로 중복 정의한다
        # (careguard_perception/config.py의 PATROL_RECORDING_TOPIC 등과 같은,
        # 이 코드베이스의 기존 방식 — 패키지 간 공유 없이 토픽명을 각자 하드코딩).
        # 결과를 여기서 직접 발행해야 하는 이유: 이 액션은 patrol_task.py를 거치지
        # 않고 downlink_node.py가 직접 호출하는 경로(P3 수동 재안내)도 있어서,
        # patrol_task.py 쪽에서만 relay하면 그 경로의 결과가 서버에 아예 안 간다.
        self._status_pub = self.create_publisher(String, '/careguard/patrol_status', 10)

        self._pillbox_client = self.create_client(PillboxSlot, PILLBOX_SLOT_SERVICE)

        # patrol_task.py의 _handle_goal_request와 같은 이유 — 서보/IR 센서는
        # 물리적으로 하나뿐이라 동시에 두 goal을 처리할 수 없다.
        self._executing = False
        self._action_server = ActionServer(
            self, DeliverMedication, 'deliver_medication', self._execute,
            goal_callback=self._handle_goal_request, cancel_callback=self._handle_cancel_request)

    def _handle_goal_request(self, goal_request):
        if self._executing:
            self.get_logger().warn('이미 배출 중이라 새 요청을 거부합니다')
            return GoalResponse.REJECT
        if goal_request.slot_no <= 0:
            self.get_logger().warn(f'유효하지 않은 slot_no={goal_request.slot_no} — 거부')
            return GoalResponse.REJECT
        return GoalResponse.ACCEPT

    def _handle_cancel_request(self, goal_handle):
        return CancelResponse.ACCEPT

    def _on_person_detected(self, msg: Bool):
        if msg.data:
            self._person_detected.set()

    def _on_ir_triggered(self, msg: String):
        if msg.data and msg.data == self._waiting_channel:
            self._ir_triggered.set()

    def _open_slot(self, slot_no: int) -> bool:
        if not self._pillbox_client.wait_for_service(timeout_sec=HW_SERVICE_TIMEOUT_SEC):
            self.get_logger().error(f'하드웨어 서비스({PILLBOX_SLOT_SERVICE})를 찾을 수 없습니다')
            return False
        request = PillboxSlot.Request()
        request.slot_no = slot_no
        future = self._pillbox_client.call_async(request)
        # medication_client.py와 달리 이 노드 자신의 executor를 그대로 쓸 수
        # 있다 — _execute()는 이미 별도 goal 스레드에서 돌고, 이 서비스 콜백
        # 응답은 같은 노드의 다른 콜백이라 spin_until_future_complete를 쓰면
        # 자기 자신을 또 기다리는 재진입 문제가 생긴다(medication_client.py가
        # 전용 노드를 따로 둔 이유와 정확히 같다) — 그래서 여기도 threading.Event로
        # 받는다.
        done = threading.Event()
        result = {'ok': False}

        def _on_done(fut):
            try:
                result['ok'] = bool(fut.result().success)
            except Exception as exc:  # noqa: BLE001 - 서비스 콜백 예외까지 hw_error로 수렴
                self.get_logger().error(f'하드웨어 서비스 호출 실패: {exc}')
            done.set()

        future.add_done_callback(_on_done)
        done.wait(timeout=HW_SERVICE_TIMEOUT_SEC)
        return result['ok']

    def _execute(self, goal_handle):
        self._executing = True
        self._person_detected.clear()
        self._ir_triggered.clear()
        slot_no = goal_handle.request.slot_no
        self._waiting_channel = _SLOT_TO_CHANNEL.get(slot_no)
        timeout_sec = goal_handle.request.person_timeout_sec
        if timeout_sec <= 0:
            timeout_sec = DEFAULT_PERSON_TIMEOUT_SEC

        result = DeliverMedication.Result()
        try:
            if self._waiting_channel is None:
                # goal_callback의 slot_no 검증(1~3)을 이미 통과했으므로 원래
                # 여기 도달할 일은 없다 — 방어적으로만 남긴다.
                goal_handle.abort()
                result.status = 'hw_error'
                return result

            # skip_person_check=True면(DeliverMedication.action 주석 참고 —
            # patrol_task.py가 순찰 도중 자기 구독으로 이미 사람을 확인하고 부른
            # 경우) 여기서 또 기다리지 않는다. 이 노드 자신의 person_detected
            # 구독은 여전히 켜둔다 — skip_person_check=False로 오는 P3 수동 호출을
            # 위해서다.
            if not goal_handle.request.skip_person_check:
                goal_handle.publish_feedback(DeliverMedication.Feedback(phase='waiting_person'))
                if not self._person_detected.wait(timeout=timeout_sec):
                    self.get_logger().warn(f'{timeout_sec}초 안에 사람을 인식하지 못했습니다')
                    goal_handle.abort()
                    result.status = 'timeout'
                    return result
            if goal_handle.is_cancel_requested:
                goal_handle.canceled()
                result.status = 'cancelled'
                return result

            goal_handle.publish_feedback(DeliverMedication.Feedback(phase='opening'))
            if not self._open_slot(slot_no):
                goal_handle.abort()
                result.status = 'hw_error'
                return result

            # "닫기"는 명령하지 않는다 — 펌웨어가 IR 감지 1초 후 스스로 닫고
            # "CLOSED <채널>"을 알려준다(pillbox_controller.py가 그걸 이 토픽으로
            # relay). 그러니 waiting_retrieval을 통과하는 순간이 곧 배출 완료다.
            goal_handle.publish_feedback(DeliverMedication.Feedback(phase='waiting_retrieval'))
            if not self._ir_triggered.wait(timeout=RETRIEVAL_TIMEOUT_SEC):
                self.get_logger().warn(
                    f'{RETRIEVAL_TIMEOUT_SEC}초 안에 회수가 감지되지 않았습니다 — 칸이 열린 채로 남습니다')
                goal_handle.abort()
                result.status = 'jammed'
                return result

            goal_handle.succeed()
            result.status = 'delivered'
            return result
        finally:
            # 위 모든 return 경로가 result.status를 먼저 채워두므로 여기 도달할 땐
            # 항상 값이 있다 — 예상 못 한 예외로 여기 왔다면(rclpy/서비스 호출
            # 자체의 버그 등) 빈 문자열로 남을 수 있어 hw_error로 방어한다.
            self._publish_status(
                schedule_id=goal_handle.request.schedule_id, slot_no=slot_no,
                status=result.status or 'hw_error')
            self._waiting_channel = None
            self._executing = False

    def _publish_status(self, schedule_id: int, slot_no: int, status: str):
        msg = String()
        msg.data = json.dumps({
            'type': 'medication_done', 'schedule_id': schedule_id, 'slot_no': slot_no, 'status': status,
        })
        self._status_pub.publish(msg)


def main(args=None):
    rclpy.init(args=args)
    node = MedicationTask()
    # rclpy.spin(node)(기본 SingleThreadedExecutor)를 쓰면 안 된다 — 실제로 겪은
    # 버그: _execute()가 ActionServer 콜백 그룹을 점유한 채 person_detected/
    # ir_triggered Event를 기다리는 동안, 같은 노드의 구독 콜백(_on_person_detected/
    # _on_ir_triggered)이 그 그룹이 안 풀릴 때까지 큐에만 쌓이고 전혀 실행되지
    # 않는다 — 그래서 Event가 영원히 안 풀려 매번 timeout/jammed로만 끝났다
    # (2026-09-11, 최소 재현으로 직접 확인: SingleThreadedExecutor에서 person_
    # detected를 아무리 pub해도 goal이 자기 타임아웃으로 끝난 "직후"에야 그
    # 콜백들이 한꺼번에 처리됨). MultiThreadedExecutor를 쓰면 액션 실행과 구독
    # 콜백이 서로 다른 스레드로 동시에 돌아 이 문제가 없다.
    executor = rclpy.executors.MultiThreadedExecutor()
    executor.add_node(node)
    try:
        executor.spin()
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
