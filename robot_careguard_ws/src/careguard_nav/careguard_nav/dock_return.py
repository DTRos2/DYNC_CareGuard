"""careguard_nav/dock_return.py

순찰/매핑 종료 후 충전 스테이션으로 복귀시킨다. careguard_mission/patrol_task.py가
순찰 마지막 단계에서 호출한다.

빌드/실행 미검증.
"""
from typing import Optional

from .waypoint_manager import WaypointManager


def return_to_charging(manager: WaypointManager) -> Optional[object]:
    charge = manager.charging_waypoint()
    if charge is None:
        return None
    return manager.navigate_sequence([charge.code])
