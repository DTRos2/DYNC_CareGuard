"""careguard_turtlebot_agent/uplink_node.py

터틀봇 -> 서버 업링크. 서버의 기존 /ws/robot?robot_code=... 에 접속을 유지하며
{"type":"position",...}를 보낸다 — 서버(app/ws/robot_ws.py::_handle_message)가
그대로 {"type":"robot_position",...}로 대시보드에 브로드캐스트한다. 접속/해제는
서버가 자동으로 robot_status(online True/False)를 브로드캐스트하므로 별도
heartbeat 메시지는 필요 없다.

연결 유지·재연결 로직은 robot_careguard_ws/src/careguard_agent/uplink_node.py
(LIMO용)의 _connect_forever를 그대로 재사용했다.

2026-09-14까지는 position 소스가 /odom(nav_msgs/Odometry)이었다 — 그 시점엔
이 로봇이 기본 bringup만으로는 AMCL/SLAM이 켜져있지 않아 map->base_link TF도,
/amcl_pose도 존재하지 않았기 때문(2026-09-07 조사)에 map 프레임 절대 위치가
아니라 /odom 기준 상대 위치(로봇 부팅 시점이 원점)를 보내는 좁은 범위의
1단계 결정이었다.

2026-09-14에 downlink_node.py가 Nav2(AMCL)를 실제로 띄울 수 있게 되면서(같은
날짜 커밋들 참고) 그 전제가 깨졌다 — 대시보드 "초기 위치 설정"으로 AMCL pose를
다시 잡아도 여기서 여전히 /odom(맵과 무관한 로봇 부팅 시점 기준 좌표)을 보내고
있어서 화면상 로봇이 전혀 안 움직이는 것처럼 보이는 문제로 실제 재현·확인했다.
그래서 LIMO(careguard_agent/uplink_node.py)의 순찰(P6) 단계와 동일하게 /amcl_pose
구독으로 교체한다 — Nav2가 아직 안 떠서 /amcl_pose가 없으면 position 자체를
안 보내는 것도 LIMO와 동일한 동작이다(맵과 무관한 좌표를 보내느니 안 보내는
게 낫다).

지도 등 다른 토픽 릴레이는 이번 범위 밖이라 구독하지 않는다 — 필요해지면
careguard_agent/uplink_node.py를 참고해 여기에 추가하면 된다. /battery_state만
"배터리 주의/복귀" 임계값 판단(서버 쪽, robot_ws.py::_handle_battery_state)에
써야 해서 예외적으로 relay한다.

2026-09-14: P6 순찰 지도의 실시간 라이다 스캔 표시(slam_scan_update WS 메시지,
대시보드 feature/p6-lidar-scan-overlay) 기능이 LIMO(careguard_agent/uplink_node.py
::_send_scan_overlay)에만 있고 터틀봇엔 없어서, 초기 위치를 잡아도 라이다 점이
전혀 안 뜬다는 리포트로 여기도 같은 방식을 추가했다 — /scan(LaserScan) 구독 +
tf2_ros로 map->스캔 프레임 TF 조회 + 각 포인트를 map 프레임 좌표로 변환해
slam_scan_update 전송. position(_on_pose)은 이미 /amcl_pose를 직접 구독하고
있어 TF가 따로 필요 없지만, 스캔 포인트는 원래 그 스캔 프레임(base_scan 등)
기준이라 map 프레임으로 옮기려면 TF 조회가 꼭 필요하다 — 그래서 tf2_ros가
이번에 새로 들어왔다.

카메라는 2026-09-11에 추가했다 — 이 로봇(R-05/R-06)은 LIMO의 orbbec_camera와
달리 라즈베리파이 카메라(camera_ros/libcamera, imx219)를 쓰고, 기본 토픽 이름도
다르다(`/camera/image_raw/compressed` — LIMO는 `/camera/color/image_raw/compressed`).
camera_ros가 image_transport로 발행하면 compressed_image_transport 플러그인이
`/camera/image_raw/compressed`(sensor_msgs/CompressedImage, 이미 JPEG)를 별도
republish 노드 없이 자동으로 같이 발행해준다 — 그래서 LIMO와 동일하게 재인코딩 없이
그대로 relay하면 된다. 로봇에서 실제로 `ros2 topic hz /camera/image_raw/compressed`로
30Hz 발행을 확인했다(2026-09-11). camera_ros 노드 자체를 띄우는 건 이 파일이 아니라
로봇 bringup(systemd) 쪽 책임이다.

카메라 상하반전(CAREGUARD_CAMERA_FLIP_VERTICAL=1)도 2026-09-11에 추가했다 — R-05/R-06
둘 다 카메라가 물리적으로 뒤집혀 마운트돼 있는데, 이 로봇들의 libcamera 버전(0.1)은
camera_ros의 `orientation` 파라미터를 지원하지 않아("parameter 'orientation' not
supported on libcamera 0.1", 실제로 확인) 드라이버 단에서 회전을 못 시킨다. 그래서
여기(소프트웨어)에서 뒤집는다 — R-06/LIMO는 이 환경변수를 안 켜므로 아래 로직을 아예
안 타고(패스스루) 완전히 무관하다.

뒤집기가 필요한 로봇(R-05/R-06)은 압축된 `/camera/image_raw/compressed`를 구독해
JPEG 디코딩 -> 뒤집기 -> 재인코딩한다. 압축 해제+재압축이라 코덱을 두 번 돌리는
셈이지만, 대신 비압축 원본 `/camera/image_raw`를 구독하는 방식은 실제로 더 느렸다
— 640x480 RGB888 비압축 프레임은 30Hz로 초당 약 27MB인데, 우리는 그 중 5개만
쓰고 나머지는 버려도 rclpy가 30Hz 전부를 매번 역직렬화해야 해서(DDS 수신 자체의
비용), 그 비용이 JPEG 디코딩 비용보다 더 컸다(2026-09-11 실측: uplink_node CPU가
33~40% -> 44~50%로 오히려 증가). 그래서 원본 구독 시도는 되돌리고 압축 토픽 +
디코딩 방식을 유지한다 — "압축 해제를 없애서 가볍게"라는 직관이 이번엔 틀렸다.
"""
import asyncio
import base64
import json
import math
import os
import threading
import time

