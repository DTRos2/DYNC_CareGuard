# 순찰 1회차에 방문할 지점 하나. 서버 app/api/v1/patrol.py::start_patrol_run()이
# patrol_start WS 메시지에 실어 보내는 좌표를 그대로 옮긴 것 — 로봇이 route_id로
# 다시 조회하지 않아도 되도록 좌표를 직접 담는다.
string code
float64 x
float64 y
float64 yaw
