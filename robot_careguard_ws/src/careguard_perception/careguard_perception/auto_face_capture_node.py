#!/usr/bin/env python3
import os
import random
import cv2
import numpy as np

import rclpy
from rclpy.node import Node
from sensor_msgs.msg import CompressedImage
from cv_bridge import CvBridge

# person_label -> class_id 매핑. 사람이 추가되면 여기 한 곳만 고치면 된다.
PERSON_CLASS_MAP = {
    'person_A': 0,
    'person_B': 1,
    'person_C': 2,
    'person_D': 3,
}
# 미등록자(unknown) 촬영 시 참고용 기본값 — 위 매핑의 값들과 절대 겹치면 안 된다.
UNKNOWN_CLASS_ID = 99


class ClassIdMismatchError(RuntimeError):
    """person_label과 class_id 파라미터가 서로 맞지 않을 때 (촬영 시작 전에 막기 위함)."""


def validate_person_class_mapping(person_label: str, class_id: int) -> None:
    if person_label in PERSON_CLASS_MAP:
        expected = PERSON_CLASS_MAP[person_label]
        if class_id != expected:
            raise ClassIdMismatchError(
                f"class_id 불일치: person_label='{person_label}'이면 class_id={expected}이어야 "
                f"하는데 {class_id}로 설정되어 있습니다. -p class_id:={expected} 로 맞춰서 다시 "
                f"실행하세요."
            )
    elif class_id in PERSON_CLASS_MAP.values():
        owner = next(p for p, c in PERSON_CLASS_MAP.items() if c == class_id)
        raise ClassIdMismatchError(
            f"person_label='{person_label}'은 등록된 사람이 아닌데 class_id={class_id}는 이미 "
            f"{owner}용으로 예약된 값입니다. 미등록자는 겹치지 않는 값(예: {UNKNOWN_CLASS_ID})을 "
            f"명시적으로 지정하세요."
        )


