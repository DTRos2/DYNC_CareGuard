"""careguard_agent/uplink_node.py

로봇 -> 서버 방향 업링크. /amcl_pose(로봇 위치), /map(SLAM 중 실시간 지도), 내부
상태 토픽(/careguard/patrol_status, /careguard/slam_status — 각각 careguard_mission의
patrol_task.py, SLAM 진행률 퍼블리셔가 JSON 문자열로 발행)을 구독해 서버의
ws://.../ws/robot?robot_code=... 로 그대로 relay한다.

/map 구독은 2026-09-04 실제 로봇(wego, Cartographer 백엔드)에서 검증됨 — SLAM 중
cartographer_occupancy_grid_node가 1Hz로 발행하는 nav_msgs/OccupancyGrid를 PGM
바이트로 인코딩해 대시보드가 이미 쓰는 PgmImageLoader::loadFromData로 그대로
디코딩되게 한다(server/app/media.py::MapImageUploadResponse와 별개 경로 — 완성
맵 업로드가 아니라 SLAM 도중 미리보기용 실시간 스트림).

나머지(patrol_status/slam_status)는 ROS2(rclpy) + websockets 환경에서 빌드/실행
검증을 하지 못한 상태로 작성됐다 — ws_config.py의 SERVER_WS_URL/ROBOT_CODE 환경변수
규칙과 서버 app/ws/robot_ws.py의 메시지 타입에 맞춰 실 로봇 검증이 아직 필요하다.

position(로봇 위치)은 원래 /amcl_pose 구독이었지만, SLAM(Cartographer) 진행 중엔
amcl이 안 떠 있어 그 토픽에 퍼블리셔가 전혀 없다 — 매핑 중엔 절대 안 오는 값이었다.
지금은 map->base_link TF를 직접 읽어(_on_slam_overlay_tick) SLAM 중에도 위치가
나오게 했고, 같은 타이밍에 /scan을 map 프레임으로 변환해 slam_scan_update로도
보낸다 — 대시보드 P5 뷰에 rviz2처럼 로봇 화살표+라이다 스캔이 실시간으로 겹쳐
보이게 하기 위함이다. /amcl_pose 구독은 매핑이 끝난 뒤 amcl로 위치를 잡는
순찰(P6) 단계를 위해 그대로 남겨둔다.
"""
import asyncio
import base64
import json
import math
import threading
import time

import numpy as np
import rclpy
import tf2_ros
from geometry_msgs.msg import PoseWithCovarianceStamped
from nav_msgs.msg import OccupancyGrid
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import LaserScan
from std_msgs.msg import String

from . import ws_config

RECONNECT_DELAY_SEC = 3.0

# /map을 너무 자주 보내면(cartographer_occupancy_grid_node는 기본 1Hz) 대시보드
# WS·네트워크에 부담이 크다 — 이 간격보다 자주는 보내지 않는다.
MAP_SEND_INTERVAL_SEC = 2.0

# SLAM 중 실시간 로봇 위치·라이다 스캔 오버레이(대시보드 rviz2 스타일 뷰) 갱신 간격.
# /map보다는 자주 갱신해야 "실시간"처럼 보인다.
SLAM_OVERLAY_INTERVAL_SEC = 0.3

# 스캔 포인트를 그대로 다 보내면 메시지가 커진다 — 화면에 뿌릴 용도로는 이 정도로 충분하다.
MAX_SCAN_POINTS = 450


def _yaw_from_quaternion(q) -> float:
    siny_cosp = 2 * (q.w * q.z + q.x * q.y)
    cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z)
    return math.atan2(siny_cosp, cosy_cosp)


