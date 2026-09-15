#!/usr/bin/env python3
"""테스트/디버깅용 ROS2 노드 — 카메라 실시간 영상에 얼굴 검출 박스 + 신원
(person_A/person_B/unknown) 라벨 + 유사도 점수를 같이 띄운다. 지금까지 만든 파이프라인
(YOLO 얼굴 검출 -> frozen FaceNet -> adapter -> centroid 비교) 전체를 눈으로 확인하기
위한 용도.

⚠️ 스코프: 테스트/디버깅 전용입니다.
  - 프로덕션 순찰 로직(정지, 알림 등)에 절대 연결하지 마세요.
  - 멀티프레임 다수결(sliding window) + hysteresis로 안정화합니다 (아래 참고).
    프레임 간 얼굴 매칭은 bbox 중심 거리 기반의 단순한 트래킹이며, 재식별(ReID)
    수준의 정식 트래커는 아닙니다.

멀티프레임 안정화 방식 (window_size/enter_threshold/exit_threshold ROS 파라미터로 조정 가능):
  - 얼굴마다(bbox 중심 거리로 프레임 간 매칭) 최근 window_size개의 프레임별 raw 판정
    (person_A/person_B/unknown)을 deque에 보관 — 꽉 차면 오래된 것부터 자동으로 밀려남.
    "한 번 확정되면 고정"이 아니라 매 프레임 이 윈도우 전체를 다시 집계함(재평가 지속).
  - Hysteresis: 현재 unknown 상태에서 특정 인물로 "진입"하려면 윈도우 내 그 인물
    비율이 enter_threshold(기본 70%) 이상이어야 함. 이미 그 인물로 확정된 상태에서는
    비율이 exit_threshold(기본 40%) 이하로 떨어져야만 unknown으로 "이탈"함(진입/이탈
    기준을 다르게 둬서 경계선 근처 미세한 흔들림만 흡수). 단, 다른 특정 인물이 즉시
    enter_threshold 이상으로 지배적이면(진짜 다른 사람 등장) 곧바로 그 인물로 전환됨.
  - 오인식 한 프레임이 끼어도 윈도우의 나머지가 정상이면 다수결에 묻혀 무시되고,
    반대로 오인식이 계속되면(지속적인 증거) exit_threshold 아래로 내려가며 정상적으로
    unknown 전환됨 — 어느 쪽으로도 영구 고착되지 않음.

⚠️ 알려진 한계 (기동 시 로그로도 출력됩니다):
  이 신원 판단은 unknown_1/2 단 두 명(32장)만으로 negative 학습된 adapter.pt(v2)를
  씁니다. 완전히 새로운(학습에 안 쓰인) 낯선 사람에 대한 open-set 거부 성능은
  검증되지 않았습니다 — face_embedding_adapter_finetune.py의 "중요한 한계" 참고.

⚠️ 2026-09-15 진단에서 확인된, 구조적으로 남아있는 crop 방식 차이 (코드로 완전히
  없앨 수 없음 — 참고만 할 것):
  학습/진단 파이프라인(face_embedding_diagnostic.py 등)은 **사람이 검수한 라벨의
  ground-truth bbox**로 크롭하는 반면, 이 실시간 노드는 **YOLO 검출 박스**로 크롭한다
  (실시간에는 ground truth가 없으므로 불가피함). YOLO 얼굴 검출기(face_detect_v4_burstsafe,
  mAP50=0.951)가 완벽하지 않아서 박스 위치/크기가 라벨과 약간 다를 수 있고, 정렬(landmark
  alignment) 없이 crop만 하는 파이프라인 특성상 이 미세한 차이가 임베딩에 꽤 크게
  반영될 수 있다 — 학습 때 unknown_2->person_B in-sample 거리(threshold=0.40 기준
  전부 0.73 이상, 오수용 0%)와 실시간에서 관측되는 값이 크게 다르다면 이 박스 출처
  차이가 유력한 원인이다. margin 공식(25%)과 160x160 리사이즈 알고리즘(PIL 기본
  BICUBIC)은 코드로 정확히 맞춰뒀다(아래 embed_crop() 참고) — 남은 차이는 detection
  박스 자체의 정확도뿐이다.

파이프라인:
  1. camera_topic(CompressedImage) 구독
  2. YOLO(face_detect_v4_burstsafe/best.pt)로 얼굴 검출
  3. 각 검출 박스를 크롭 -> frozen FaceNet -> adapter(v2)로 512차원 임베딩 추출
  4. person_centroids.pt에 저장된 person_A/B centroid와 코사인 거리 비교
  5. identity_threshold 이하로 가까우면 그 이름, 아니면 "unknown"
  6. OpenCV 창에 bbox + 라벨(이름) + 거리 점수를 그려서 표시

사전 준비물 (build_person_centroids.py로 만든 뒤 로봇에 scp):
  - YOLO 얼굴 검출 가중치 (face_detect_v4_burstsafe/weights/best.pt)
  - adapter.pt (v2 — adapter_v1_original_backup.pt 아님, unknown negative 포함 재학습본)
  - person_centroids.pt (build_person_centroids.py 출력)

화면 표시는 두 가지를 같이 시도한다:
  - cv2.imshow (show_window 파라미터, 기본 True) — plain SSH에는 DISPLAY가 없어서 안 뜨고,
    NoMachine에서도 Jetson OpenCV의 GUI 백엔드가 안 먹혀 검은 창만 뜨는 경우가 실측으로
    확인됨 (frame/YOLO 진단 로그는 정상인데 imshow만 검게 나옴).
  - annotated_image 토픽(sensor_msgs/Image, 압축 안 함 — image_transport 플러그인을 아예
    안 거쳐서 rqt의 theora 플러그인 버그도 회피) 발행 — **이게 실제로 동작 확인된 방법**.
    rqt 실행 후 Plugins -> Visualization -> Image View에서 토픽 드롭다운에 뜨는
    `/face_identify_test/annotated`를 선택하면 박스+라벨이 그려진 화면이 보인다.

사용법 (로봇에서, ROS2 환경 소싱 + DISPLAY 있는 세션(NoMachine 등)에서):
    ros2 run careguard_perception face_identify_test_node --ros-args \\
        -p yolo_weights:=/home/wego/careguard_data/runs/face_detect_v4_burstsafe/weights/best.pt \\
        -p adapter_path:=/home/wego/careguard_data/runs/face_embedding_adapter/adapter.pt \\
        -p centroids_path:=/home/wego/careguard_data/runs/face_embedding_adapter/person_centroids.pt \\
        -p window_size:=15 -p enter_threshold:=0.70 -p exit_threshold:=0.40
(모든 파라미터에 기본값이 있어서, 파일들이 기본 경로에 있으면 인자 없이 그냥
`ros2 run careguard_perception face_identify_test_node`만 실행해도 됩니다.
window_size/enter_threshold/exit_threshold/track_match_max_dist/track_max_missed_frames로
멀티프레임 안정화 동작을 재빌드 없이 조정할 수 있습니다.)

raw 유사도 점수 캡처 (2026-09-15 추가, 진단③용):
  기본적으로 매 프레임 매 검출의 raw_label/stable_label/dist_person_A/dist_person_B가
  `~/careguard_data/face_identify_test_raw_scores.csv`에 누적 기록됩니다
  (log_raw_scores:=false로 끌 수 있고, raw_score_log_path로 경로 변경 가능).
  unknown_2가 person_B로 뜨는 순간을 재현한 뒤 이 CSV에서 그 구간의 dist_person_B
  값을 뽑아 학습 때 in-sample 검증 수치(같은 threshold=0.40 기준, unknown_2->B 최소
  거리 0.73)와 비교하면 됨 — compute_unknown2_vs_B_centroid.py(dev-machine
  scratchpad) 참고.
"""
import csv
import sys
import time
from collections import Counter, deque
from pathlib import Path

