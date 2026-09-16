#!/usr/bin/env python3
"""careguard_turtlebot_agent/perception/photo_identify_node.py

트리거를 받으면 R-06 카메라를 잠깐 열어 사진 1장을 판정(대상자/침입자)하고 바로
카메라를 닫는 노드. face_capture_node.py와 마찬가지로 상시 구독을 피한다 —
ArUco 도킹 CPU 과부하 사고(docs/TroubleShooting/2026-09-15-turtlebot-aruco-cpu-
overload.md) 이후 확정된 원칙: 카메라 디코딩+무거운 연산은 필요한 순간에만 열고
끝나면 반드시 닫는다.

전제: build_target_centroid.py로 만든 target_centroid.pt가 있어야 한다(zero-shot
FaceNet centroid, adapter 없음 — R-07과 달리 등록 대상자가 1명뿐이라 A/B 구분용
adapter가 필요 없다는 게 이 프로젝트의 결론이다).

트리거/결과 프로토콜(둘 다 std_msgs/String, JSON):
  구독: perception/identify_trigger — 아무 내용이나 오면(빈 문자열도 OK) 판정 1회 시작.
        이미 판정 진행 중이면 무시한다.
  발행: perception/identify_result — {"label": "target"|"intruder"|"no_face",
        "distance": float|null, "elapsed_sec": float}
        - "target": identity_threshold 이내로 가까움 (등록된 대상자)
        - "intruder": 사람 얼굴은 잡혔지만 threshold보다 멂 (대상자 아님)
        - "no_face": capture_timeout_sec 안에 조건(얼굴 1개+크기+블러) 통과하는
          프레임을 못 찾음 — 판단 불가(사람이 없거나 카메라 각도가 안 맞음 등)

label이 "target"이면 추가로 /careguard/perception/person_detected(std_msgs/Bool,
data=True)도 발행한다. 이 토픽/타입은 새로 정한 게 아니라 `feature/pillbox-slot-
rotation` 브랜치의 careguard_mission/patrol_task.py·medication_task.py가 이미
구독하고 있는 계약이다(그쪽 코드 주석: "아직 이 토픽을 실제로 발행하는 실시간
인식 노드는 없다 — 이 구독은 언젠가 생길 발행자를 기다리는 자리") — 이 노드가
바로 그 발행자가 된다. 다만 그 구독자들은 지금 robot_careguard_ws(R-07/LIMO
워크스페이스)에 있어서, 약통 개방까지 실제로 이어지려면 그 로직을 R-06에도
가져오거나 무언가가 두 워크스페이스를 이어줘야 한다 — 이 노드는 "발행"까지만
책임진다.

아직 웨이포인트 도착 이벤트(patrol_controller.py)와 연결 안 함 — 그 통합은
patrol_controller.py가 identify_trigger를 자동으로 언제 쏠지(어느 웨이포인트인지
판단하는 로직) 정한 뒤에 할 일이라 이 노드는 트리거 프로토콜만 우선 만들어서
독립적으로 테스트 가능하게 했다.

identity_threshold 기본값(0.4)은 검증 안 된 값이다 — R-07 문서에도 "zero-shot
단독으로는 threshold=0.40에서 낯선 사람 오수용률이 약 25%"라는 실측이 있어서,
실전 배치 전에 반드시 등록 안 한 다른 사람 사진으로 오수용률을 확인하고 맞춰야
한다.

사용법:
    ros2 run careguard_turtlebot_agent photo_identify_node --ros-args \\
        -p centroid_path:=/home/yeyu/careguard_data/target_centroid.pt \\
        -p identity_threshold:=0.4

    # 다른 터미널에서 판정 트리거:
    ros2 topic pub --once perception/identify_trigger std_msgs/msg/String "data: ''"
    ros2 topic echo perception/identify_result
"""
import json
import os
import time

import cv2
import numpy as np
import rclpy
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage
from std_msgs.msg import Bool, String

import torch
import torch.nn.functional as F
from PIL import Image as PILImage
from facenet_pytorch import InceptionResnetV1, fixed_image_standardization

from .face_capture_node import CAMERA_TOPIC, CAMERA_FLIP_VERTICAL, _find_haarcascade_path

EMBED_SIZE = 160  # build_target_centroid.py와 반드시 동일해야 함


def _crop_with_margin(image, box, margin):
    x, y, w, h = box
    img_h, img_w = image.shape[:2]
    mx, my = int(w * margin), int(h * margin)
    x0, y0 = max(0, x - mx), max(0, y - my)
    x1, y1 = min(img_w, x + w + mx), min(img_h, y + h + my)
    return image[y0:y1, x0:x1]


def _embed_crop(crop_bgr, model):
    """build_target_centroid.py::embed_crops()와 완전히 동일한 전처리(160x160,
    PIL 기본 BICUBIC 리사이즈, fixed_image_standardization) — 등록 때와 판정 때
    리사이즈 알고리즘이 다르면 임베딩이 어긋나는 문제가 R-07에서 실증됐던 것과
    같은 이유로 한 글자도 다르게 짜면 안 된다."""
    crop_rgb = cv2.cvtColor(crop_bgr, cv2.COLOR_BGR2RGB)
    resized = np.asarray(PILImage.fromarray(crop_rgb).resize((EMBED_SIZE, EMBED_SIZE)))
    t = torch.from_numpy(resized).permute(2, 0, 1).float().unsqueeze(0)
    t = fixed_image_standardization(t)
    with torch.no_grad():
        emb = model(t)
    return emb[0]


