"""careguard_mission/slam_progress_publisher.py

/map(OccupancyGrid)을 구독해 "탐색 면적"(미탐색이 아닌 셀 수 * 셀 면적)과 이 노드가
뜬 뒤 지난 시간을 1Hz로 /careguard/slam_status에 JSON 문자열로 발행한다.
careguard_agent/uplink_node.py가 이미 그 토픽을 구독해 서버로 relay하고 있다
(careguard_agent만 서버/WS를 직접 만지는 "유일한 외부 통신구" 원칙을 지키기 위해,
patrol_task.py의 /careguard/patrol_status 발행과 같은 패턴으로 이 노드는 서버를
직접 호출하지 않는다).

careguard_bringup/launch/cartographer_launch.py에 이 노드를 같이 띄워서
cartographer_node와 생명주기를 맞춘다 — downlink_node._stop_slam()이 그 launch
프로세스 그룹 전체를 SIGINT로 죽이므로 SLAM이 멈추면 이 노드도 같이 죽고,
"매핑 재시작" 때 경과시간·면적이 자동으로 0부터 다시 시작된다(대시보드
SlamMappingCard가 재시작 시 자기 값을 0으로 리셋하는 것과 동일한 기대에 맞춘다).

빌드/실행 미검증.
"""
import json
import time

import rclpy
from nav_msgs.msg import OccupancyGrid
from rclpy.node import Node
from std_msgs.msg import String

PUBLISH_PERIOD_SEC = 1.0


class SlamProgressPublisher(Node):
    def __init__(self):
        super().__init__('slam_progress_publisher')
        self._status_pub = self.create_publisher(String, '/careguard/slam_status', 10)
        self._start_time = time.monotonic()
        self._area_m2 = 0.0
        self.create_subscription(OccupancyGrid, '/map', self._on_map, 1)
        self.create_timer(PUBLISH_PERIOD_SEC, self._on_tick)

    def _on_map(self, msg: OccupancyGrid):
        # -1(미탐색)이 아닌 셀 = 라이다가 이미 지나간 자리(빈 공간이든 장애물이든)를
        # "탐색된 영역"으로 센다. free(0)인 셀만 세면 벽·장애물로 막힌 안쪽 면적이
        # 누락돼 실제 로봇이 돈 범위보다 화면 수치가 작게 나온다.
        cell_area = msg.info.resolution * msg.info.resolution
        known_cells = len(msg.data) - msg.data.count(-1)
        self._area_m2 = known_cells * cell_area

    def _on_tick(self):
        elapsed_sec = int(time.monotonic() - self._start_time)
        msg = String()
        msg.data = json.dumps({
            'type': 'slam_progress',
            'elapsed_sec': elapsed_sec,
            'area_m2': round(self._area_m2, 1),
        })
        self._status_pub.publish(msg)


def main(args=None):
    rclpy.init(args=args)
    node = SlamProgressPublisher()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        # SIGINT를 받으면 rclpy가 내부적으로 먼저 shutdown()을 호출해버릴 수 있다 —
        # 이 노드는 downlink_node._stop_slam()이 프로세스 그룹 전체(cartographer_node·
        # rviz2·이 노드)에 os.killpg(SIGINT)를 보낼 때 같이 죽는데, 그 경로에서
        # 아래 shutdown()을 무조건 또 부르면 "rcl_shutdown already called"로 exit
        # code 1로 죽었다(실제로 겪음 — cartographer_node/rviz2는 C++ 노드라 이
        # rclpy 특유의 경쟁 상태가 없어 멀쩡히 끝났는데 이 노드만 죽었다). 그러면
        # downlink_node가 그룹 전체 종료를 5초 기다리다 확인이 꼬여 "매핑 취소"가
        # 안 끝나는 것처럼 보였다.
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