import cv2
import numpy as np
from PIL import Image as PILImage
import rclpy
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage, Image
from cv_bridge import CvBridge

import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).resolve().parent))
from config import CAMERA_TOPIC  # noqa: E402  (재사용 — 새로 안 짬)
from face_embedding_adapter_model import ResidualAdapter  # noqa: E402  (재사용)

# ── identity 판단 threshold: 코사인 거리가 이 값 이하면 등록된 사람으로 인정, 넘으면
# unknown. 나중에 조정하기 쉽게 상수로 뺴둠(기본값) — 실행 중엔 identity_threshold
# ROS 파라미터로도 덮어쓸 수 있음(재빌드 없이 조정 가능).
DEFAULT_IDENTITY_THRESHOLD = 0.40

DEFAULT_YOLO_WEIGHTS = str(Path.home() / "careguard_data/runs/face_detect_v4_burstsafe/weights/best.pt")
DEFAULT_ADAPTER_PATH = str(Path.home() / "careguard_data/runs/face_embedding_adapter/adapter.pt")
DEFAULT_CENTROIDS_PATH = str(Path.home() / "careguard_data/runs/face_embedding_adapter/person_centroids.pt")

DECIDING_LABEL = "판정중"  # 콜드스타트: window가 아직 덜 찬 트랙의 상태(사람도, unknown도 아님)
LOW_CONF_LABEL = "검출불안정"  # YOLO confidence가 threshold 미만이라 식별 파이프라인에 안 넘긴 프레임