class PhotoIdentifyNode(Node):
    def __init__(self):
        super().__init__('photo_identify_node')

        self.declare_parameter('centroid_path', os.path.expanduser('~/careguard_data/target_centroid.pt'))
        self.declare_parameter('identity_threshold', 0.4)
        self.declare_parameter('capture_timeout_sec', 5.0)
        self.declare_parameter('blur_threshold', 15.0)
        self.declare_parameter('crop_margin', 0.25)
        self.declare_parameter('cascade_path', '')

        self.identity_threshold = self.get_parameter('identity_threshold').value
        self.capture_timeout_sec = self.get_parameter('capture_timeout_sec').value
        self.blur_threshold = self.get_parameter('blur_threshold').value
        self.crop_margin = self.get_parameter('crop_margin').value
        cascade_path = self.get_parameter('cascade_path').value or _find_haarcascade_path()

        centroid_path = self.get_parameter('centroid_path').value
        ckpt = torch.load(centroid_path, map_location='cpu')
        self.centroid = ckpt['target']
        self.get_logger().info(f"centroid 로딩: {centroid_path} (등록 {ckpt.get('n', '?')}장 기반)")

        self.face_cascade = cv2.CascadeClassifier(cascade_path)
        if self.face_cascade.empty():
            raise RuntimeError(f"cascade 로딩 실패: {cascade_path}")

        self.get_logger().info("FaceNet(frozen, vggface2) 로딩...")
        self.embedder = InceptionResnetV1(pretrained='vggface2').eval()
        self.get_logger().info(
            f"판정 준비 완료 (identity_threshold={self.identity_threshold} — "
            f"미검증 기본값, 실전 배치 전 검증 필요)"
        )

        self.result_pub = self.create_publisher(String, 'perception/identify_result', 10)
        # careguard_mission(R-07)의 patrol_task.py/medication_task.py가 이미 구독
        # 중인 계약 — 모듈 docstring 참고. 절대 경로(/careguard/...)로 명시한다.
        self.person_detected_pub = self.create_publisher(Bool, '/careguard/perception/person_detected', 10)
        self.create_subscription(String, 'perception/identify_trigger', self._on_trigger, 10)

        self._awaiting = False
        self._camera_sub = None
        self._timeout_timer = None
        self._trigger_start = 0.0

    def _on_trigger(self, _msg: String):
        if self._awaiting:
            self.get_logger().warn('이미 판정 진행 중 — 이번 트리거는 무시합니다')
            return
        self._awaiting = True
        self._trigger_start = time.monotonic()
        self.get_logger().info('판정 시작 — 카메라 구독을 엽니다')
        self._camera_sub = self.create_subscription(
            CompressedImage, CAMERA_TOPIC, self._on_camera_frame, qos_profile_sensor_data
        )
        self._timeout_timer = self.create_timer(self.capture_timeout_sec, self._on_timeout)

    def _on_timeout(self):
        if not self._awaiting:
            return
        self.get_logger().warn(f'{self.capture_timeout_sec}초 안에 쓸만한 얼굴을 못 찾음 — no_face')
        self._finish({'label': 'no_face', 'distance': None})

    def _on_camera_frame(self, msg: CompressedImage):
        if not self._awaiting or not msg.data:
            return

        image = cv2.imdecode(np.frombuffer(bytes(msg.data), dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            return
        if CAMERA_FLIP_VERTICAL:
            image = cv2.flip(image, 0)

        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        faces = self.face_cascade.detectMultiScale(
            gray, scaleFactor=1.1, minNeighbors=6, minSize=(25, 25)
        )
        if len(faces) != 1:
            return

        x, y, w, h = faces[0]
        img_h, img_w = image.shape[:2]
        face_ratio = (w * h) / (img_w * img_h)
        if not (0.003 <= face_ratio <= 0.6):
            return

        blur_score = cv2.Laplacian(gray, cv2.CV_64F).var()
        if blur_score < self.blur_threshold:
            return

        crop = _crop_with_margin(image, (x, y, w, h), self.crop_margin)
        emb = _embed_crop(crop, self.embedder)
        distance = float(1 - (emb @ self.centroid))
        label = 'target' if distance <= self.identity_threshold else 'intruder'
        self.get_logger().info(f'판정 완료: {label} (distance={distance:.4f})')
        self._finish({'label': label, 'distance': distance})

    def _finish(self, result: dict):
        result['elapsed_sec'] = round(time.monotonic() - self._trigger_start, 2)
        self.result_pub.publish(String(data=json.dumps(result, ensure_ascii=False)))

        if result['label'] == 'target':
            self.person_detected_pub.publish(Bool(data=True))

        if self._camera_sub is not None:
            self.destroy_subscription(self._camera_sub)
            self._camera_sub = None
        if self._timeout_timer is not None:
            self._timeout_timer.cancel()
            self._timeout_timer = None
        self._awaiting = False


def main(args=None):
    rclpy.init(args=args)
    node = PhotoIdentifyNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    node.destroy_node()
    if rclpy.ok():
        rclpy.shutdown()


if __name__ == '__main__':
    main()
