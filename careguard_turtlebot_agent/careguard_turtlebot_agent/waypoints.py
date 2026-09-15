"""careguard_turtlebot_agent/waypoints.py

서버 REST API(/waypoints)에서 충전 스테이션 좌표를 조회한다 —
downlink_node.py에 몰려있던 책임을 분리했다(2026-09-15 리팩터링).
careguard_nav/waypoint_manager.py::charging_waypoint()와 같은 조회를, 그
패키지를 가져다 쓰지 않고 직접 한다(터틀봇 워크스페이스에 careguard_nav가
없다 — downlink_node.py 모듈 docstring 참고).
"""
from typing import Tuple

import requests

from . import ws_config


def fetch_charge_waypoint(map_id: int) -> Tuple[float, float, float]:
    resp = requests.get(
        f'{ws_config.SERVER_HTTP_URL}/waypoints', params={'map_id': map_id}, timeout=10)
    resp.raise_for_status()
    charge = next((w for w in resp.json() if w.get('kind') == 'charge'), None)
    if charge is None:
        raise RuntimeError(f'map_id={map_id}에 충전 스테이션 웨이포인트가 없습니다')
    return float(charge['pos_x']), float(charge['pos_y']), float(charge['yaw'])