BOX_COLOR_BGR = {
    "person_A": (60, 180, 75),
    "person_B": (66, 133, 244),
    "unknown": (60, 60, 220),
    DECIDING_LABEL: (0, 215, 255),   # 주황 — 아직 판정 보류 중
    LOW_CONF_LABEL: (128, 128, 128),  # 회색 — 이번 프레임 검출 자체를 신뢰 안 함
}
EMBED_SIZE = 160
CROP_MARGIN = 0.25

# ── 멀티프레임 sliding-window 다수결 + hysteresis 기본값. ROS 파라미터로 런타임에
# 덮어쓸 수 있음(재빌드 없이 조정 가능) — identity_threshold와 동일한 방침.
DEFAULT_WINDOW_SIZE = 15
DEFAULT_ENTER_THRESHOLD = 0.70
DEFAULT_EXIT_THRESHOLD = 0.40
# 얼굴 트래킹(프레임 간 매칭)용 — 정식 트래커가 아니라 bbox 중심 거리 기반의 단순
# 매칭이라 값이 크게 정교하지 않아도 됨(테스트/디버깅 도구 스코프).
DEFAULT_TRACK_MATCH_MAX_DIST = 80.0  # px
DEFAULT_TRACK_MAX_MISSED_FRAMES = 10  # 이 프레임 수만큼 연속으로 안 잡히면 트랙 제거

# ── 2026-09-15 완화 로직 ①: sliding window 콜드스타트 — window가 이 비율만큼 안
# 채워지면 A/B/unknown 중 하나로 확정하지 않고 DECIDING_LABEL로만 표시한다.
# enter_threshold(기본 0.70)와 같은 비율을 기본값으로 써서 기존 다수결 기준과 맞춤
# (예: window_size=10이면 최소 7개는 쌓여야 판정 시작).
DEFAULT_MIN_WINDOW_FILL_RATIO = DEFAULT_ENTER_THRESHOLD

# ── 완화 로직 ②: YOLO confidence 하한선 — 이 값 미만인 검출은 임베딩/거리비교
# 자체를 안 하고 LOW_CONF_LABEL로만 표시(사람이 화면에 막 들어오는 순간의 불완전한
# 검출이 신원 판정에 아예 안 들어가게 막음).
DEFAULT_MIN_YOLO_CONFIDENCE = 0.5


def crop_with_margin(image_bgr, box, margin=CROP_MARGIN):
    x1, y1, x2, y2 = box
    w, h = x2 - x1, y2 - y1
    x1 = max(0, int(x1 - w * margin / 2))
    y1 = max(0, int(y1 - h * margin / 2))
    x2 = min(image_bgr.shape[1], int(x2 + w * margin / 2))
    y2 = min(image_bgr.shape[0], int(y2 + h * margin / 2))
    return image_bgr[y1:y2, x1:x2]


