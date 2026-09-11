#!/usr/bin/env python3
import os
import random
import cv2
import numpy as np

import rclpy
from rclpy.node import Node
from sensor_msgs.msg import CompressedImage
from cv_bridge import CvBridge


class CaptureComplete(Exception):
    """max_images에 도달해서 정상 종료할 때 쓰는 신호용 예외."""
    pass


class AutoFaceCaptureNode(Node):
    def __init__(self):
        super().__init__('auto_face_capture_node')

        # ── 세션마다 바꿔줄 파라미터 ──
        self.declare_parameter('person_label', 'person_A')   # 이번 세션에서 찍을 사람
        self.declare_parameter('class_id', 0)                 # person_A=0, B=1, C=2, D=3
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
            raise CaptureComplete()


def main(args=None):
    rclpy.init(args=args)
    node = AutoFaceCaptureNode()
    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, CaptureComplete):
        pass
    node.destroy_node()
    rclpy.shutdown()


if __name__ == '__main__':
    main()