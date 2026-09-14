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

2026-09-14에 position 메시지에 pos_std_m(x/y 공분산 대각 성분에서 뽑은 위치
표준편차, 미터)을 추가했다 — _on_pose 참고. _on_slam_overlay_tick(TF 기반)
경로는 공분산이 없어 이 필드를 안 보낸다(그 경로는 P5 SLAM 뷰 전용이라 P6의
수렴 판단과 무관).

/camera/color/image_raw/compressed(orbbec_camera, sensor_msgs/CompressedImage)도
같은 방식으로 relay한다 — 이미 JPEG로 압축되어 있어 재인코딩 없이 그대로
base64만 씌워 보낸다(대시보드 P5/P6 우측 상단 카메라 라이브 박스용).

/limo_status(limo_base, limo_msgs/msg/LimoStatus)도 relay한다 — 터틀봇(R-05/R-06,
careguard_turtlebot_agent/uplink_node.py)은 sensor_msgs/BatteryState를 그대로 쓰지만,
LIMO(wego 패키지 limo_base)는 표준 BatteryState가 아니라 자체 메시지로 battery_voltage
(실수, 볼트)만 준다 — 퍼센트가 아니다. 그래서 여기서 직접 전압 -> % 환산이 필요하다.
_LIMO_BATTERY_VOLTAGE_MIN/MAX 상수 참고.

/plan(nav2_planner, nav_msgs/Path)도 relay한다 — 순찰(Nav2) 중 로봇이 실제로
따라가려는 전역 경로를 대시보드 P6에 rviz2처럼 선으로 그려주기 위함
(2026-09-08 요청으로 추가, 실 로봇 미검증).

video_recorder.py가 남긴 mp4+json 업로드도 이 노드가 같이 맡는다 —
media_uploader.start_video_upload_watcher() 백그라운드 스레드 하나만 띄운다.
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
from limo_msgs.msg import LimoStatus
from nav_msgs.msg import OccupancyGrid, Path
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage, LaserScan
from std_msgs.msg import String

from . import media_uploader, ws_config

RECONNECT_DELAY_SEC = 3.0

# /map을 너무 자주 보내면(cartographer_occupancy_grid_node는 기본 1Hz) 대시보드
# WS·네트워크에 부담이 크다 — 이 간격보다 자주는 보내지 않는다.
MAP_SEND_INTERVAL_SEC = 2.0

# SLAM 중 실시간 로봇 위치·라이다 스캔 오버레이(대시보드 rviz2 스타일 뷰) 갱신 간격.
# /map보다는 자주 갱신해야 "실시간"처럼 보인다.
SLAM_OVERLAY_INTERVAL_SEC = 0.3

# 스캔 포인트를 그대로 다 보내면 메시지가 커진다 — 화면에 뿌릴 용도로는 이 정도로 충분하다.
MAX_SCAN_POINTS = 450

# 카메라 라이브 미리보기 전송 간격 — 대시보드 우측 상단 작은 박스에 보여줄 용도라
# 원본 카메라 프레임레이트 그대로 보낼 필요는 없다. 이 정도면 "실시간"으로 보이면서도
# WS 대역폭 부담이 적다.
CAMERA_SEND_INTERVAL_SEC = 0.2