def _center_dist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def compute_stable_label(window, current_state, enter_threshold, exit_threshold):
    """Sliding-window 다수결 + hysteresis로 안정화된 라벨을 계산.

    매 프레임 호출 시 window(최근 N개 raw 판정이 들어있는 deque) 전체를 처음부터 다시
    집계한다 — 한 번 결정되면 고정되는 방식이 아니라 계속 재평가됨(deque가 꽉 차면
    오래된 프레임은 자동으로 밀려나므로 "최근 N개"가 항상 유지됨).

    - current_state가 "unknown"일 때: 특정 인물로 "진입"하려면 그 인물 비율이
      enter_threshold 이상이어야 함(예: 70%).
    - current_state가 특정 인물일 때: 비율이 exit_threshold 이하로 떨어져야만
      "unknown"으로 이탈함(예: 40%). 진입/이탈 기준을 다르게 둬서 두 기준 사이
      구간에서는 이전 상태를 유지 — 경계선 근처 미세한 흔들림(오인식 한두 프레임)이
      상태를 뒤집지 못하게 흡수하는 hysteresis 역할.
    - 단, 현재 인물이 아닌 다른 특정 인물이 즉시 enter_threshold 이상으로 지배적이면
      (진짜 다른 사람이 나타난 경우) hysteresis를 거치지 않고 곧바로 그 인물로 전환.

    Returns: (stable_label, 그 라벨의 윈도우 내 비율)
    """
    if not window:
        return "unknown", 0.0

    total = len(window)
    counts = Counter(window)
    best_label, best_count = counts.most_common(1)[0]
    best_frac = best_count / total

    if current_state == "unknown":
        if best_label != "unknown" and best_frac >= enter_threshold:
            return best_label, best_frac
        return "unknown", counts.get("unknown", 0) / total

    if best_label != current_state and best_label != "unknown" and best_frac >= enter_threshold:
        return best_label, best_frac

    current_frac = counts.get(current_state, 0) / total
    if current_frac <= exit_threshold:
        return "unknown", counts.get("unknown", 0) / total

    return current_state, current_frac


class FaceTrack:
    """프레임 간 같은 얼굴을 잇기 위한 최소한의 상태 — 정식 트래커가 아니라
    bbox 중심 거리로 매칭하는 단순 구현(테스트/디버깅 도구 스코프에 맞춤)."""

    __slots__ = ("center", "window", "stable_label", "stable_frac", "missed_frames")

    def __init__(self, center, window_size):
        self.center = center
        self.window = deque(maxlen=window_size)
        self.stable_label = DECIDING_LABEL  # 완화① — 갓 생긴 트랙은 아직 아무 판정도 안 함
        self.stable_frac = 0.0
        self.missed_frames = 0


