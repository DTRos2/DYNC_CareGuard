#!/usr/bin/env python3
import os
import random
import cv2
import numpy as np

import rclpy
from rclpy.node import Node
from sensor_msgs.msg import CompressedImage
from cv_bridge import CvBridge


class AutoFaceCaptureNode(Node):
    def __init__(self):
        super().__init__('auto_face_capture_node')

        # ── 세션마다 바꿔줄 파라미터 ──
        self.declare_parameter('person_label', 'person_A')   # 이번 세션에서 찍을 사람
        self.declare_parameter('class_id', 0)                 # person_A=0, B=1, C=2, D=3
        self.declare_parameter('save_dir', '/home/wego/careguard_data/face_dataset/')
        self.declare_parameter('capture_interval', 0.7)       # 저장 간격(초)
        self.declare_parameter('val_ratio', 0.2)

        self.person_label = self.get_parameter('person_label').value
        self.class_id = self.get_parameter('class_id').value
        self.save_dir = self.get_parameter('save_dir').value
        self.interval = self.get_parameter('capture_interval').value
        self.val_ratio = self.get_parameter('val_ratio').value

        for split in ['train', 'val']:
            os.makedirs(f'{self.save_dir}/images/{split}', exist_ok=True)
            os.makedirs(f'{self.save_dir}/labels/{split}', exist_ok=True)

        self.bridge = CvBridge()
        self.face_cascade = cv2.CascadeClassifier(
            cv2.data.haarcascades + 'haarcascade_frontalface_default.xml'
        )

        self.last_saved_time = self.get_clock().now()
        self.saved_count = 0

        self.sub = self.create_subscription(
            CompressedImage,
            '/camera/color/image_raw/compressed',
            self.image_callback,
            10
        )

        self.get_logger().info(
            f'[{self.person_label}] 자동 촬영 시작 (class_id={self.class_id})'
        )

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
        self.get_logger().info(f'저장됨: {filename} ({split}) — 총 {self.saved_count}장')


def main(args=None):
    rclpy.init(args=args)
    node = AutoFaceCaptureNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    node.destroy_node()
    rclpy.shutdown()


if __name__ == '__main__':
    main()