"""careguard_turtlebot_agent/uplink_node.py

터틀봇 -> 서버 업링크. 서버의 기존 /ws/robot?robot_code=... 에 접속을 유지하며
{"type":"position",...}를 보낸다 — 서버(app/ws/robot_ws.py::_handle_message)가
그대로 {"type":"robot_position",...}로 대시보드에 브로드캐스트한다. 접속/해제는
서버가 자동으로 robot_status(online True/False)를 브로드캐스트하므로 별도
heartbeat 메시지는 필요 없다.

연결 유지·재연결 로직은 robot_careguard_ws/src/careguard_agent/uplink_node.py
(LIMO용)의 _connect_forever를 그대로 재사용했다.

position 소스는 /odom(nav_msgs/Odometry)이다 — 실제 파이 조사 결과
(2026-09-07) 이 로봇은 기본 bringup만으로는 AMCL/SLAM이 켜져있지 않아
map->base_link TF도, /amcl_pose도 존재하지 않는다. 그래서 map 프레임 절대
위치가 아니라 /odom 기준 상대 위치(로봇 부팅 시점이 원점)를 보낸다 — 범위를
좁게 잡은 의도적 1단계 결정이다. AMCL/맵 정렬 위치가 필요해지면 LIMO의
_on_slam_overlay_tick처럼 map->base_link TF 구독으로 교체하면 된다.

지도 등 다른 토픽 릴레이는 이번 범위 밖이라 구독하지 않는다 — 필요해지면
careguard_agent/uplink_node.py를 참고해 여기에 추가하면 된다. /battery_state만
"배터리 주의/복귀" 임계값 판단(서버 쪽, robot_ws.py::_handle_battery_state)에
써야 해서 예외적으로 relay한다.

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
from nav_msgs.msg import Odometry
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import BatteryState, CompressedImage

from . import ws_config

DEFAULT_RECONNECT_DELAY_SEC = 3.0

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

        self.create_subscription(Odometry, '/odom', self._on_odom, 10)
        self.create_subscription(BatteryState, '/battery_state', self._on_battery_state, 10)
        # 카메라 드라이버(camera_ros)도 센서 QoS로 퍼블리시해서(라이다와 같은 이유)
        # sensor QoS로 구독한다 — LIMO의 _on_camera_frame 구독과 동일한 이유.
        self.create_subscription(CompressedImage, '/camera/image_raw/compressed',
                                  self._on_camera_frame, qos_profile_sensor_data)

        self._thread = threading.Thread(target=self._run_ws_loop, daemon=True)
        self._thread.start()

    def _on_odom(self, msg: Odometry):
        yaw = _yaw_from_quaternion(msg.pose.pose.orientation)
        self._send({
            'type': 'position',
            'x': msg.pose.pose.position.x,
            'y': msg.pose.pose.position.y,
            'yaw': yaw,
        })

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

        url = f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}'
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
