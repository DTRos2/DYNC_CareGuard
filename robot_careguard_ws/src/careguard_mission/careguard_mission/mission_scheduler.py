"""careguard_mission/mission_scheduler.py

오프라인(서버 단절) fallback 자리만 남겨둔다 — 실제 순찰 자동 트리거는 서버
pc_careguard/server/app/services/scheduler.py가 담당한다(patrol_schedule을
서버가 직접 확인해 patrol.start_patrol_run()을 호출). 이 노드는 로봇이 서버와
끊긴 동안에도 마지막으로 동기화해둔 스케줄로 순찰을 계속하기 위한 자리이며,
이번 스코프에서는 최소 stub만 둔다(체크리스트 ⑦ 범위 밖).

TODO(향후): 서버에서 내려받은 patrol_schedule을 로컬에 캐시하고, 서버 연결이
끊긴 상태가 일정 시간 이상 지속되면 이 스케줄러가 대신 patrol_task 액션을
호출하도록 구현한다.
"""
import rclpy
from rclpy.node import Node


class MissionScheduler(Node):
    def __init__(self):
        super().__init__('mission_scheduler')
        self.get_logger().info(
            'mission_scheduler: 오프라인 fallback 미구현 — 서버 services/scheduler.py가 순찰을 트리거합니다'
        )


def main(args=None):
    rclpy.init(args=args)
    node = MissionScheduler()
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
