"""careguard_nav/initial_pose_setter.py

기동 시 충전 스테이션 웨이포인트를 /initialpose로 자동 발행해 AMCL 초기 위치를
맞춘다. 대상 맵은 환경변수 CAREGUARD_MAP_ID(서버 map.id)로 받는다 — 로봇 목록/
현재 배정 맵 조회 API가 이번 스코프 밖이라, careguard_bringup 런치에서 이
값을 넘겨주는 것을 전제로 한다.

CAREGUARD_INIT_X/Y/YAW가 모두 있으면 DB 조회(충전 스테이션 가정) 대신 그 값을
그대로 쓴다 — P6 "위치 보정"(대시보드 지도 클릭+드래그)이 로봇이 실제로 충전
스테이션이 아닌 곳에 있을 때 AMCL 초기 위치를 직접 지정하는 경로
(careguard_agent/downlink_node.py::_set_initial_pose)가 이 값을 넘겨준다.

빌드/실행 검증됨(2026-09-08) — 단, 최초 구현엔 실 로봇에서 실제로 겪은 버그가
있었다: 퍼블리셔를 만들자마자(디스커버리가 AMCL 구독자와 아직 안 맺어진 채로)
바로 publish()해서, 메시지가 구독자 없이 그냥 버려지는 경우가 있었다(AMCL이
"please set initial pose" 경고를 몇 분째 반복하는데 /amcl_pose는 끝내 안 나옴 —
확률적으로 성공하기도 해서 "가끔 되는" 것처럼 보였다). publish_when_ready()가
구독자가 실제로 잡힐 때까지 기다렸다가 발행하도록 고쳤다.
"""
import math
import os
import time

import rclpy
from geometry_msgs.msg import PoseWithCovarianceStamped
from rclpy.node import Node

from .waypoint_manager import WaypointManager

MAP_ID_ENV = 'CAREGUARD_MAP_ID'
MANUAL_X_ENV = 'CAREGUARD_INIT_X'
MANUAL_Y_ENV = 'CAREGUARD_INIT_Y'
MANUAL_YAW_ENV = 'CAREGUARD_INIT_YAW'

# AMCL이 /initialpose 구독을 맺을 때까지 기다리는 최대 시간.
SUBSCRIBER_WAIT_TIMEOUT_SEC = 5.0


class InitialPoseSetter(Node):
    def __init__(self):
        super().__init__('initial_pose_setter')
        self._pub = self.create_publisher(PoseWithCovarianceStamped, '/initialpose', 10)
        # 여기선 값만 계산해서 들고 있고, 실제 발행은 publish_when_ready()가
        # 구독자를 확인한 뒤에 한다 — 생성자에서 곧바로 publish()하면 디스커버리가
        # 아직 안 끝나 메시지가 버려질 수 있다(위 docstring 참고).
        self._pose = None  # (x, y, yaw, source) 또는 조기 반환 사유로 None

        manual_x = os.environ.get(MANUAL_X_ENV)
        manual_y = os.environ.get(MANUAL_Y_ENV)
        if manual_x is not None and manual_y is not None:
            x = float(manual_x)
            y = float(manual_y)
            yaw = float(os.environ.get(MANUAL_YAW_ENV, '0.0') or 0.0)
            source = '대시보드 위치 보정'
        else:
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
            x, y, yaw = charge.pos_x, charge.pos_y, charge.yaw
            source = f'충전 스테이션({charge.code})'

        self._pose = (x, y, yaw, source)

    def publish_when_ready(self, timeout_sec: float = SUBSCRIBER_WAIT_TIMEOUT_SEC) -> bool:
        """AMCL의 /initialpose 구독이 실제로 맺어질 때까지 기다린 뒤 발행한다.
        타임아웃 안에 구독자가 안 잡혀도(예: amcl이 아직 안 떴다) 마지막엔 밑져야
        본전으로 한 번 발행은 시도한다.

        get_subscription_count()가 0보다 커진 시점 == DDS가 서로를 "발견"한
        시점이지, 실제로 신뢰성 있게 주고받을 수 있는 채널이 완전히 준비된
        시점은 아니다 — 그 직후 곧바로 보낸 메시지 몇 개가 씹히는 경우가 실제로
        있었다(2026-09-08 실 로봇에서 재현: 예전엔 여기서 3번(약 0.6초)만
        발행했는데, 그 3번이 전부 씹혀서 initial_pose_setter는 "성공"으로 로그를
        남겼지만 AMCL은 끝내 /amcl_pose를 한 번도 발행하지 않았다 — 그 여파로
        global_costmap이 map 좌표계를 못 얻어 activating에서 멈추고, 그 뒤에
        걸린 bt_navigator도 계속 inactive라 순찰 명령이 전부 조용히 실패했다.
        같은 상황에서 이 스크립트를 다시 한 번 실행하면(이번엔 채널이 이미 데워져
        있어서) 바로 성공하는 것으로 확인됨 — 재시도 횟수·기간을 넉넉히 늘려
        같은 프로세스 안에서 이 워밍업 구간을 확실히 넘기게 한다.
        """
        if self._pose is None:
            return False

        deadline = time.monotonic() + timeout_sec
        while self._pub.get_subscription_count() == 0 and time.monotonic() < deadline:
            rclpy.spin_once(self, timeout_sec=0.1)
        if self._pub.get_subscription_count() == 0:
            self.get_logger().warn('AMCL의 /initialpose 구독을 못 찾았습니다 — 그래도 발행은 시도합니다')

        x, y, yaw, source = self._pose
        msg = PoseWithCovarianceStamped()
        msg.header.frame_id = 'map'
        msg.pose.pose.position.x = x
        msg.pose.pose.position.y = y
        msg.pose.pose.orientation.z = math.sin(yaw / 2.0)
        msg.pose.pose.orientation.w = math.cos(yaw / 2.0)
        # 3번/0.6초 -> 15번/약 3초로 늘렸다 — downlink_node.py 쪽 subprocess
        # timeout도 같이 늘려야 한다(_ensure_navigation_running/_set_initial_pose).
        for _ in range(15):
            msg.header.stamp = self.get_clock().now().to_msg()
            self._pub.publish(msg)
            rclpy.spin_once(self, timeout_sec=0.2)
        self.get_logger().info(f'초기 위치를 {source}(으)로 설정했습니다')
        return True


def main(args=None):
    rclpy.init(args=args)
    node = InitialPoseSetter()
    node.publish_when_ready()
    node.destroy_node()
    rclpy.shutdown()


if __name__ == '__main__':
    main()
