"""app/api/v1/call.py

로봇 상세 모달의 "통화 시작/종료" REST — patrol.py::cancel_patrol_run과 같은 패턴으로,
대시보드가 REST를 호출하면 서버가 그 robot_code의 기존 JSON 다운링크로 call_start/
call_end를 실어 보낸다. 오디오 바이트 자체(마이크 프레임)는 이 요청과 완전히 분리된
app/ws/audio_relay.py의 별도 WebSocket 4개로 흐른다 — 이 모듈은 "통화를 시작/끝내라"는
신호와, 지금 이 로봇이 통화 중인지 상태만 다룬다.
"""
from typing import Dict

from fastapi import APIRouter

from ...schemas import CallRequest, CallResponse
from ...ws import dashboard_ws, robot_ws

router = APIRouter(prefix="/calls", tags=["call"])


class CallSessionRegistry:
    """robot_code -> 통화 진행 여부. 실제 오디오 소켓 개수·상태는
    audio_relay.py::AudioRoom이 이미 갖고 있으므로, 여기서는 "지금 이 로봇과 통화
    중인가"라는 최소한의 플래그만 관리한다 — 중복 call_start 방지, P6/P1 등 다른
    화면에 통화 중 배지를 보여줄 때 참조하는 용도."""

    def __init__(self) -> None:
        self._active: Dict[str, bool] = {}

    def is_active(self, robot_code: str) -> bool:
        return self._active.get(robot_code, False)

    def mark_started(self, robot_code: str) -> None:
        self._active[robot_code] = True

    def mark_ended(self, robot_code: str) -> None:
        self._active.pop(robot_code, None)


_sessions = CallSessionRegistry()


@router.post("/start", response_model=CallResponse)
async def start_call(payload: CallRequest):
    """로봇에 call_start를 보낸다 — LIMO의 downlink_node가 이걸 받아 오디오
    캡처/재생 노드에 ROS 토픽으로 릴레이한다(audio_capture_node.py/
    audio_downlink_player.py 참고). 로봇이 오프라인이면 robot_connected=False로
    응답하고, 대시보드가 이 상태를 보고 "통화 시작" 버튼을 되돌려야 한다.
    """
    connected = await robot_ws.send_command(payload.robot_code, {"type": "call_start"})
    if connected:
        _sessions.mark_started(payload.robot_code)
    await dashboard_ws.broadcast(
        {"type": "call_started", "robot_code": payload.robot_code, "robot_connected": connected}
    )
    return CallResponse(robot_connected=connected)


@router.post("/end", response_model=CallResponse)
async def end_call(payload: CallRequest):
    """call_end는 로봇 연결 여부와 무관하게 항상 세션 상태를 정리한다 —
    cancel_patrol_run과 같은 이유(연결이 이미 끊겼다고 서버 쪽 상태까지 stuck으로
    남으면 안 된다)."""
    connected = await robot_ws.send_command(payload.robot_code, {"type": "call_end"})
    _sessions.mark_ended(payload.robot_code)
    await dashboard_ws.broadcast({"type": "call_ended", "robot_code": payload.robot_code})
    return CallResponse(robot_connected=connected)
