# 대시보드/서버로 올라가는 로봇 상태 요약. careguard_agent/uplink_node.py가
# 주기적으로 발행·전송한다.
float64 x               # 맵 기준 x (m)
float64 y               # 맵 기준 y (m)
float64 yaw             # 맵 기준 yaw (rad)
string status            # "idle" / "patrolling" / "mapping" / "charging" / "error"
float32 battery_pct      # 0.0 ~ 100.0