import cv2
import numpy as np
import rclpy
import requests
import tf2_ros
from geometry_msgs.msg import PoseWithCovarianceStamped
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import BatteryState, CompressedImage, LaserScan

from . import ws_config

DEFAULT_RECONNECT_DELAY_SEC = 3.0

# careguard_agent(LIMO)의 SLAM_OVERLAY_INTERVAL_SEC/MAX_SCAN_POINTS와 같은 값 —
# 대시보드가 두 로봇 종류를 구분 안 하고 같은 slam_scan_update를 그리므로, 점
# 밀도·갱신 주기가 로봇마다 달라 보이지 않게 맞춘다.
SCAN_OVERLAY_INTERVAL_SEC = 0.3
MAX_SCAN_POINTS = 450

# 카메라 라이브 미리보기 전송 간격. 대시보드 우측 상단 작은 박스용이라 원본
# 프레임레이트(30Hz) 그대로 보낼 필요는 없다. 온라인/오프라인 흔들림 조사 중
# uplink_node CPU를 더 줄이기 위해 2026-09-12에 5Hz(0.2s) -> 3Hz(약 0.33s)로
# 낮췄다 — 압축 프레임 구독 자체는 30Hz 그대로 들어와 역직렬화 비용은 그대로지만,
# 디코딩(뒤집기 로봇만)·재인코딩·base64 인코딩·전송 비용은 이 값에 비례해 줄어든다.
CAMERA_SEND_INTERVAL_SEC = 1.0 / 3.0

# 이 로봇의 카메라가 물리적으로 뒤집혀 마운트된 경우에만(R-05) systemd Environment로 켠다.
CAMERA_FLIP_VERTICAL = os.environ.get('CAREGUARD_CAMERA_FLIP_VERTICAL', '0') == '1'


