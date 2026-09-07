from fastapi import APIRouter

from ...schemas import TeleopCommand
from ...ws import robot_ws

# P5 방향키 패널 -> 실제 로봇 cmd_vel. DB에 남기지 않는 순간 명령이라 map.py/patrol.py의
# slam-jobs/patrol-runs와 달리 별도 파일로 둔다.
router = APIRouter(prefix="/teleop", tags=["teleop"])


@router.post("/{robot_code}")
async def send_teleop(robot_code: str, payload: TeleopCommand):
    """방향키로 계산한 선속도/각속도를 로봇에 전달한다. robot_ws가 연결이 없으면 조용히 False."""
    connected = await robot_ws.send_command(
        robot_code, {"type": "cmd_vel", "linear": payload.linear, "angular": payload.angular}
    )
    return {"robot_connected": connected}
