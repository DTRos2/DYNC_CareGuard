#!/usr/bin/env python3
"""careguard_turtlebot_agent/perception/face_capture_node.py

R-06(터틀봇) 카메라로 대상자(target) 얼굴 사진을 자동 촬영해 모은다.
R-07(LIMO)의 careguard_perception/auto_face_capture_node.py와 같은 목적이지만
그대로 재사용하지 않는 이유:
  - 카메라 토픽이 다르다(R-06: /camera/image_raw/compressed, R-07: /camera/color/
    image_raw/compressed) — 카메라 기종(imx219 vs orbbec)도 달라서, 화질/크롭 방식
    차이가 임베딩 정확도에 영향을 줄 수 있다는 게 R-07 쪽에서 이미 확인된 문제라
    사진을 아예 R-06 카메라로 새로 찍어야 한다.
  - R-06은 대상자 1명만 등록하면 되는 문제라(나머지는 전부 침입자), R-07의
    person_A/B/C/D class_id 매핑·YOLO 라벨(.txt) 생성 로직이 필요 없다 — 그건
    커스텀 얼굴 검출기(YOLO)를 처음부터 학습시키기 위한 것이었는데, 여기서는
    검출기를 새로 학습하지 않고(얼굴 검출 자체는 신원과 무관해 기존 것을 재사용
    가능) 신원 확인용 임베딩 재료(얼굴 크롭 이미지)만 있으면 되기 때문이다.
  - 그래서 이 노드는 바운딩박스 라벨을 저장하지 않고, 검출된 얼굴 영역을 곧바로
    크롭해서 저장한다 — build_target_centroid.py가 그 크롭을 그대로 읽어 임베딩을
    뽑는다.

별도 ROS2 패키지로 안 만들고 careguard_turtlebot_agent 안의 perception/ 폴더로 둔
이유: R-06에는 이미 이 패키지 하나만 빌드/배포되고 있어서(deploy_agent.sh, systemd
서비스가 전부 careguard_turtlebot_agent 기준), 새 컬콘 패키지를 추가하면 배포 스크립트·
systemd 유닛까지 같이 늘려야 해서 배보다 배꼽이 커진다. cv_bridge도 새로 의존성에
추가하지 않고, uplink_node.py의 _on_camera_frame()과 똑같이 cv2.imdecode로 직접
디코딩한다(이미 검증된 방식 재사용).

사용법 (R-06에서, ROS2 환경 소싱 후):
    ros2 run careguard_turtlebot_agent face_capture_node --ros-args \\
        -p save_dir:=/home/yeyu/careguard_data/target_face_dataset \\
        -p max_images:=40

카메라가 상하반전 마운트된 로봇이면(uplink_node.py와 동일 조건) 환경변수
CAREGUARD_CAMERA_FLIP_VERTICAL=1을 켜서 실행해야 한다 — 안 그러면 Haar cascade가
뒤집힌 얼굴을 정면 얼굴로 인식하지 못해 검출률이 크게 떨어진다.

화면 확인: R-07의 face_identify_test_node.py와 같은 이유(SSH/NoMachine 환경에서
cv2.imshow가 안 뜨거나 검은 창만 뜨는 경우가 실측 확인됨)로 cv2.imshow를 쓰지 않고,
비압축 sensor_msgs/Image를 'face_capture/annotated' 토픽으로 매 프레임 발행한다.
로봇에서 X11 포워딩(ssh -X)으로 rqt를 띄우거나, 로컬 머신에서 rqt_image_view가
R-06과 같은 ROS2 네트워크에 도달할 수 있으면: rqt 실행 후 Plugins -> Visualization ->
Image View에서 'face_capture/annotated' 토픽을 선택하면 지금 카메라에 뭐가 잡히고
있는지(초록 박스=저장 조건 충족, 빨간 박스=검출은 됐지만 크기/블러 등 조건 미달)를
실시간으로 볼 수 있다.
"""
import os

import cv2
import numpy as np
import rclpy
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage, Image

# R-06(imx219 라즈베리파이캠) 토픽 — R-07의 /camera/color/image_raw/compressed와 다름.
CAMERA_TOPIC = '/camera/image_raw/compressed'
# uplink_node.py와 같은 환경변수 — R-05/R-06은 카메라가 물리적으로 뒤집혀 마운트돼 있다.
CAMERA_FLIP_VERTICAL = os.environ.get('CAREGUARD_CAMERA_FLIP_VERTICAL', '0') == '1'