def _fetch_reconnect_delay_sec(logger) -> float:
    """대시보드 "시스템 설정 > 임계값·알림"에서 저장한 재연결 재시도 주기를 가져온다.

    robot_id를 모르니 /robots에서 코드로 먼저 찾고 나서 /robots/{id}/thresholds를
    부른다 — 시작할 때 한 번만 하는 동기 호출이라 순서대로 두 번 부르는 걸로 충분하다.
    서버가 아직 안 떠 있거나 이 로봇이 등록 전이면 기본값(3.0s)으로 조용히 넘어간다.
    """
    try:
        robots = requests.get(f'{ws_config.SERVER_HTTP_URL}/robots', timeout=5).json()
        robot = next((r for r in robots if r['code'] == ws_config.ROBOT_CODE), None)
        if robot is None:
            logger.warn(f'{ws_config.ROBOT_CODE} 미등록 — 재연결 주기 기본값({DEFAULT_RECONNECT_DELAY_SEC}s) 사용')
            return DEFAULT_RECONNECT_DELAY_SEC
        thresholds = requests.get(
            f'{ws_config.SERVER_HTTP_URL}/robots/{robot["id"]}/thresholds', timeout=5).json()
        return float(thresholds['reconnect_retry_sec'])
    except Exception as exc:  # noqa: BLE001 - 시작을 막으면 안 되니 실패해도 기본값으로 계속 진행
        logger.warn(f'재연결 주기 조회 실패, 기본값({DEFAULT_RECONNECT_DELAY_SEC}s) 사용: {exc}')
        return DEFAULT_RECONNECT_DELAY_SEC


def _yaw_from_quaternion(q) -> float:
    siny_cosp = 2 * (q.w * q.z + q.x * q.y)
    cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z)
    return math.atan2(siny_cosp, cosy_cosp)