class AutoFaceCaptureNode(Node):
    def __init__(self):
        super().__init__('auto_face_capture_node')

        # ── 세션마다 바꿔줄 파라미터 ──
        self.declare_parameter('person_label', 'person_A')   # 이번 세션에서 찍을 사람
        self.declare_parameter('class_id', 0)                 # PERSON_CLASS_MAP 참고
        self.declare_parameter('save_dir', '/home/wego/careguard_data/face_dataset/')
        self.declare_parameter('capture_interval', 0.7)       # 저장 간격(초)
        self.declare_parameter('val_ratio', 0.2)
        # 이번 세션에서 몇 장 찍으면 자동 종료할지. 0이면 무제한(기존 동작 그대로).
        # 촬영 대상자는 화면을 못 보니, 목표 장수+여유분을 넣어두고 노드가
        # 알아서 멈추게 하는 용도 (예: 목표 11장이면 21로 잡고 나중에 골라내기).
        self.declare_parameter('max_images', 0)

        self.person_label = self.get_parameter('person_label').value
        self.class_id = self.get_parameter('class_id').value
        self.save_dir = self.get_parameter('save_dir').value
        self.interval = self.get_parameter('capture_interval').value
        self.val_ratio = self.get_parameter('val_ratio').value
        self.max_images = self.get_parameter('max_images').value

        # person_label과 class_id가 안 맞으면(사람이 깜빡하고 안 바꾼 경우 등) 촬영을
        # 시작하기 전에 바로 막는다 — 조용히 넘어가면 이미 한 번 데이터가 오염된 적 있음.
        try:
            validate_person_class_mapping(self.person_label, self.class_id)
        except ClassIdMismatchError as e:
            self.get_logger().fatal(str(e))
            raise
        self.get_logger().info(
            f'=== 지금부터 {self.person_label} (class_id={self.class_id})로 저장합니다 ==='
        )

        for split in ['train', 'val']:
            os.makedirs(f'{self.save_dir}/images/{split}', exist_ok=True)
            os.makedirs(f'{self.save_dir}/labels/{split}', exist_ok=True)

        self.bridge = CvBridge()
        self.face_cascade = cv2.CascadeClassifier(
            cv2.data.haarcascades + 'haarcascade_frontalface_default.xml'
        )

        self.last_saved_time = self.get_clock().now()
        # 재시작해도 0번부터 다시 찍지 않도록, 기존 파일 중 가장 큰 번호 다음부터 이어서 저장.
        # (0번부터 다시 찍으면 이전 세션 파일을 같은 이름으로 덮어써버림)
        self.saved_count = self._next_start_index()
        self.session_start_count = self.saved_count
        self.done = False

        self.sub = self.create_subscription(
            CompressedImage,
            '/camera/color/image_raw/compressed',
            self.image_callback,
            10
        )

        max_note = f', 이번 세션 목표 {self.max_images}장' if self.max_images else ''
        self.get_logger().info(
            f'[{self.person_label}] 자동 촬영 시작 (class_id={self.class_id}, '
            f'{self.saved_count}번부터 이어서 저장{max_note})'
        )

    def _next_start_index(self):
        max_idx = -1
        for split in ['train', 'val']:
            d = f'{self.save_dir}/images/{split}'
            if not os.path.isdir(d):
                continue
            for fname in os.listdir(d):
                if fname.startswith(f'{self.person_label}_') and fname.endswith('.jpg'):
                    stem = fname[len(self.person_label) + 1:-4]
                    if stem.isdigit():
                        max_idx = max(max_idx, int(stem))
        return max_idx + 1

    def image_callback(self, msg):
        now = self.get_clock().now()
        elapsed = (now - self.last_saved_time).nanoseconds / 1e9
        if elapsed < self.interval:
            return

        image = self.bridge.compressed_imgmsg_to_cv2(msg, 'bgr8')
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        faces = self.face_cascade.detectMultiScale(
            gray, scaleFactor=1.1, minNeighbors=6, minSize=(25, 25)
        )

        # 얼굴이 정확히 1개일 때만 사용
        if len(faces) != 1:
            return

        x, y, w, h = faces[0]
        img_h, img_w = image.shape[:2]
        face_ratio = (w * h) / (img_w * img_h)

        # 너무 멀거나(작음) 너무 가까우면(큼) 제외
        if not (0.003 <= face_ratio <= 0.6):
            return

        # 블러 체크
        blur_score = cv2.Laplacian(gray, cv2.CV_64F).var()
        if blur_score < 80:   # 값이 낮을수록 흐린 이미지, 환경 보고 조정
            return

        # ── 저장 ──
        split = 'val' if random.random() < self.val_ratio else 'train'
        filename = f'{self.person_label}_{self.saved_count:04d}'

        img_path = f'{self.save_dir}/images/{split}/{filename}.jpg'
        cv2.imwrite(img_path, image)

        # YOLO 라벨 자동 생성 (정규화된 x_center, y_center, w, h)
        x_center = (x + w / 2) / img_w
        y_center = (y + h / 2) / img_h
        norm_w = w / img_w
        norm_h = h / img_h

        label_path = f'{self.save_dir}/labels/{split}/{filename}.txt'
        with open(label_path, 'w') as f:
            f.write(f'{self.class_id} {x_center:.6f} {y_center:.6f} {norm_w:.6f} {norm_h:.6f}\n')

        self.saved_count += 1
        self.last_saved_time = now
        session_saved = self.saved_count - self.session_start_count
        self.get_logger().info(
            f'저장됨: {filename} ({split}) — 이번 세션 {session_saved}장 (전체 누적 {self.saved_count}장)'
        )

        if self.max_images and session_saved >= self.max_images:
            self.get_logger().info(f'목표 {self.max_images}장 도달 — 자동 종료합니다.')
            self.done = True


def main(args=None):
    rclpy.init(args=args)
    node = AutoFaceCaptureNode()
    try:
        while rclpy.ok() and not node.done:
            rclpy.spin_once(node, timeout_sec=0.5)
    except KeyboardInterrupt:
        pass
    node.destroy_node()
    if rclpy.ok():
        rclpy.shutdown()


if __name__ == '__main__':
    main()