# LIMO(3S 리튬이온 팩) 전압 -> 배터리 % 환산표. 2점 직선(MAX/MIN)으로는 리튬이온의
# 비선형 방전곡선(중간은 완만, 저잔량에서 급격히 꺾임)을 못 따라가서 실제로 크게
# 어긋나는 사례가 나왔다 — 2026-09-14, 물리 게이지가 4칸 중 1.5칸(~37.5%)인데
# 당시 MIN=10.0V 기준 계산값은 1%였다(실측 전압 9.8V는 MIN보다도 낮았음).
# 그래서 실측 지점들을 직접 잇는 구간별 직선(피스와이즈)으로 바꿨다. 전압 내림차순:
#   12.6V -> 100%  : 3S 리튬이온 화학적 완충 전압(4.2V/cell) — 실측 없이도 신뢰 가능
#   11.3V -> 50%   : 2026-09-13 실측. 게이지 3번째 칸이 깜빡이던 시점(칸당 25% 기준
#                    50% 경계로 추정)의 전압
#   9.8V  -> 37.5% : 2026-09-14 실측. 게이지 1.5/4칸(칸당 25% 기준) 상태에서
#                    ros2 topic echo /limo_status로 확인한 전압(정지 상태, 안정적)
#   9.0V  -> 0%    : 실측치 없음. 3S 팩의 일반적인 완전방전 컷오프(3.0V/cell) 추정치
# 중간 두 점(11.3V, 9.8V)은 게이지를 눈대중으로 읽은 값이라 여전히 근사치다.
# 추가로 칸 전환 시점의 전압을 실측할수록 이 표가 더 정확해진다.
LIMO_BATTERY_VOLTAGE_PERCENT_CURVE = (
    (12.6, 100.0),
    (11.3, 50.0),
    (9.8, 37.5),
    (9.0, 0.0),
)


