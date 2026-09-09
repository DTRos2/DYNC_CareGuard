"""careguard_mission/patrol_task.py

careguard_msgs/action/Patrol 액션 서버. careguard_agent/downlink_node.py가
서버의 "patrol_start" 명령을 받아 이 액션을 호출한다(goal에 이미 방문할
웨이포인트 좌표가 실려 있다 — route_id를 로봇이 다시 조회할 필요가 없다).
좌표 순서대로 Nav2로 이동시키고, 끝나면 충전소로 복귀한다.

진행률은 액션 feedback과 /careguard/patrol_status 토픽(JSON 문자열) 양쪽에
발행한다 — careguard_agent/uplink_node.py가 그 토픽을 구독해 서버로 relay한다
(careguard_agent만 서버/WS를 직접 만지도록 하는 "유일한 외부 통신구" 원칙을
지키기 위해, 이 노드는 서버를 직접 호출하지 않는다).

이 노드는 Nav2가 이미 떠 있다고 가정한다(teleop_launch.py처럼 자동으로
띄우지 않음) — 순찰 전에 사전에 별도로
`ros2 launch careguard_bringup navigation_diff_launch.py map:=<등록된 맵 yaml 경로>`
를 실행해 amcl·costmap·bt_navigator 등을 먼저 올려둬야 한다.

빌드/실행 미검증.
"""
import json
import os

import rclpy
from rclpy.action import ActionServer, GoalResponse
from rclpy.node import Node
from std_msgs.msg import String

from nav2_simple_commander.robot_navigator import TaskResult

from careguard_msgs.action import Patrol
from careguard_nav.dock_return import return_to_charging
from careguard_nav.waypoint_manager import WaypointManager


class PatrolTask(Node):
    def __init__(self):
        super().__init__('patrol_task')
        self._status_pub = self.create_publisher(String, '/careguard/patrol_status', 10)
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
            self, Patrol, 'patrol', self._execute, goal_callback=self._handle_goal_request)

    def _handle_goal_request(self, goal_request):
        if self._executing:
            self.get_logger().warn('이미 순찰/이동 중이라 새 요청을 거부합니다 — 끝난 뒤 다시 시도하세요')
            return GoalResponse.REJECT
        return GoalResponse.ACCEPT

    def _publish_status(self, **fields):
        msg = String()
        msg.data = json.dumps(fields)
        self._status_pub.publish(msg)

    def _execute(self, goal_handle):
        self._executing = True
        try:
            run_id = goal_handle.request.run_id
            waypoints = goal_handle.request.waypoints
            waypoints_total = len(waypoints)
            self.get_logger().info(f'순찰 시작: run_id={run_id} 지점 {waypoints_total}개')

            for i, wp in enumerate(waypoints, start=1):
                nav_result = self._waypoint_manager.navigate_poses([wp])
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
                    goal_handle.abort()
                    result = Patrol.Result()
                    result.status = 'failed'
                    result.waypoints_done = i - 1
                    return result
                goal_handle.publish_feedback(Patrol.Feedback(waypoints_done=i, waypoints_total=waypoints_total))
                self._publish_status(type='patrol_progress', run_id=run_id, waypoints_done=i)

            # P6 "이동"(단일 웨이포인트 주행, run_id=0)은 도착한 자리에 그대로 머물러야
            # 한다 — 원래 순찰(여러 지점을 돌고 항상 충전소로 복귀)과 이 필드로 구분한다.
            if goal_handle.request.return_to_charging:
                # 위 웨이포인트 루프와 같은 이유로 결과를 확인한다 — 예전엔 이 반환값을
                # 그냥 버려서, 복귀 이동이 Nav2에서 조용히 실패(aborted)해도 로그 한 줄
                # 안 남고 goal_handle.succeed()+'completed'로 끝났다("wp3에서 충전소로
                # 복귀를 안 함" 리포트, 2026-09-08). charge 웨이포인트 자체가 없는
                # 경우(None)와 실제 이동 실패(FAILED/CANCELED)를 구분해서 로그를 남긴다.
                charge_result = return_to_charging(self._waypoint_manager)
                if charge_result is None:
                    self.get_logger().warn('충전 스테이션 웨이포인트가 없어 복귀를 건너뜁니다')
                elif charge_result != TaskResult.SUCCEEDED:
                    self.get_logger().error(f'충전 스테이션 복귀 실패(result={charge_result})')
                    self._publish_status(type='patrol_done', run_id=run_id, status='failed')
                    goal_handle.abort()
                    result = Patrol.Result()
                    result.status = 'failed'
                    result.waypoints_done = waypoints_total
                    return result

            goal_handle.succeed()
            self._publish_status(type='patrol_done', run_id=run_id, status='completed')

            result = Patrol.Result()
            result.status = 'completed'
            result.waypoints_done = waypoints_total
            return result
        finally:
            self._executing = False


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
