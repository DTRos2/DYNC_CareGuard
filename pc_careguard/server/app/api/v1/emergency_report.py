"""app/api/v1/emergency_report.py

이벤트 상세 모달 "119 모의 신고" 버튼 — call.py와 같은 패턴(로봇 후면 키오스크에
상태를 알리는 REST + 세션 레지스트리)이지만, 실제 119/구급 연동이 아니라 모의
신고라 로봇에는 아무 신호도 보내지 않는다(robot_ws.send_command 없음). 오직
dashboard_ws.broadcast로 키오스크(deploy/robot_kiosk)에 "보호자가 119 신고중"
상태를 띄우는 용도.
"""
from typing import Dict

from fastapi import APIRouter

from ...schemas import EmergencyReportRequest, EmergencyReportResponse, EmergencyReportStatusResponse
from ...ws import dashboard_ws

router = APIRouter(prefix="/emergency-reports", tags=["emergency-report"])


class EmergencyReportRegistry:
    """robot_code -> reporter_name. call.py::CallSessionRegistry와 같은 이유로
    최소한의 상태만 관리한다 — 중복 start 방지, GET /status로 키오스크 재연결 시
    재조회하는 용도(2026-09-17 추가)."""

    def __init__(self) -> None:
        self._active: Dict[str, str] = {}

    def is_active(self, robot_code: str) -> bool:
        return robot_code in self._active

    def reporter_name(self, robot_code: str) -> str:
        return self._active.get(robot_code, "")

    def mark_started(self, robot_code: str, reporter_name: str) -> None:
        self._active[robot_code] = reporter_name

    def mark_ended(self, robot_code: str) -> None:
        self._active.pop(robot_code, None)


_reports = EmergencyReportRegistry()


@router.post("/start", response_model=EmergencyReportResponse)
async def start_emergency_report(payload: EmergencyReportRequest):
    _reports.mark_started(payload.robot_code, payload.reporter_name)
    await dashboard_ws.broadcast(
        {
            "type": "emergency_report_started",
            "robot_code": payload.robot_code,
            "reporter_name": payload.reporter_name,
        }
    )
    return EmergencyReportResponse(active=True)


@router.post("/end", response_model=EmergencyReportResponse)
async def end_emergency_report(payload: EmergencyReportRequest):
    _reports.mark_ended(payload.robot_code)
    await dashboard_ws.broadcast({"type": "emergency_report_ended", "robot_code": payload.robot_code})
    return EmergencyReportResponse(active=False)


@router.get("/status", response_model=EmergencyReportStatusResponse)
async def get_emergency_report_status(robot_code: str):
    """키오스크가 loadPatrolState/get_call_status와 같은 패턴으로 초기 로드·WS 재연결
    시 호출한다 — emergency_report_started 브로드캐스트를 놓쳤을 때 대비."""
    return EmergencyReportStatusResponse(
        active=_reports.is_active(robot_code), reporter_name=_reports.reporter_name(robot_code)
    )
