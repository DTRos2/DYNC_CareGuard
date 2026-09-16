"""careguard_mission/medication_client.py

DeliverMedication 액션을 "동기 호출"처럼 쓰기 위한 얇은 래퍼.
careguard_nav/waypoint_manager.py가 nav2_simple_commander.BasicNavigator(자체
노드+스핀 스레드를 갖고 블로킹 API를 제공)를 감싸는 것과 같은 이유 —
patrol_task.py의 _execute()는 rclpy ActionServer가 goal마다 띄우는 실행 스레드
안에서 동작하는데, 그 스레드가 속한 노드(PatrolTask)의 executor는 이미 그
_execute() 자신을 돌리느라 바쁘다. 같은 노드/같은 executor로 DeliverMedication
액션 클라이언트를 만들면 응답(goal accept, result)을 받을 콜백을 스핀할 스레드가
없어 그대로 멈춘다(자기 자신을 기다리는 déadlock). 그래서 이 클라이언트는 자기
전용 rclpy Node + 백그라운드 스핀 스레드를 따로 들고, 그 위에서만 오가는
콜백을 threading.Event로 건네받아 블로킹 메서드 하나로 노출한다.

빌드/실행 미검증 — DeliverMedication 액션 서버(medication_task.py)가 아직 실제
하드웨어 없이 만들어졌으므로, 이 클라이언트도 실 로봇에서 한 번도 안 돌려봤다.
"""
import threading

import rclpy
from rclpy.action import ActionClient
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node

from careguard_msgs.action import DeliverMedication

# medication_task가 뜰 때까지 기다리는 최대 시간. patrol_task.py의
# PATROL_TASK_READY_TIMEOUT_SEC과 같은 이유로 넉넉하게 잡는다 — 다만 medication_task는
# video_recorder/audio_player처럼 map_id에 안 묶이는 "한 번 뜨면 계속 재사용" 노드라
# 보통은 patrol_task 실행 시점엔 이미 떠 있을 것으로 기대한다(downlink_node.py가
# nav_prepare/patrol_start 시점에 같이 기동을 보장해야 함 — 아직 미구현, 아래 TODO).
SERVER_WAIT_TIMEOUT_SEC = 10.0

# person_timeout_sec를 호출부가 안 채웠을 때(<= 0) 대신 쓰는 기본값. medication_task.py
# 쪽 DEFAULT_PERSON_TIMEOUT_SEC과 반드시 같은 값이어야 하는 건 아니다 — 여기 값은
# "얼마나 기다렸다가 포기할지"를 호출부 기준으로 판단하는 용도라, 액션 goal에 항상
# 명시적으로 채워 보내 서버 쪽 기본값에 의존하지 않게 한다.
DEFAULT_PERSON_TIMEOUT_SEC = 60.0


class MedicationClient:
    def __init__(self):
        self._node = Node('medication_client')
        self._action_client = ActionClient(self._node, DeliverMedication, 'deliver_medication')
        self._executor = SingleThreadedExecutor()
        self._executor.add_node(self._node)
        self._thread = threading.Thread(target=self._executor.spin, daemon=True)
        self._thread.start()

    def deliver(self, slot_no: int, schedule_id: int = 0,
                person_timeout_sec: float = DEFAULT_PERSON_TIMEOUT_SEC,
                skip_person_check: bool = False, cancel_check=None) -> str:
        """DeliverMedication 액션을 goal로 보내고 끝날 때까지 블로킹으로 기다린다.
        반환값은 액션 result의 status 문자열("delivered"/"timeout"/"jammed"/
        "hw_error"/"cancelled") — 서버 통신 자체가 안 되면(액션 서버 못 찾음 등)
        "hw_error"로 대체한다(호출부가 매번 예외 처리를 따로 안 해도 되게).

        cancel_check: patrol_task.py의 goal_handle.is_cancel_requested와 같은
        콜백. WaypointManager._go_through의 cancel_check 패턴을 그대로 따른다 —
        다만 지금은 폴링 주기(0.5초)로만 확인한다(진짜 취소 전파는 TODO, 아래 참고).
        """
        if not self._action_client.wait_for_server(timeout_sec=SERVER_WAIT_TIMEOUT_SEC):
            self._node.get_logger().error('DeliverMedication 액션 서버를 찾을 수 없습니다')
            return 'hw_error'

        goal = DeliverMedication.Goal()
        goal.slot_no = int(slot_no)
        goal.schedule_id = int(schedule_id)
        goal.person_timeout_sec = float(person_timeout_sec)
        goal.skip_person_check = bool(skip_person_check)

        goal_response_event = threading.Event()
        result_event = threading.Event()
        state = {'goal_handle': None, 'status': 'hw_error'}

        def _on_goal_response(future):
            goal_handle = future.result()
            state['goal_handle'] = goal_handle
            if not goal_handle.accepted:
                self._node.get_logger().error('DeliverMedication goal이 거부됐습니다')
            goal_response_event.set()

        def _on_result(future):
            result = future.result().result
            state['status'] = result.status
            result_event.set()

        send_future = self._action_client.send_goal_async(goal)
        send_future.add_done_callback(_on_goal_response)
        goal_response_event.wait(timeout=SERVER_WAIT_TIMEOUT_SEC)

        goal_handle = state['goal_handle']
        if goal_handle is None or not goal_handle.accepted:
            return 'hw_error'

        result_future = goal_handle.get_result_async()
        result_future.add_done_callback(_on_result)

        # TODO(향후): cancel_check가 True를 반환하면 goal_handle.cancel_goal_async()를
        # 부르는 실제 취소 전파 — 지금은 person_timeout_sec + 여유(closing 단계 포함)로만
        # 기다린다. patrol_task.py가 지금 이 클라이언트를 쓰는 지점(마지막 웨이포인트
        # 도착 후, 충전소 복귀 직전)은 순찰 취소 시 어차피 return_to_charging을 안
        # 타므로 당장 급하지 않다고 보고 최소 구현으로 남긴다.
        result_event.wait(timeout=person_timeout_sec + 30.0)
        return state['status']

    def destroy(self):
        self._executor.shutdown()
        self._action_client.destroy()
        self._node.destroy_node()