# apt python3-opencv(시스템 패키지, R-06에 이미 깔려있던 것)는 pip opencv-python과
# 달리 cv2.data 서브모듈(하르 캐스케이드 XML을 패키지 안에 동봉)이 없다 — XML 자체는
# 시스템 어딘가(보통 /usr/share/opencv4/haarcascades/)에 별도로 설치돼 있다. pip판
# opencv-python은 numpy 2.x/버전 충돌 문제가 있어 시스템판으로 되돌렸으므로(2026-09-16
# 실제로 겪음: 커스텀 컴파일 ABI 불일치로 cv2 자체가 깨짐), cv2.data가 없는 것까지
# 감안해서 여러 경로를 순서대로 찾는다.
_CASCADE_CANDIDATES = [
    '/usr/share/opencv4/haarcascades/',
    '/usr/share/opencv/haarcascades/',
    '/usr/local/share/opencv4/haarcascades/',
]


def _find_haarcascade_path():
    try:
        _CASCADE_CANDIDATES.insert(0, cv2.data.haarcascades)
    except AttributeError:
        pass
    for d in _CASCADE_CANDIDATES:
        p = os.path.join(d, 'haarcascade_frontalface_default.xml')
        if os.path.isfile(p):
            return p
    raise FileNotFoundError(
        "haarcascade_frontalface_default.xml을 못 찾았습니다. "
        "'sudo apt install opencv-data'로 설치하거나 -p cascade_path:=<xml경로>로 직접 지정하세요."
    )