def _occupancy_grid_to_pgm_bytes(msg: OccupancyGrid) -> bytes:
    """nav_msgs/OccupancyGrid(-1=미탐색, 0=빈공간, 100=장애물)를 PGM(P5) 바이트로 인코딩한다.

    map_saver(nav2_map_server)와 같은 매핑을 쓴다: 빈공간=255(흰색), 장애물=0(검은색),
    미탐색=128(회색). OccupancyGrid의 0번 행은 원점(왼쪽 아래) 기준이라 이미지 좌표
    (왼쪽 위가 0행)로 세로를 뒤집어야 한다.
    """
    width, height = msg.info.width, msg.info.height
    grid = np.array(msg.data, dtype=np.int16).reshape(height, width)
    pixels = np.where(grid < 0, 128, 255 - (grid.astype(np.int32) * 255 // 100)).astype(np.uint8)
    pixels = np.flipud(pixels)  # OccupancyGrid: 아래->위, 이미지: 위->아래
    header = f'P5\n{width} {height}\n255\n'.encode('ascii')
    return header + pixels.tobytes()


class UplinkNode(Node):
    def __init__(self):
        super().__init__('uplink_node')
        self._loop = asyncio.new_event_loop()
        self._ws = None
        self._connected = False
        self._last_map_sent_at = 0.0

        self.create_subscription(PoseWithCovarianceStamped, '/amcl_pose', self._on_pose, 10)
        self.create_subscription(OccupancyGrid, '/map', self._on_map, 1)
        self.create_subscription(String, '/careguard/patrol_status', self._on_status_topic, 10)
        self.create_subscription(String, '/careguard/slam_status', self._on_status_topic, 10)

        # SLAM(Cartographer) 중에는 /amcl_pose가 없다 — map->base_link TF로 직접 위치를
        # 구해 대시보드의 실시간 rviz2 스타일 뷰(화살표+라이다 스캔)에 넘긴다.
        self._latest_scan = None
        # ydlidar_ros2_driver_node은 /scan을 BEST_EFFORT로 퍼블리시한다 — 기본(RELIABLE)
        # QoS로 구독하면 "incompatible QoS"로 아예 메시지를 못 받는다(실 로봇에서 확인됨).
        self.create_subscription(LaserScan, '/scan', self._on_scan, qos_profile_sensor_data)
        self._tf_buffer = tf2_ros.Buffer()
        self._tf_listener = tf2_ros.TransformListener(self._tf_buffer, self)
        self.create_timer(SLAM_OVERLAY_INTERVAL_SEC, self._on_slam_overlay_tick)

        self._thread = threading.Thread(target=self._run_ws_loop, daemon=True)
        self._thread.start()

    # ---------- ROS2 콜백: 들어오는 대로 asyncio 루프에 넘겨서 전송한다 ----------

    def _on_pose(self, msg: PoseWithCovarianceStamped):
        yaw = _yaw_from_quaternion(msg.pose.pose.orientation)
        self._send({
            'type': 'position',
            'x': msg.pose.pose.position.x,
            'y': msg.pose.pose.position.y,
            'yaw': yaw,
        })

    def _on_map(self, msg: OccupancyGrid):
        now = time.monotonic()
        if now - self._last_map_sent_at < MAP_SEND_INTERVAL_SEC:
            return
        if msg.info.width == 0 or msg.info.height == 0:
            return
        self._last_map_sent_at = now

        pgm_bytes = _occupancy_grid_to_pgm_bytes(msg)
        self._send({
            'type': 'slam_map_update',
            'width': msg.info.width,
            'height': msg.info.height,
            'resolution': msg.info.resolution,
            'origin_x': msg.info.origin.position.x,
            'origin_y': msg.info.origin.position.y,
            'pgm_base64': base64.b64encode(pgm_bytes).decode('ascii'),
        })

    def _on_scan(self, msg: LaserScan):
        self._latest_scan = msg

    def _on_slam_overlay_tick(self):
        """map->base_link TF가 있을 때만(=SLAM 진행 중) 위치·스캔을 내보낸다.

        TF가 아직 없으면(SLAM 시작 전) 예외를 조용히 삼키고 스킵한다 — /amcl_pose와
        달리 이 경로는 map 프레임이 생기기 전엔 항상 실패하는 게 정상이기 때문이다.
        """
        try:
            tf = self._tf_buffer.lookup_transform('map', 'base_link', rclpy.time.Time())
        except (tf2_ros.LookupException, tf2_ros.ConnectivityException, tf2_ros.ExtrapolationException):
            return

        x = tf.transform.translation.x
        y = tf.transform.translation.y
        yaw = _yaw_from_quaternion(tf.transform.rotation)
        self._send({'type': 'position', 'x': x, 'y': y, 'yaw': yaw})
        self._send_scan_overlay()

    def _send_scan_overlay(self):
        scan = self._latest_scan
        if scan is None:
            return
        try:
            tf = self._tf_buffer.lookup_transform('map', scan.header.frame_id, rclpy.time.Time())
        except (tf2_ros.LookupException, tf2_ros.ConnectivityException, tf2_ros.ExtrapolationException):
            return

        ranges = np.array(scan.ranges, dtype=np.float64)
        valid = np.isfinite(ranges) & (ranges >= scan.range_min) & (ranges <= scan.range_max)
        idx = np.nonzero(valid)[0]
        if idx.size == 0:
            return
        if idx.size > MAX_SCAN_POINTS:
            idx = idx[np.linspace(0, idx.size - 1, MAX_SCAN_POINTS).astype(int)]

        angles = scan.angle_min + idx * scan.angle_increment
        r = ranges[idx]
        local_x = r * np.cos(angles)
        local_y = r * np.sin(angles)

        tx, ty = tf.transform.translation.x, tf.transform.translation.y
        yaw = _yaw_from_quaternion(tf.transform.rotation)
        cos_yaw, sin_yaw = math.cos(yaw), math.sin(yaw)
        map_x = tx + local_x * cos_yaw - local_y * sin_yaw
        map_y = ty + local_x * sin_yaw + local_y * cos_yaw

        points = np.empty(map_x.size * 2, dtype=np.float64)
        points[0::2] = map_x
        points[1::2] = map_y
        self._send({'type': 'slam_scan_update', 'points': np.round(points, 3).tolist()})

    def _on_status_topic(self, msg: String):
        # patrol_task.py / SLAM 진행률 퍼블리셔가 이미 서버가 기대하는 형식
        # ({"type": "patrol_progress", ...} 등)으로 JSON을 만들어 발행하므로
        # 여기서는 그대로 전달만 한다.
        try:
            payload = json.loads(msg.data)
        except ValueError:
            self.get_logger().warn(f'상태 토픽 JSON 파싱 실패: {msg.data!r}')
            return
        self._send(payload)

    def _send(self, payload: dict):
        if not self._connected or self._ws is None:
            return
        asyncio.run_coroutine_threadsafe(self._ws.send(json.dumps(payload)), self._loop)

    # ---------- asyncio 쪽: 연결 유지 + 재연결 (체크리스트 ⑩) ----------

    def _run_ws_loop(self):
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect_forever())

    async def _connect_forever(self):
        import websockets

        url = f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}'
        while rclpy.ok():
            try:
                async with websockets.connect(url) as ws:
                    self._ws = ws
                    self._connected = True
                    self.get_logger().info(f'서버 업링크 연결됨: {url}')
                    await ws.wait_closed()
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                self.get_logger().warn(f'업링크 연결 실패, {RECONNECT_DELAY_SEC}s 후 재시도: {exc}')
            self._connected = False
            self._ws = None
            await asyncio.sleep(RECONNECT_DELAY_SEC)


def main(args=None):
    rclpy.init(args=args)
    node = UplinkNode()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