class UplinkNode(Node):
    def __init__(self):
        super().__init__('uplink_node')

        if not ws_config.ROBOT_CODE:
            self.get_logger().error(
                "CAREGUARD_ROBOT_CODE 환경변수가 비어 있습니다. 예: "
                "CAREGUARD_ROBOT_CODE=R-05 ros2 run careguard_turtlebot_agent uplink_node")
            raise SystemExit(1)

        self._loop = asyncio.new_event_loop()
        self._ws = None
        self._connected = False
        self._reconnect_delay_sec = _fetch_reconnect_delay_sec(self.get_logger())
        self._last_camera_sent_at = 0.0
        self._latest_scan = None

        self.create_subscription(PoseWithCovarianceStamped, '/amcl_pose', self._on_pose, 10)
        self.create_subscription(BatteryState, '/battery_state', self._on_battery_state, 10)
        # 카메라 드라이버(camera_ros)도 센서 QoS로 퍼블리시해서(라이다와 같은 이유)
        # sensor QoS로 구독한다 — LIMO의 _on_camera_frame 구독과 동일한 이유.
        self.create_subscription(CompressedImage, '/camera/image_raw/compressed',
                                  self._on_camera_frame, qos_profile_sensor_data)
        # LDS(LDS-01/LDS-03) 드라이버도 BEST_EFFORT로 퍼블리시한다 — LIMO의 ydlidar와
        # 같은 이유로 sensor QoS로 구독해야 한다(기본 RELIABLE로는 아무것도 안 옴).
        self.create_subscription(LaserScan, '/scan', self._on_scan, qos_profile_sensor_data)
        self._tf_buffer = tf2_ros.Buffer()
        self._tf_listener = tf2_ros.TransformListener(self._tf_buffer, self)
        self.create_timer(SCAN_OVERLAY_INTERVAL_SEC, self._on_slam_overlay_tick)

        self._thread = threading.Thread(target=self._run_ws_loop, daemon=True)
        self._thread.start()

    def _on_pose(self, msg: PoseWithCovarianceStamped):
        yaw = _yaw_from_quaternion(msg.pose.pose.orientation)
        self._send({
            'type': 'position',
            'x': msg.pose.pose.position.x,
            'y': msg.pose.pose.position.y,
            'yaw': yaw,
        })

    def _on_scan(self, msg: LaserScan):
        self._latest_scan = msg

    def _on_slam_overlay_tick(self):
        """map->base_link TF가 있을 때만(=Nav2/AMCL 켜짐) 위치를 보완 전송한다.

        지금까지는 위치를 /amcl_pose 구독(_on_pose)에만 의존했는데, AMCL은 로봇이
        멈춰있거나 스캔 매칭 갱신이 뜸할 때 새 추정치를 잘 안 내놓아 대시보드 위치도
        같이 뜸해지는 문제가 있었다(LIMO는 careguard_agent::_on_slam_overlay_tick이
        이미 TF 타이머 방식이라 이 문제가 없음 — 2026-09-15 R-06만 실시간 위치가
        안 잡힌다는 리포트로 원인 확인). LIMO와 동일하게 이 스캔 오버레이 타이머
        (0.3초)에 얹어 TF를 직접 읽어 보낸다 — _on_pose는 그대로 두고(공분산 있는
        추정치라 값 자체는 더 정확) 두 경로가 같이 있어도 더 자주 갱신될 뿐 문제없다.
        """
        try:
            tf = self._tf_buffer.lookup_transform('map', 'base_link', rclpy.time.Time())
        except (tf2_ros.LookupException, tf2_ros.ConnectivityException, tf2_ros.ExtrapolationException):
            pass
        else:
            self._send({
                'type': 'position',
                'x': tf.transform.translation.x,
                'y': tf.transform.translation.y,
                'yaw': _yaw_from_quaternion(tf.transform.rotation),
            })
        self._send_scan_overlay()

    def _send_scan_overlay(self):
        """careguard_agent(LIMO)::_send_scan_overlay와 동일 — 최신 /scan을
        map 프레임 좌표로 변환해 slam_scan_update로 보낸다. Nav2(AMCL)가 아직
        안 떠서 map->스캔 프레임 TF가 없으면(패트롤 대기 중 등) 조용히
        스킵한다 — _on_pose가 /amcl_pose 없을 때 아무것도 안 보내는 것과 같은
        이유(맵과 무관한 좌표를 보내느니 안 보내는 게 낫다).
        """
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

    def _on_battery_state(self, msg: BatteryState):
        # 드라이버에 따라 percentage를 0.0~1.0(REP 표준) 또는 0~100 그대로 발행하는
        # 경우가 섞여 있어(turtlebot3_node가 후자인 경우가 실제로 관찰됨), 1.0 이하면
        # 비율로 보고 100을 곱한다.
        percentage = msg.percentage * 100.0 if msg.percentage <= 1.0 else msg.percentage
        self._send({'type': 'battery_state', 'percentage': percentage})

    def _on_camera_frame(self, msg: CompressedImage):
        now = time.monotonic()
        if now - self._last_camera_sent_at < CAMERA_SEND_INTERVAL_SEC:
            return
        if not msg.data:
            return
        self._last_camera_sent_at = now
        # 카메라가 똑바로 마운트된 로봇(플래그 미설정)은 msg.data가 이미 JPEG로
        # 압축되어 있어(compressed_image_transport) 재인코딩 없이 그대로 relay한다.
        # 뒤집힌 마운트(플래그 설정)만 cv2로 디코딩 -> 상하반전 -> 재인코딩한다.
        jpeg_bytes = bytes(msg.data)
        if CAMERA_FLIP_VERTICAL:
            image = cv2.imdecode(np.frombuffer(jpeg_bytes, dtype=np.uint8), cv2.IMREAD_COLOR)
            if image is None:
                return
            flipped = cv2.flip(image, 0)
            ok, encoded = cv2.imencode('.jpg', flipped)
            if not ok:
                return
            jpeg_bytes = encoded.tobytes()
        self._send({
            'type': 'camera_frame',
            'jpeg_base64': base64.b64encode(jpeg_bytes).decode('ascii'),
        })

    def _send(self, payload: dict):
        if not self._connected or self._ws is None:
            return
        asyncio.run_coroutine_threadsafe(self._ws.send(json.dumps(payload)), self._loop)

    def _run_ws_loop(self):
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect_forever())

    async def _connect_forever(self):
        import websockets

        # role=uplink를 명시해야 한다 — 안 보내면 서버(robot_ws.py)가 기본값인
        # downlink로 취급해서, 이 연결이 나중에 맺어질 때 downlink_node의 명령
        # 수신용 연결(_connections[robot_code])을 덮어써버린다. 그러면 서버가
        # patrol_start/nav_prepare를 이 uplink 연결로 보내게 돼 downlink_node가
        # 명령을 영영 못 받는다 — 2026-09-14 R-06에서 nav_prepare가 로봇까지
        # 안 오는 문제로 실제 재현·확인했다(서버 로그에 role 없는 연결이 찍힘).
        url = f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}&role=uplink'
        while rclpy.ok():
            try:
                async with websockets.connect(url) as ws:
                    self._ws = ws
                    self._connected = True
                    self.get_logger().info(f'서버 업링크 연결됨: {url}')
                    await ws.wait_closed()
            except Exception as exc:  # noqa: BLE001 - 재연결 루프이므로 모든 예외를 삼킨다
                self.get_logger().warn(f'업링크 연결 실패, {self._reconnect_delay_sec}s 후 재시도: {exc}')
            self._connected = False
            self._ws = None
            await asyncio.sleep(self._reconnect_delay_sec)


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