class FaceCaptureNode(Node):
    def __init__(self):
        super().__init__('face_capture_node')

        self.declare_parameter('person_label', 'target')
        self.declare_parameter('save_dir', os.path.expanduser('~/careguard_data/target_face_dataset'))
        self.declare_parameter('capture_interval', 0.7)
        self.declare_parameter('crop_margin', 0.25)
        # R-07(다른 카메라, 실제 얼굴 기준)에서 80을 썼는데 R-06 + "사진을 다시 촬영"
        # 조합에선 절대값이 다르게 나온다(2026-09-16 실측: 정상적으로 보이는 샷도 26
        # 근처). 카메라/피사체 조합마다 다시 잡아야 하는 값이라 파라미터로 뺐다.
        self.declare_parameter('blur_threshold', 15.0)
        # 촬영 대상자는 화면을 못 보므로, 목표 장수를 넣어두면 노드가 알아서 멈춘다.
        # 0이면 무제한(Ctrl+C로 직접 종료).
        self.declare_parameter('max_images', 40)
        # 빈 문자열이면 _find_haarcascade_path()로 자동 탐색.
        self.declare_parameter('cascade_path', '')

        self.person_label = self.get_parameter('person_label').value
        self.save_dir = self.get_parameter('save_dir').value
        self.interval = self.get_parameter('capture_interval').value
        self.crop_margin = self.get_parameter('crop_margin').value
        self.blur_threshold = self.get_parameter('blur_threshold').value
        self.max_images = self.get_parameter('max_images').value
        cascade_path = self.get_parameter('cascade_path').value or _find_haarcascade_path()

        os.makedirs(self.save_dir, exist_ok=True)

        self.face_cascade = cv2.CascadeClassifier(cascade_path)
        if self.face_cascade.empty():
            raise RuntimeError(f"cascade 로딩 실패(파일이 있어도 형식이 이상함): {cascade_path}")
        self.get_logger().info(f"cascade 로딩: {cascade_path}")

        self.last_saved_time = self.get_clock().now()
        # 재시작해도 0번부터 다시 찍지 않도록, 기존 파일 중 가장 큰 번호 다음부터 이어서 저장.
        self.saved_count = self._next_start_index()
        self.session_start_count = self.saved_count
        self.done = False

        self.sub = self.create_subscription(
            CompressedImage, CAMERA_TOPIC, self.image_callback, qos_profile_sensor_data
        )
        self.annotated_pub = self.create_publisher(Image, 'face_capture/annotated', 10)

        self.get_logger().info(
            f"[{self.person_label}] 자동 촬영 시작 (topic={CAMERA_TOPIC}, "
            f"flip_vertical={CAMERA_FLIP_VERTICAL}, {self.saved_count}번부터 이어서 저장, "
            f"목표 {self.max_images or '무제한'}장)"
        )

    def _next_start_index(self):
        max_idx = -1
        if not os.path.isdir(self.save_dir):
            return 0
        prefix = f'{self.person_label}_'
        for fname in os.listdir(self.save_dir):
            if fname.startswith(prefix) and fname.endswith('.jpg'):
                stem = fname[len(prefix):-4]
                if stem.isdigit():
                    max_idx = max(max_idx, int(stem))
        return max_idx + 1

    def image_callback(self, msg: CompressedImage):
        if not msg.data:
            return

        image = cv2.imdecode(np.frombuffer(bytes(msg.data), dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            return
        if CAMERA_FLIP_VERTICAL:
            image = cv2.flip(image, 0)

        # rqt로 실시간 확인할 수 있게, 저장 여부와 무관하게 매 프레임 검출+주석은
        # 항상 수행하고 발행한다 — 저장 자체만 아래에서 interval/품질 기준으로 걸러낸다.
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        faces = self.face_cascade.detectMultiScale(
            gray, scaleFactor=1.1, minNeighbors=6, minSize=(25, 25)
        )

        status = self._decide(image, gray, faces)
        self._publish_annotated(image, faces, status)

        # rqt 없이 지금 왜 안 찍히는지 바로 알 수 있게, 매 프레임 상태를 터미널에
        # 그대로 찍는다(2026-09-16: rqt 화면 확인이 원격 X11/DDS 문제로 계속 막혀서
        # 훨씬 간단한 이 방법으로 대체함).
        self.get_logger().info(f'상태: {status}')

        if status != 'ok':
            return

        now = self.get_clock().now()
        elapsed = (now - self.last_saved_time).nanoseconds / 1e9
        if elapsed < self.interval:
            return

        x, y, w, h = faces[0]
        crop = self._crop_with_margin(image, (x, y, w, h))

        filename = f'{self.person_label}_{self.saved_count:04d}.jpg'
        cv2.imwrite(os.path.join(self.save_dir, filename), crop)

        self.saved_count += 1
        self.last_saved_time = now
        session_saved = self.saved_count - self.session_start_count
        self.get_logger().info(
            f'저장됨: {filename} — 이번 세션 {session_saved}장 (전체 누적 {self.saved_count}장)'
        )

        if self.max_images and session_saved >= self.max_images:
            self.get_logger().info(f'목표 {self.max_images}장 도달 — 자동 종료합니다.')
            self.done = True

    def _decide(self, image, gray, faces):
        """지금 프레임이 '저장 가능(ok)'한지, 아니라면 어떤 이유로 걸렸는지를
        rqt 화면 텍스트로 바로 보여주기 위한 판정. 저장 여부 로직과 완전히 같은
        기준(개수/크기비율/블러)을 그대로 쓴다 — 여기서 갈리면 실제로도 안 찍힌다."""
        if len(faces) != 1:
            return f'얼굴 {len(faces)}개 검출 (1개여야 함)'

        x, y, w, h = faces[0]
        img_h, img_w = image.shape[:2]
        face_ratio = (w * h) / (img_w * img_h)
        if not (0.003 <= face_ratio <= 0.6):
            return f'크기 부적합 (ratio={face_ratio:.3f})'

        blur_score = cv2.Laplacian(gray, cv2.CV_64F).var()
        if blur_score < self.blur_threshold:
            return f'블러 (score={blur_score:.0f} < {self.blur_threshold:.0f})'

        return 'ok'

    def _publish_annotated(self, image, faces, status):
        try:
            self._publish_annotated_impl(image, faces, status)
        except Exception:
            # ROS 로깅 레벨/설정에 따라 실행기(executor) 안에서 조용히 씹히는
            # 예외가 있어서(2026-09-16 R-06에서 원인 추적 중 실제로 헷갈렸던 부분),
            # get_logger()가 아니라 print로 무조건 터미널에 보이게 한다.
            import traceback
            print('[face_capture_node] annotated 발행 실패:')
            traceback.print_exc()

    def _publish_annotated_impl(self, image, faces, status):
        vis = image.copy()
        color = (0, 200, 0) if status == 'ok' else (0, 0, 255)
        for (x, y, w, h) in faces:
            cv2.rectangle(vis, (x, y), (x + w, y + h), color, 2)
        session_saved = self.saved_count - self.session_start_count
        label = 'OK - 저장 조건 충족' if status == 'ok' else status
        cv2.putText(vis, label, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2)
        cv2.putText(
            vis, f'{session_saved}/{self.max_images or "?"}',
            (10, 60), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2,
        )

        msg = Image()
        msg.header.stamp = self.get_clock().now().to_msg()
        # vis.shape는 numpy int(예: numpy.int64)라, ROS2 메시지 필드(uint32)에 그대로
        # 넣으면 rclpy의 타입 체크에 걸려 publish()가 예외를 던지며 조용히 실패한다
        # (2026-09-16 R-06 실측: 토픽은 등록되는데 'ros2 topic hz'에 아무것도 안 잡힘).
        # 반드시 파이썬 기본 int로 변환해서 넣어야 한다.
        msg.height, msg.width = int(vis.shape[0]), int(vis.shape[1])
        msg.encoding = 'bgr8'
        msg.is_bigendian = 0
        msg.step = int(vis.shape[1] * 3)
        msg.data = vis.tobytes()
        self.annotated_pub.publish(msg)

    def _crop_with_margin(self, image, box):
        x, y, w, h = box
        img_h, img_w = image.shape[:2]
        mx, my = int(w * self.crop_margin), int(h * self.crop_margin)
        x0, y0 = max(0, x - mx), max(0, y - my)
        x1, y1 = min(img_w, x + w + mx), min(img_h, y + h + my)
        return image[y0:y1, x0:x1]


def main(args=None):
    rclpy.init(args=args)
    node = FaceCaptureNode()
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
