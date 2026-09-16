"""careguard_turtlebot_agent/patrol_controller.py

patrol_start/patrol_stop 명령을 받아 웨이포인트 순찰 또는 충전소 단독 복귀
상태 기계를 돌린다 — downlink_node.py에 몰려있던 책임을 분리했다(2026-09-15
리팩터링). LIMO(careguard_mission/patrol_task.py)와 같은 역할이지만 그쪽이
액션 서버 하나로 같이 처리하는 영상 녹화(careguard_perception)는 터틀봇
워크스페이스에 없어서 빠졌고, 음성 안내(careguard_hmi)만 VoiceAnnouncer를
직접 호출하는 방식으로 재현했다(downlink_node.py 모듈 docstring 참고).
웨이포인트를 순서대로 돌다가 하나라도 실패하면 즉시 중단하고(실패해도 조용히
다음으로 넘어가면 "순찰 완료"로 잘못 기록된다), 끝나면(return_to_charging이면)
충전소로 복귀한다. 진행률은 patrol_progress/patrol_done으로 서버에 보고한다
(robot_ws.py가 DB의 patrol_run 행과 대시보드로 그대로 릴레이).
"""
import threading
from typing import Callable, Optional

from .nav2_manager import Nav2Manager
from .voice import VoiceAnnouncer
from .waypoints import fetch_charge_waypoint


class PatrolController:
    def __init__(self, nav2: Nav2Manager, voice: VoiceAnnouncer, send: Callable[[dict], None], logger):
        self._nav2 = nav2
        self._voice = voice
        self._send = send
        self._logger = logger
        self._active = False
        self._cancel_requested = threading.Event()
        self._thread: Optional[threading.Thread] = None

    @property
    def is_active(self) -> bool:
        return self._active

    def start(self, message: dict):
        waypoints = message.get('waypoints') or []
        return_to_charging = bool(message.get('return_to_charging', True))
        if not waypoints and not return_to_charging:
            # "이동"(단일 지점, return_to_charging=False) — P6 "이동" 버튼/SOS 자동
            # 출동이 이 조합을 보낸다. 여전히 스코프 밖(downlink_node.py 모듈
            # docstring 참고).
            self._logger.warn(
                '터틀봇은 아직 단일 지점 "이동"을 지원하지 않습니다 — '
                '웨이포인트 순찰 또는 배터리 자동 복귀(빈 웨이포인트+return_to_charging)만 처리합니다')
            return

        if self._active:
            self._logger.warn('이미 주행 중입니다 — 새 요청을 무시합니다')
            return

        map_id = int(message.get('map_id', 0) or 0)
        if map_id <= 0:
            self._logger.error('patrol_start에 유효한 map_id가 없습니다')
            return

        try:
            charge_x, charge_y, charge_yaw = fetch_charge_waypoint(map_id)
        except Exception as exc:  # noqa: BLE001 - 원인이 다양해(네트워크/DB/웨이포인트 없음 등) 전부 여기서 로그만 남긴다
            self._logger.error(f'충전 웨이포인트 조회 실패: {exc}')
            return

        if not self._nav2.ensure_running(map_id, charge_x, charge_y, charge_yaw):
            self._logger.error('Nav2 준비 실패로 주행을 시작하지 않습니다')
            return

        self._cancel_requested.clear()
        self._active = True
        run_id = int(message.get('run_id', 0) or 0)
        if waypoints:
            self._thread = threading.Thread(
                target=self._run_patrol,
                args=(run_id, waypoints, return_to_charging, charge_x, charge_y, charge_yaw),
                daemon=True)
        else:
            self._thread = threading.Thread(
                target=self._drive_to_charge, args=(charge_x, charge_y, charge_yaw), daemon=True)
        self._thread.start()

    def stop(self, message: dict):
        if not self._active:
            self._logger.warn('취소할 진행 중인 주행이 없습니다')
            return
        self._cancel_requested.set()

    def _drive_to_charge(self, x: float, y: float, yaw: float):
        try:
            if self._cancel_requested.is_set():
                return
            self._voice.speak('충전소로 복귀합니다')
            success = self._nav2.go_to_pose_and_wait(x, y, yaw, self._cancel_requested)
            if self._cancel_requested.is_set():
                self._logger.info('충전소 복귀 주행이 취소됐습니다')
            else:
                self._logger.info(f'충전소 복귀 결과: {"성공" if success else "실패"}')
                if success:
                    self._voice.speak('충전을 시작합니다')
        finally:
            self._active = False

    def _run_patrol(self, run_id: int, waypoints: list, return_to_charging: bool,
                     charge_x: float, charge_y: float, charge_yaw: float):
        try:
            self._voice.speak('주행을 시작합니다')
            waypoints_done = 0
            for wp in waypoints:
                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self._logger.info('순찰이 취소됐습니다')
                    return

                success = self._nav2.go_to_pose_and_wait(
                    float(wp['x']), float(wp['y']), float(wp.get('yaw', 0.0) or 0.0),
                    self._cancel_requested)

                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self._logger.info(f"{wp.get('code', '?')} 이동 중 취소됨")
                    return
                if not success:
                    self._logger.error(f"{wp.get('code', '?')} 이동 실패 — 순찰을 중단합니다")
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'failed'})
                    return

                waypoints_done += 1
                self._send({'type': 'patrol_progress', 'run_id': run_id, 'waypoints_done': waypoints_done})

            if return_to_charging:
                self._voice.speak('충전소로 복귀합니다')
                success = self._nav2.go_to_pose_and_wait(charge_x, charge_y, charge_yaw, self._cancel_requested)
                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self._logger.info('충전소 복귀 중 취소됨')
                    return
                if not success:
                    self._logger.error('충전소 복귀 실패')
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'failed'})
                    return
                self._voice.speak('충전을 시작합니다')

            self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'completed'})
            self._logger.info(f'순찰 완료: run_id={run_id}')
        finally:
            self._active = False