class FaceIdentifyTestNode(Node):
    def __init__(self):
        super().__init__("face_identify_test_node")

        self.declare_parameter("camera_topic", CAMERA_TOPIC)
        self.declare_parameter("yolo_weights", DEFAULT_YOLO_WEIGHTS)
        self.declare_parameter("adapter_path", DEFAULT_ADAPTER_PATH)
        self.declare_parameter("centroids_path", DEFAULT_CENTROIDS_PATH)
        self.declare_parameter("identity_threshold", DEFAULT_IDENTITY_THRESHOLD)
        self.declare_parameter("show_window", True)
        self.declare_parameter("window_size", DEFAULT_WINDOW_SIZE)
        self.declare_parameter("enter_threshold", DEFAULT_ENTER_THRESHOLD)
        self.declare_parameter("exit_threshold", DEFAULT_EXIT_THRESHOLD)
        self.declare_parameter("track_match_max_dist", DEFAULT_TRACK_MATCH_MAX_DIST)
        self.declare_parameter("track_max_missed_frames", DEFAULT_TRACK_MAX_MISSED_FRAMES)
        self.declare_parameter("log_raw_scores", True)
        self.declare_parameter(
            "raw_score_log_path",
            str(Path.home() / "careguard_data/face_identify_test_raw_scores.csv"),
        )
        self.declare_parameter("min_window_fill_ratio", DEFAULT_MIN_WINDOW_FILL_RATIO)
        self.declare_parameter("min_yolo_confidence", DEFAULT_MIN_YOLO_CONFIDENCE)

        camera_topic = self.get_parameter("camera_topic").value
        yolo_weights = self.get_parameter("yolo_weights").value
        adapter_path = self.get_parameter("adapter_path").value
        centroids_path = self.get_parameter("centroids_path").value
        self.threshold = self.get_parameter("identity_threshold").value
        self.show_window = self.get_parameter("show_window").value
        self.window_size = int(self.get_parameter("window_size").value)
        self.enter_threshold = float(self.get_parameter("enter_threshold").value)
        self.exit_threshold = float(self.get_parameter("exit_threshold").value)
        self.track_match_max_dist = float(self.get_parameter("track_match_max_dist").value)
        self.track_max_missed_frames = int(self.get_parameter("track_max_missed_frames").value)
        self.tracks = {}
        self._next_track_id = 0

        self.min_window_fill_ratio = float(self.get_parameter("min_window_fill_ratio").value)
        self.min_window_fill = max(1, round(self.window_size * self.min_window_fill_ratio))
        self.min_yolo_confidence = float(self.get_parameter("min_yolo_confidence").value)

        self.log_raw_scores = self.get_parameter("log_raw_scores").value
        self.raw_score_log_path = Path(self.get_parameter("raw_score_log_path").value).expanduser()

        self.get_logger().warn(
            "⚠️  [테스트/디버깅 전용] 이 identity 판단은 unknown_1/2 단 두 명(32장)만으로 "
            "negative 학습된 adapter를 씁니다 — 완전히 새로운 낯선 사람에 대한 open-set 거부 "
            "성능은 검증되지 않았습니다. 프로덕션 순찰 로직에 연결하지 마세요."
        )
        if self.log_raw_scores:
            self.raw_score_log_path.parent.mkdir(parents=True, exist_ok=True)
            is_new_file = not self.raw_score_log_path.exists()
            self._raw_score_file = open(self.raw_score_log_path, "a", newline="")
            self._raw_score_writer = csv.writer(self._raw_score_file)
            if is_new_file:
                self._raw_score_writer.writerow([
                    "timestamp", "frame_count", "track_id",
                    "raw_label", "stable_label", "stable_frac",
                    "dist_person_A", "dist_person_B", "identity_threshold",
                ])
            self.get_logger().info(
                f"raw 유사도 점수를 매 프레임 CSV로 기록합니다: {self.raw_score_log_path} "
                "(진단③ — 학습 때 in-sample 검증 수치와 실시간 점수를 나중에 비교하기 위함)"
            )

        self.device = "cuda" if torch.cuda.is_available() else "cpu"
        self.get_logger().info(f"device={self.device}")

        from ultralytics import YOLO  # 지연 import — 노드 파일 로드만으로 무거운 라이브러리 안 끌고오게
        self.get_logger().info(f"YOLO 얼굴 검출 가중치 로딩: {yolo_weights}")
        self.yolo = YOLO(yolo_weights)

        from facenet_pytorch import InceptionResnetV1, fixed_image_standardization
        self.get_logger().info("FaceNet(vggface2, frozen) 로딩...")
        self.embedder = InceptionResnetV1(pretrained="vggface2").eval().to(self.device)
        for p in self.embedder.parameters():
            p.requires_grad_(False)
        self.standardize = fixed_image_standardization

        ckpt = torch.load(adapter_path, map_location=self.device)
        self.adapter = ResidualAdapter(dim=ckpt["dim"], bottleneck=ckpt["bottleneck"]).to(self.device)
        self.adapter.load_state_dict(ckpt["state_dict"])
        self.adapter.eval()
        self.get_logger().info(f"adapter 로딩 완료: {adapter_path}")

        cent = torch.load(centroids_path, map_location=self.device)
        self.centroids = {name: cent[name].to(self.device) for name in ("person_A", "person_B")}
        self.get_logger().info(
            f"centroid 로딩 완료: person_A(n={cent.get('n_A', '?')}) "
            f"person_B(n={cent.get('n_B', '?')})  identity_threshold={self.threshold}"
        )
        self.get_logger().info(
            f"멀티프레임 안정화: window_size={self.window_size} "
            f"enter_threshold={self.enter_threshold} exit_threshold={self.exit_threshold} "
            f"track_match_max_dist={self.track_match_max_dist}px "
            f"track_max_missed_frames={self.track_max_missed_frames}"
        )
        self.get_logger().info(
            f"[완화①] 콜드스타트 게이팅: window가 {self.min_window_fill}/{self.window_size}개 "
            f"(min_window_fill_ratio={self.min_window_fill_ratio}) 이상 찰 때까지 "
            f"'{DECIDING_LABEL}'로만 표시 — 다수결 로직 자체는 그대로."
        )
        self.get_logger().info(
            f"[완화②] YOLO confidence 하한선: min_yolo_confidence={self.min_yolo_confidence} "
            f"미만 검출은 임베딩/거리비교 없이 '{LOW_CONF_LABEL}'로만 표시."
        )

        self.bridge = CvBridge()
        self.frame_count = 0
        # 카메라 드라이버가 qos_profile_sensor_data(best-effort)로 발행함 — video_recorder.py와
        # 동일하게 맞춰야 구독이 실제로 붙는다. 기본(reliable) QoS로는 DDS가 매칭을 거부해서
        # 콜백이 아예 안 불릴 수 있다.
        self.sub = self.create_subscription(
            CompressedImage, camera_topic, self.on_frame, qos_profile_sensor_data
        )
        self.get_logger().info(f"구독 시작: {camera_topic} (qos=sensor_data/best_effort)")

        self.annotated_pub = self.create_publisher(Image, "face_identify_test/annotated", 10)
        self.get_logger().info(
            "박스+라벨 그린 화면을 'face_identify_test/annotated' 토픽(비압축 Image)으로도 "
            "발행합니다 — rqt Image View에서 이 토픽을 선택해서 보세요 (cv2.imshow가 이 "
            "환경에서 안 뜨는 경우가 확인되어, 이쪽이 실제로 동작하는 방법입니다)."
        )
        if self.show_window:
            self.get_logger().info(
                "cv2.imshow 창을 띄웁니다 — DISPLAY가 없는 plain SSH 세션에서는 창이 뜨지 "
                "않습니다(rviz2와 동일한 제약). NoMachine 등 GUI 세션에서 실행하세요."
            )

    def embed_crop(self, face_bgr) -> torch.Tensor:
        # ── face_embedding_diagnostic.py(학습/진단 파이프라인)와 리사이즈 알고리즘을
        # 맞추기 위해 cv2.resize(기본 INTER_LINEAR/bilinear) 대신 PIL Image.resize를
        # 쓴다 — PIL은 인자 생략 시 기본이 BICUBIC(Pillow>=9.1)이라 diagnostic 스크립트의
        # crop.resize((EMBED_SIZE, EMBED_SIZE))와 동일한 보간 알고리즘이 됨. 2026-09-15
        # 진단에서 발견: 이전엔 두 파이프라인의 리사이즈 알고리즘이 서로 달랐음(불일치
        # 원인 중 하나, crop bbox 출처 차이와는 별개).
        face_rgb = cv2.cvtColor(face_bgr, cv2.COLOR_BGR2RGB)
        face_rgb = np.asarray(PILImage.fromarray(face_rgb).resize((EMBED_SIZE, EMBED_SIZE)))
        t = torch.from_numpy(face_rgb).permute(2, 0, 1).float().unsqueeze(0).to(self.device)
        t = self.standardize(t)
        with torch.no_grad():
            emb = self.embedder(t)
            emb = F.normalize(self.adapter(emb), p=2, dim=1)
        return emb[0]

    def identify(self, emb: torch.Tensor):
        """centroid까지 코사인 거리가 가장 가까운 사람을 찾고, threshold를 넘으면 unknown.

        Returns: (best_name_or_unknown, best_dist, dists) — dists는
        {"person_A": dist, "person_B": dist} 전체(로그/진단용, best만으론 부족).
        """
        dists = {name: float(1.0 - torch.dot(emb, centroid)) for name, centroid in self.centroids.items()}
        best_name, best_dist = min(dists.items(), key=lambda kv: kv[1])
        if best_dist > self.threshold:
            return "unknown", best_dist, dists
        return best_name, best_dist, dists

    def _update_tracks(self, detections):
        """detections(이번 프레임의 raw 판정들)를 기존 track에 bbox 중심 거리로
        매칭하고, 매칭된 track의 sliding window에 raw_label을 넣어 매 프레임
        다시 집계한다. detections의 각 dict에 "track"을 채워 넣어 반환한다.

        완화②(YOLO confidence 하한선)로 인해 raw_label이 None인 검출(det["low_conf"]
        True)은 이번 프레임의 위치 추적(중심좌표 갱신, missed_frames 리셋)에는
        반영하되, window에는 아무것도 넣지 않고 track.stable_label/stable_frac도
        건드리지 않는다 — 신뢰 못 할 프레임 하나 때문에 기존에 쌓인 판정이 흔들리거나
        트랙이 끊기지 않게 하기 위함."""
        unmatched_track_ids = set(self.tracks.keys())

        for det in detections:
            best_id, best_dist = None, None
            for track_id in unmatched_track_ids:
                d = _center_dist(det["center"], self.tracks[track_id].center)
                if d <= self.track_match_max_dist and (best_dist is None or d < best_dist):
                    best_id, best_dist = track_id, d

            if best_id is not None:
                track = self.tracks[best_id]
                unmatched_track_ids.discard(best_id)
            else:
                track = FaceTrack(det["center"], self.window_size)
                self.tracks[self._next_track_id] = track
                self._next_track_id += 1

            track.center = det["center"]
            track.missed_frames = 0

            if det["raw_label"] is not None:
                track.window.append(det["raw_label"])
                # ── 완화① 콜드스타트 게이팅: window가 min_window_fill개 미만으로 쌓였으면
                # 기존 다수결(compute_stable_label)을 아예 호출하지 않고 DECIDING_LABEL로
                # 고정한다 — 기존 다수결/hysteresis 로직 자체는 변경하지 않음, 그 앞에
                # 게이트만 하나 추가하는 것.
                if len(track.window) < self.min_window_fill:
                    track.stable_label = DECIDING_LABEL
                    track.stable_frac = len(track.window) / self.window_size
                else:
                    # DECIDING_LABEL에서 막 벗어나는 시점엔 compute_stable_label이 모르는
                    # 상태이므로 "unknown"에서 시작한 것처럼 취급(진입 기준 enter_threshold부터
                    # 정상 적용됨 — 곧바로 A/B로 확정되려면 그때도 enter_threshold를 넘어야 함).
                    current_state = track.stable_label if track.stable_label != DECIDING_LABEL else "unknown"
                    track.stable_label, track.stable_frac = compute_stable_label(
                        track.window, current_state, self.enter_threshold, self.exit_threshold
                    )

            det["track"] = track

        # 이번 프레임에 매칭 안 된 track은 "안 보임"이지 "unknown으로 확인됨"이 아니라서
        # window에는 아무것도 넣지 않고 missed_frames만 늘림 — 오래 안 보이면 트랙 제거.
        for track_id in unmatched_track_ids:
            self.tracks[track_id].missed_frames += 1

        self.tracks = {
            tid: t for tid, t in self.tracks.items() if t.missed_frames <= self.track_max_missed_frames
        }

    def on_frame(self, msg: CompressedImage):
        image = self.bridge.compressed_imgmsg_to_cv2(msg, "bgr8")

        self.frame_count += 1
        if self.frame_count % 30 == 1:
            # 화면이 안 보이는 세션(로그만 보이는 상황)에서도 실제로 프레임이 들어오고
            # 디코딩이 되는지 확인할 수 있게 — GUI 문제인지 파이프라인 문제인지 구분용.
            self.get_logger().info(
                f"[진단] frame#{self.frame_count} shape={image.shape} "
                f"mean_pixel={image.mean():.1f} (0에 가까우면 디코딩/노출 문제 의심)"
            )

        results = self.yolo.predict(image, verbose=False)[0]
        if self.frame_count % 30 == 1:
            self.get_logger().info(f"[진단] YOLO 검출 수: {len(results.boxes)}")

        detections = []
        for box in results.boxes:
            x1, y1, x2, y2 = [float(v) for v in box.xyxy[0]]
            conf = float(box.conf[0])
            center = ((x1 + x2) / 2.0, (y1 + y2) / 2.0)

            # ── 완화② YOLO confidence 하한선: 미달 검출은 크롭/임베딩/거리비교를 아예
            # 안 하고 건너뛴다 (연산 낭비도 줄고, 신뢹 못 할 검출이 신원 판정에 안 들어감).
            if conf < self.min_yolo_confidence:
                detections.append({
                    "box": (x1, y1, x2, y2), "center": center, "conf": conf,
                    "raw_label": None, "dist": None, "dists": None, "low_conf": True,
                })
                continue

            face = crop_with_margin(image, (x1, y1, x2, y2))
            if face.size == 0:
                continue

            emb = self.embed_crop(face)
            raw_label, dist, dists = self.identify(emb)
            detections.append({
                "box": (x1, y1, x2, y2), "center": center, "conf": conf,
                "raw_label": raw_label, "dist": dist, "dists": dists, "low_conf": False,
            })

        self._update_tracks(detections)

        if self.log_raw_scores:
            now = time.time()
            for det in detections:
                # low_conf 프레임은 임베딩/거리 계산 자체를 안 했으므로(dists=None) 기록할
                # raw score가 없다 — 건너뜀 (진단③ CSV는 실제 계산된 거리만 남긴다).
                if det["dists"] is None:
                    continue
                track = det["track"]
                self._raw_score_writer.writerow([
                    f"{now:.3f}", self.frame_count, id(track),
                    det["raw_label"], track.stable_label, f"{track.stable_frac:.4f}",
                    f"{det['dists']['person_A']:.4f}", f"{det['dists']['person_B']:.4f}",
                    self.threshold,
                ])
            self._raw_score_file.flush()

        for det in detections:
            x1, y1, x2, y2 = det["box"]
            track = det["track"]
            conf_s = f"yolo_conf={det['conf']:.2f}"

            if det["low_conf"]:
                # 완화②: 이번 프레임은 confidence 미달이라 식별 파이프라인에 안 넘겼음 —
                # track에 쌓여 있던 기존 판정과 무관하게 이번 프레임만 회색으로 표시.
                color = BOX_COLOR_BGR[LOW_CONF_LABEL]
                label = f"{LOW_CONF_LABEL} ({conf_s} < {self.min_yolo_confidence:.2f})"
            elif track.stable_label == DECIDING_LABEL:
                # 완화①: window가 아직 min_window_fill만큼 안 쌓여서 확정 보류 중.
                color = BOX_COLOR_BGR[DECIDING_LABEL]
                label = (f"{DECIDING_LABEL}... ({len(track.window)}/{self.min_window_fill}) "
                         f"(raw:{det['raw_label']} {conf_s})")
            else:
                stable_label, stable_frac = track.stable_label, track.stable_frac
                color = BOX_COLOR_BGR.get(stable_label, BOX_COLOR_BGR["unknown"])
                dist_s = f"{det['dist']:.2f}" if det["dist"] is not None else "n/a"
                # 화면엔 안정화된(sliding-window+hysteresis) 라벨을 주로 보여주고, 괄호 안에
                # 이번 프레임 raw 판정(원래 방식)도 같이 보여줘서 둘이 언제 갈리는지 눈으로
                # 비교할 수 있게 함.
                label = f"{stable_label} {stable_frac * 100:.0f}% (raw:{det['raw_label']} {dist_s} {conf_s})"

            cv2.rectangle(image, (int(x1), int(y1)), (int(x2), int(y2)), color, 2)
            (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 2)
            ty = max(th + 4, int(y1) - 6)
            cv2.rectangle(image, (int(x1), ty - th - 6), (int(x1) + tw + 6, ty + 2), color, -1)
            cv2.putText(image, label, (int(x1) + 3, ty - 3),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)

        out_msg = self.bridge.cv2_to_imgmsg(image, encoding="bgr8")
        out_msg.header = msg.header
        self.annotated_pub.publish(out_msg)

        if self.show_window:
            cv2.imshow("face_identify_test (q로 종료)", image)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                self.get_logger().info("q 입력 — 노드 종료")
                rclpy.shutdown()


def main(args=None):
    rclpy.init(args=args)
    node = FaceIdentifyTestNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    if getattr(node, "log_raw_scores", False):
        node._raw_score_file.close()
    cv2.destroyAllWindows()
    node.destroy_node()
    if rclpy.ok():
        rclpy.shutdown()


if __name__ == "__main__":
    main()
