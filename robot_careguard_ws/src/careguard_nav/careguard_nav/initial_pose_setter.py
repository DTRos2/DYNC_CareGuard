"""careguard_nav/initial_pose_setter.py

기동 시 충전 스테이션 웨이포인트를 /initialpose로 자동 발행해 AMCL 초기 위치를
맞춘다. 대상 맵은 환경변수 CAREGUARD_MAP_ID(서버 map.id)로 받는다 — 로봇 목록/
현재 배정 맵 조회 API가 이번 스코프 밖이라, careguard_bringup 런치에서 이
값을 넘겨주는 것을 전제로 한다.

빌드/실행 미검증.
"""
import math
import os

import rclpy
from geometry_msgs.msg import PoseWithCovarianceStamped
from rclpy.node import Node

from .waypoint_manager import WaypointManager

MAP_ID_ENV = 'CAREGUARD_MAP_ID'


class InitialPoseSetter(Node):
    def __init__(self):
        super().__init__('initial_pose_setter')
        self._pub = self.create_publisher(PoseWithCovarianceStamped, '/initialpose', 10)

        map_id = int(os.environ.get(MAP_ID_ENV, '0') or 0)
        if map_id <= 0:
            self.get_logger().warn(f'{MAP_ID_ENV} 환경변수가 없어 초기 위치를 설정하지 않습니다')
            return

        manager = WaypointManager()
        manager.fetch(map_id)
        charge = manager.charging_waypoint()
        if charge is None:
            self.get_logger().warn('충전 스테이션 웨이포인트가 없습니다')
            return

        msg = PoseWithCovarianceStamped()
        msg.header.frame_id = 'map'
        msg.header.stamp = self.get_clock().now().to_msg()
        msg.pose.pose.position.x = charge.pos_x
        msg.pose.pose.position.y = charge.pos_y
        msg.pose.pose.orientation.z = math.sin(charge.yaw / 2.0)
        msg.pose.pose.orientation.w = math.cos(charge.yaw / 2.0)
        self._pub.publish(msg)
        self.get_logger().info(f'초기 위치를 충전 스테이션({charge.code})으로 설정했습니다')


def main(args=None):
    rclpy.init(args=args)
    node = InitialPoseSetter()
    rclpy.spin_once(node, timeout_sec=1.0)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == '__main__':
    main()