def _limo_voltage_to_percentage(voltage: float) -> float:
    points = LIMO_BATTERY_VOLTAGE_PERCENT_CURVE
    if voltage >= points[0][0]:
        return 100.0
    if voltage <= points[-1][0]:
        return 0.0
    for (v_hi, p_hi), (v_lo, p_lo) in zip(points, points[1:]):
        if v_lo <= voltage <= v_hi:
            ratio = (voltage - v_lo) / (v_hi - v_lo)
            return p_lo + ratio * (p_hi - p_lo)
    return 0.0


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
        self._last_camera_sent_at = 0.0

        self.create_subscription(PoseWithCovarianceStamped, '/amcl_pose', self._on_pose, 10)
        self.create_subscription(OccupancyGrid, '/map', self._on_map, 1)
        # 순찰(Nav2) 중 planner_server가 발행하는 전역 경로 — rviz2가 지도 위에 그려주는
        # "로봇이 가려는 길" 선과 같은 것. 대시보드 P6에도 똑같이 그려주기 위한 relay.
        # QoS는 nav2_planner의 기본(RELIABLE, TRANSIENT_LOCAL 아님, KEEP_LAST)과 맞춘
        # 기본 프로파일로 충분하다 — /scan·/map과 달리 BEST_EFFORT가 필요하지 않다.
        self.create_subscription(Path, '/plan', self._on_global_plan, 10)
        self.create_subscription(String, '/careguard/patrol_status', self._on_status_topic, 10)
        self.create_subscription(String, '/careguard/slam_status', self._on_status_topic, 10)
        # orbbec_camera가 발행하는 압축(JPEG) 이미지 — 재인코딩 없이 그대로 relay하면
        # 되므로 cv_bridge/OpenCV 의존성이 필요 없다. 카메라 드라이버도 센서 QoS로
        # 퍼블리시하는 경우가 많아(ydlidar /scan과 같은 이유) sensor QoS로 구독한다.
        self.create_subscription(CompressedImage, '/camera/color/image_raw/compressed',
                                  self._on_camera_frame, qos_profile_sensor_data)
        self.create_subscription(LimoStatus, '/limo_status', self._on_limo_status, 10)

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

        # 영상 업로드 워처. WS 연결과 무관한 별개의 백그라운드 스레드.
        media_uploader.start_video_upload_watcher(self.get_logger())

    # ---------- ROS2 콜백: 들어오는 대로 asyncio 루프에 넘겨서 전송한다 ----------

    def _on_pose(self, msg: PoseWithCovarianceStamped):
        yaw = _yaw_from_quaternion(msg.pose.pose.orientation)
        # 6x6 행-우선 공분산 행렬(geometry_msgs/PoseWithCovariance)에서 x/y 분산은
        # 대각 성분 [0](x,x)·[7](y,y)에 있다 — P6 대시보드가 "위치 보정" 직후 AMCL이
        # 실제로 수렴했는지 판단하는 데 쓴다(고정 시간 타이머로 흔들림을 가리던
        # 방식은 실 로봇에서 수렴 시간이 그보다 길면 타이머가 먼저 풀려 흔들림이
        # 그대로 보이는 문제가 있었다 — P6PatrolPage::poseCaptureSettling_ 주석 참고).
        # max(cov_x, cov_y, 0.0)로 더 불확실한 축 기준으로 보수적으로 잡는다.
        cov_x = msg.pose.covariance[0]
        cov_y = msg.pose.covariance[7]
        pos_std_m = math.sqrt(max(cov_x, cov_y, 0.0))
        self._send({
            'type': 'position',
            'x': msg.pose.pose.position.x,
            'y': msg.pose.pose.position.y,
            'yaw': yaw,
            'pos_std_m': pos_std_m,
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

    def _on_global_plan(self, msg: Path):
        """Nav2 planner_server가 goal마다 다시 계산해 발행하는 전역 경로(map 프레임,
        이미 실제 미터 좌표라 slam_scan_update처럼 TF 변환이 필요 없다) — rviz2에서
        보이는 "로봇이 가려는 길" 초록/파란 선과 같은 것을 대시보드에도 보여준다.
        poses가 비어 있으면(목표 취소/도달 등으로 경로가 사라짐) 빈 배열을 보내
        대시보드가 그려둔 선을 지우게 한다.
        """
        points = []
        for pose in msg.poses:
            points.append(round(pose.pose.position.x, 3))
            points.append(round(pose.pose.position.y, 3))
        self._send({'type': 'patrol_path_update', 'points': points})

    def _on_scan(self, msg: LaserScan):
        self._latest_scan = msg

    def _on_camera_frame(self, msg: CompressedImage):
        now = time.monotonic()
        if now - self._last_camera_sent_at < CAMERA_SEND_INTERVAL_SEC:
            return
        if not msg.data:
            return
        self._last_camera_sent_at = now
        # msg.data는 이미 JPEG로 압축되어 있다 — 대시보드가 QImage로 바로 디코딩하므로
        # 여기서 OpenCV/cv_bridge로 다시 디코딩·인코딩할 필요가 없다.
        self._send({
            'type': 'camera_frame',
            'jpeg_base64': base64.b64encode(bytes(msg.data)).decode('ascii'),
        })

    def _on_limo_status(self, msg: LimoStatus):
        # battery_voltage(V) -> percentage 환산 — 위 LIMO_BATTERY_VOLTAGE_PERCENT_CURVE 참고.
        # 서버(robot_ws.py::_handle_battery_state)와 대시보드는 터틀봇/LIMO 구분 없이
        # {"type": "battery_state", "percentage": 0~100}만 기대하므로, 환산은 여기서
        # 끝내고 나머지 파이프라인(스무딩·임계값 판단·브로드캐스트)은 그대로 재사용한다.
        percentage = _limo_voltage_to_percentage(msg.battery_voltage)
        self._send({'type': 'battery_state', 'percentage': percentage})

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

        # role=uplink — 서버가 downlink(명령 수신)와 다른 딕셔너리에 이 연결을 담는다.
        # 안 붙이면(role 생략) 서버가 downlink로 취급해, 와이파이 재연결 타이밍에 따라
        # 이 연결이 downlink의 명령 수신 자리를 가로챌 수 있다(server/app/ws/robot_ws.py
        # 참고 — 실제로 이 문제로 teleop/저장 명령이 조용히 씹히는 버그가 있었다).
        url = f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}&role=uplink'
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
        # SIGINT 시 rclpy가 내부적으로 먼저 shutdown()을 부를 수 있어, 여기서
        # 무조건 또 부르면 "rcl_shutdown already called"로 비정상 종료한다
        # (slam_progress_publisher.py에서 실제로 겪은 버그, 같은 패턴이라 방어).
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
