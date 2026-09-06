"""careguard_mission/patrol_task.py

careguard_msgs/action/Patrol 액션 서버. careguard_agent/downlink_node.py가
서버의 "patrol_start" 명령을 받아 이 액션을 호출한다(goal에 이미 방문할
웨이포인트 좌표가 실려 있다 — route_id를 로봇이 다시 조회할 필요가 없다).
좌표 순서대로 Nav2로 이동시키고, 끝나면 충전소로 복귀한다.

진행률은 액션 feedback과 /careguard/patrol_status 토픽(JSON 문자열) 양쪽에
발행한다 — careguard_agent/uplink_node.py가 그 토픽을 구독해 서버로 relay한다
(careguard_agent만 서버/WS를 직접 만지도록 하는 "유일한 외부 통신구" 원칙을
지키기 위해, 이 노드는 서버를 직접 호출하지 않는다).

빌드/실행 미검증.
"""
import json
import os

import rclpy
from rclpy.action import ActionServer
from rclpy.node import Node
from std_msgs.msg import String

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

        self._action_server = ActionServer(self, Patrol, 'patrol', self._execute)

    def _publish_status(self, **fields):
        msg = String()
        msg.data = json.dumps(fields)
        self._status_pub.publish(msg)

    def _execute(self, goal_handle):
        run_id = goal_handle.request.run_id
        waypoints = goal_handle.request.waypoints
        waypoints_total = len(waypoints)
        self.get_logger().info(f'순찰 시작: run_id={run_id} 지점 {waypoints_total}개')

        for i, wp in enumerate(waypoints, start=1):
            self._waypoint_manager.navigate_poses([wp])
            goal_handle.publish_feedback(Patrol.Feedback(waypoints_done=i, waypoints_total=waypoints_total))
            self._publish_status(type='patrol_progress', run_id=run_id, waypoints_done=i)

        return_to_charging(self._waypoint_manager)

        goal_handle.succeed()
        self._publish_status(type='patrol_done', run_id=run_id, status='completed')

        result = Patrol.Result()
        result.status = 'completed'
        result.waypoints_done = waypoints_total
        return result


def main(args=None):
    rclpy.init(args=args)
    node = PatrolTask()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
