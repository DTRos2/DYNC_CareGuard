"""careguard_nav/waypoint_manager.py

서버에서 웨이포인트를 내려받아 캐시하고(GET /waypoints?map_id=), Nav2로
순서대로 이동시키는 역할. careguard_mission/patrol_task.py가 이 클래스를
가져다 쓴다.

이동 경로는 두 가지 출처를 지원한다:
  - navigate_sequence(codes): fetch()로 미리 내려받은 캐시에서 code로 찾는다
    (initial_pose_setter.py의 충전 스테이션 조회, dock_return.py의 복귀 등).
  - navigate_poses(waypoints): 서버가 patrol_start WS 메시지로 이미 좌표를
    실어 보낸 경우 캐시 없이 바로 사용한다(patrol_task.py의 순찰 경로).

빌드/실행 미검증 — nav2_simple_commander.BasicNavigator API는 ROS2 배포판마다
세부가 조금씩 다르니 실 로봇에서 한 번 맞춰봐야 한다.
"""
import math
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence

import requests
from geometry_msgs.msg import PoseStamped
from nav2_simple_commander.robot_navigator import BasicNavigator

from . import config


@dataclass
class Waypoint:
    code: str
    name: Optional[str]
    kind: str  # "patrol" / "charge"
    pos_x: float
    pos_y: float
    yaw: float


class WaypointManager:
    def __init__(self, navigator: Optional[BasicNavigator] = None):
        self._navigator = navigator or BasicNavigator()
        self._waypoints: Dict[str, Waypoint] = {}

    def fetch(self, map_id: int) -> None:
        """GET /waypoints?map_id= 로 최신 웨이포인트를 내려받아 캐시를 교체한다."""
        response = requests.get(
            f'{config.SERVER_HTTP_URL}/waypoints', params={'map_id': map_id}, timeout=10
        )
        response.raise_for_status()
        self._waypoints = {
            row['code']: Waypoint(
                code=row['code'],
                name=row.get('name'),
                kind=row['kind'],
                pos_x=row['pos_x'],
                pos_y=row['pos_y'],
                yaw=row['yaw'],
            )
            for row in response.json()
        }

    def charging_waypoint(self) -> Optional[Waypoint]:
        return next((w for w in self._waypoints.values() if w.kind == 'charge'), None)

    def _to_pose_xyz(self, x: float, y: float, yaw: float) -> PoseStamped:
        pose = PoseStamped()
        pose.header.frame_id = 'map'
        pose.header.stamp = self._navigator.get_clock().now().to_msg()
        pose.pose.position.x = x
        pose.pose.position.y = y
        pose.pose.orientation.z = math.sin(yaw / 2.0)
        pose.pose.orientation.w = math.cos(yaw / 2.0)
        return pose

    def navigate_sequence(self, codes: Sequence[str]):
        """캐시(fetch)에 있는 code 순서대로 이동한다."""
        poses = [self._to_pose_xyz(w.pos_x, w.pos_y, w.yaw) for w in (self._waypoints.get(c) for c in codes) if w]
        return self._go_through(poses)

    def navigate_poses(self, waypoints: Sequence) -> None:
        """이미 좌표를 알고 있는 웨이포인트 목록을 캐시와 무관하게 그대로 이동한다.

        waypoints의 각 원소는 x/y/yaw 속성을 갖는 객체(예: careguard_msgs/msg/Waypoint)면 된다.
        """
        poses = [self._to_pose_xyz(w.x, w.y, w.yaw) for w in waypoints]
        return self._go_through(poses)

    def _go_through(self, poses: List[PoseStamped]):
        if not poses:
            return None
        if len(poses) == 1:
            # 단일 목적지는 goToPose로 보낸다 — goThroughPoses는 "여러 지점을
            # 매끄럽게 지나가는" 용도라 goal_checker가 goToPose만큼 엄격하게
            # 정지시키지 않고, 도착 지점을 지나쳐버리는 걸 실 로봇 테스트로
            # 확인했다("이동"으로 웨이포인트를 클릭하면 그 지점을 지나쳐 더 감).
            # 이 코드베이스의 모든 호출부(patrol_task._execute의 웨이포인트별
            # 루프, dock_return)가 실제로는 항상 좌표를 하나씩만 넘기므로, 이
            # 분기가 사실상 전부를 처리하고 여러 개짜리 goThroughPoses 경로는
            # 나중에 진짜 다중 웨이포인트 일괄 이동이 필요해질 때를 위해 남겨둔다.
            self._navigator.goToPose(poses[0])
        else:
            self._navigator.goThroughPoses(poses)
        while not self._navigator.isTaskComplete():
            pass
        return self._navigator.getResult()
