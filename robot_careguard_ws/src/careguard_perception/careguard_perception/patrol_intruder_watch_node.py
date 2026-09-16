#!/usr/bin/env python3
"""careguard_perception/patrol_intruder_watch_node.py

순찰(patrol) 주행 중에만 동작하는 프로덕션 대상자/침입자 판별 노드.

face_identify_test_node.py(테스트/디버깅 전용, 파일 상단에 "프로덕션 순찰 로직에
절대 연결하지 마세요"라고 명시되어 있음)는 그대로 두고, 그 노드가 이미 검증한 핵심
판정 로직(얼굴 검출 crop, sliding-window 다수결+hysteresis, 사람 전신 게이트)만
face_pipeline_common.py를 통해 공유한다 — 두 노드가 같은 얼굴을 다르게 판단하는
drift를 막기 위함(모듈 docstring 참고).

정지→접근→재판별 (2026-09-15 추가):
카메라에 처음 잡히는 순간(먼 거리, 작은 crop)의 판정은 화질이 떨어져 오판 위험이
있다. 그래서 sliding-window로 "raw 판정"이 한 번 안정화(DECIDING_LABEL을 벗어남)돼도
그걸 바로 최종 결과로 쓰지 않고, 우선 그 사람 쪽으로 로봇을 가깝게(기본 1.5m)
접근시킨 뒤 그 자리에서 다시 찍은 사진으로 재판별해서 그 결과를 최종으로 쓴다:

  1. 검출된 얼굴 bbox 중심을 CameraInfo 내부 파라미터로 각도(bearing)로 바꾸고,
     그 방향의 /scan(라이다) 거리를 tf2로 프레임을 맞춰 조회한다. 라이다가 그
     방향에 유효한 리턴이 없으면(문/유리 등) bbox 폭 기반 거리 추정치로 대체한다
     (_estimate_distance_and_bearing 참고).
  2. /amcl_pose로 알고 있는 로봇의 현재 map 좌표 + 위 거리/방향으로 접근 목표
     좌표를 계산해 APPROACH_REQUEST_TOPIC으로 요청한다. 이 요청은 서버를 거치지
     않고 patrol_task.py가 로컬에서 직접 처리한다(순찰을 완전히 취소하는 게
     아니라 "잠깐 다녀오는" 것 — careguard_mission/patrol_task.py 모듈 docstring
     참고).
  3. patrol_task.py가 접근 이동을 마치면 APPROACH_ARRIVED_TOPIC으로 알려온다.
     도착했으면 최대 APPROACH_VERIFY_TIMEOUT_SEC/APPROACH_VERIFY_MAX_SAMPLES
     동안 새로 찍히는 프레임으로 다시 판정을 모아 다수결로 최종 라벨을 정한다
     (그 사이 아무도 안 잡히면 "회피"로 보고 원거리 판정을 그대로 최종으로 쓴다 —
     verified_at_close_range=False로 표시).
  4. 최종 라벨이 등록된 recipient_id면 "인식됨"(서보 트리거는 2단계 TODO),
     "unknown"이면 INTRUDER_STATUS_TOPIC으로 침입자 신고를 발행한다.
  5. APPROACH_RELEASE_TOPIC으로 "이제 가도 됨"을 patrol_task.py에 돌려주면 순찰이
     재개된다.

같은 사람이 접근을 피해 계속 새 트랙으로 잡혀도 로봇이 끝없이 쫓아다니지 않도록,
짧은 시간 안의 접근 시도 횟수를 전역으로 제한한다(기본 5분에 최대 2회,
approach_max_attempts/approach_window_sec). 제한에 걸리거나 거리 정보를 아예 못
구하면 접근 없이 원거리 판정을 그대로 최종으로 쓴다(enable_approach:=false로
접근 자체를 끄면 1단계와 동일하게 항상 이 경로로 동작).

person_centroids.pt는 recipient_id를 키로 쓴다(build_person_centroids.py 참고,
person_A/B 같은 세션 라벨이 아니라 실제 recipient.id) — 그리고 --robot-code로
빌드하면 "이 로봇에 배정된 대상자"만 포함되므로, 런타임에 DB 조회 없이 "이
recipient_id가 파일에 있으면 = 내 담당 대상자"로 바로 판별할 수 있다.

zero-shot 이중 확인 게이트(face_identify_test_node.py의 실험C)는 이식하지 않았다 —
build_zero_shot_centroids.py도 recipient_id 키로 다시 만들어야 하는 추가 작업이라
범위를 넘어간다. adapter 단독 판정만 쓰므로 open-set 거부 성능은 테스트 노드보다
보수적이지 않다는 점을 감안할 것.

카메라-라이다 방향 융합의 단순화(중요, 실기 검증 필요): 카메라의 수평 bearing을
base_link 기준으로 옮길 때, tf2로 구한 camera_frame->base_link 회전의 yaw 성분만
쓰고 pitch/roll은 무시한다(카메라가 아래로 기울어져 있어도 좌우 각도는 크게 안
바뀐다는 근사). 라이다도 같은 방식으로 base_link 기준 yaw만 맞춘다. 완전한 3D
벡터 변환이 아니라 근사치이므로, 실기에서 접근 목표가 계속 사람을 비껴간다면
이 근사부터 의심할 것.

순찰 중에만 동작해야 하므로 video_recorder.py와 같은 PATROL_RECORDING_TOPIC
(순찰 시작/종료) 신호를 구독해서 순찰 중이 아니면 프레임을 아예 처리하지 않는다
(require_patrol_active:=false로 끄면 수동 테스트 가능).

사전 준비물:
  - YOLO 얼굴 검출 가중치(face_detect_v4_burstsafe/best.pt) — face_identify_test_node와 동일
  - adapter.pt(v2) — 동일
  - person_centroids.pt — build_person_centroids.py --robot-code <이 로봇 코드>로 생성한 것
  - (완화③ 사용 시) person_yolo_weights(yolov8n.pt, COCO 사전학습)
  - (접근 기능 사용 시) /camera/color/camera_info, /scan, /amcl_pose가 모두 발행되고 있어야 함

사용법:
    ros2 run careguard_perception patrol_intruder_watch_node --ros-args \\
        -p yolo_weights:=/home/wego/careguard_data/runs/face_detect_v4_burstsafe/weights/best.pt \\
        -p adapter_path:=/home/wego/careguard_data/runs/face_embedding_adapter/adapter.pt \\
        -p centroids_path:=/home/wego/careguard_data/runs/face_embedding_adapter/person_centroids.pt

빌드/실행 미검증.
"""
import base64
import json
import math
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
from geometry_msgs.msg import PoseWithCovarianceStamped
from sensor_msgs.msg import CameraInfo, CompressedImage, LaserScan
from std_msgs.msg import Bool, String
from cv_bridge import CvBridge
import tf2_ros

import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).resolve().parent))
from config import (  # noqa: E402
    APPROACH_ARRIVED_TOPIC,
    APPROACH_ATTEMPTS_WINDOW_SEC,
    APPROACH_MAX_ATTEMPTS,
    APPROACH_RELEASE_TOPIC,
    APPROACH_REQUEST_TOPIC,
    APPROACH_TARGET_DISTANCE_M,
    APPROACH_VERIFY_MAX_SAMPLES,
    APPROACH_VERIFY_TIMEOUT_SEC,
    AVERAGE_FACE_WIDTH_M,
    CAMERA_TOPIC,
    INTRUDER_STATUS_TOPIC,
    PATROL_RECORDING_TOPIC,
    PERSON_DETECTED_TOPIC,
    PERSON_RECOGNIZED_TOPIC,
    TRIGGER_TOPIC,
    VOICE_TOPIC,
)
from face_embedding_adapter_model import ResidualAdapter  # noqa: E402  (재사용)
from face_pipeline_common import (  # noqa: E402
    COCO_PERSON_CLASS_ID,
    DEFAULT_ENTER_THRESHOLD,
    DEFAULT_EXIT_THRESHOLD,
    DEFAULT_IDENTITY_THRESHOLD,
    DEFAULT_MIN_WINDOW_FILL_RATIO,
    DEFAULT_MIN_YOLO_CONFIDENCE,
    DEFAULT_PERSON_GATE_INTERVAL,
    DEFAULT_PERSON_GATE_MODE,
    DEFAULT_PERSON_GATE_OVERLAP_RATIO,
    DEFAULT_PERSON_MIN_CONFIDENCE,
    DEFAULT_TRACK_MATCH_MAX_DIST,
    DEFAULT_TRACK_MAX_MISSED_FRAMES,
    DEFAULT_WINDOW_SIZE,
    DECIDING_LABEL,
    EMBED_SIZE,
    FaceTrackManager,
    crop_with_margin,
    face_in_any_person_box,
)

DEFAULT_YOLO_WEIGHTS = str(Path.home() / "careguard_data/runs/face_detect_v4_burstsafe/weights/best.pt")
DEFAULT_ADAPTER_PATH = str(Path.home() / "careguard_data/runs/face_embedding_adapter/adapter.pt")
DEFAULT_CENTROIDS_PATH = str(Path.home() / "careguard_data/runs/face_embedding_adapter/person_centroids.pt")
DEFAULT_PERSON_YOLO_WEIGHTS = str(Path.home() / "careguard_data/runs/yolov8n_coco/yolov8n.pt")

DEFAULT_CAMERA_INFO_TOPIC = "/camera/color/camera_info"
DEFAULT_SCAN_TOPIC = "/scan"

# 접근 목표까지 이만큼(m) 미만이면 새 Nav2 목표를 보낼 가치가 없다고 보고(이미 거의
# 도착한 거리) 접근 없이 지금 이 프레임 판정을 그대로 "근접 판정"으로 취급한다.
MIN_APPROACH_TRIP_M = 0.3


def _yaw_from_quaternion(q) -> float:
    # uplink_node.py의 같은 이름 함수와 동일(패키지 간 공유 없이 중복 정의하는 기존 방식).
    siny_cosp = 2.0 * (q.w * q.z + q.x * q.y)
    cosy_cosp = 1.0 - 2.0 * (q.y * q.y + q.z * q.z)
    return math.atan2(siny_cosp, cosy_cosp)


class PatrolIntruderWatchNode(Node):
    def __init__(self):
        super().__init__("patrol_intruder_watch_node")

        self.declare_parameter("camera_topic", CAMERA_TOPIC)
        self.declare_parameter("camera_info_topic", DEFAULT_CAMERA_INFO_TOPIC)
        self.declare_parameter("scan_topic", DEFAULT_SCAN_TOPIC)
        self.declare_parameter("yolo_weights", DEFAULT_YOLO_WEIGHTS)
        self.declare_parameter("adapter_path", DEFAULT_ADAPTER_PATH)
        self.declare_parameter("centroids_path", DEFAULT_CENTROIDS_PATH)
        self.declare_parameter("identity_threshold", DEFAULT_IDENTITY_THRESHOLD)
        self.declare_parameter("person_yolo_weights", DEFAULT_PERSON_YOLO_WEIGHTS)
        self.declare_parameter("person_min_confidence", DEFAULT_PERSON_MIN_CONFIDENCE)
        self.declare_parameter("person_gate_mode", DEFAULT_PERSON_GATE_MODE)
        self.declare_parameter("person_gate_overlap_ratio", DEFAULT_PERSON_GATE_OVERLAP_RATIO)
        self.declare_parameter("person_gate_interval", DEFAULT_PERSON_GATE_INTERVAL)
        self.declare_parameter("enable_person_gate", True)
        self.declare_parameter("window_size", DEFAULT_WINDOW_SIZE)
        self.declare_parameter("enter_threshold", DEFAULT_ENTER_THRESHOLD)
        self.declare_parameter("exit_threshold", DEFAULT_EXIT_THRESHOLD)
        self.declare_parameter("track_match_max_dist", DEFAULT_TRACK_MATCH_MAX_DIST)
        self.declare_parameter("track_max_missed_frames", DEFAULT_TRACK_MAX_MISSED_FRAMES)
        self.declare_parameter("min_window_fill_ratio", DEFAULT_MIN_WINDOW_FILL_RATIO)
        self.declare_parameter("min_yolo_confidence", DEFAULT_MIN_YOLO_CONFIDENCE)
        # 정지→접근→재판별 관련 파라미터 — 기본값은 careguard_perception/config.py 참고.
        self.declare_parameter("enable_approach", True)
        self.declare_parameter("approach_target_distance_m", APPROACH_TARGET_DISTANCE_M)
        self.declare_parameter("approach_max_attempts", APPROACH_MAX_ATTEMPTS)
        self.declare_parameter("approach_window_sec", APPROACH_ATTEMPTS_WINDOW_SEC)
        self.declare_parameter("approach_verify_timeout_sec", APPROACH_VERIFY_TIMEOUT_SEC)
        self.declare_parameter("approach_verify_max_samples", APPROACH_VERIFY_MAX_SAMPLES)
        self.declare_parameter("average_face_width_m", AVERAGE_FACE_WIDTH_M)
        # 수동 테스트용 — 끄면 순찰 중이 아니어도 매 프레임 처리한다.
        self.declare_parameter("require_patrol_active", True)

        camera_topic = self.get_parameter("camera_topic").value
        camera_info_topic = self.get_parameter("camera_info_topic").value
        scan_topic = self.get_parameter("scan_topic").value
        yolo_weights = self.get_parameter("yolo_weights").value
        adapter_path = self.get_parameter("adapter_path").value
        centroids_path = self.get_parameter("centroids_path").value
        self.threshold = float(self.get_parameter("identity_threshold").value)

        person_yolo_weights = self.get_parameter("person_yolo_weights").value
        self.person_min_confidence = float(self.get_parameter("person_min_confidence").value)
        self.person_gate_mode = self.get_parameter("person_gate_mode").value
        self.person_gate_overlap_ratio = float(self.get_parameter("person_gate_overlap_ratio").value)
        self.person_gate_interval = max(1, int(self.get_parameter("person_gate_interval").value))
        self.enable_person_gate = self.get_parameter("enable_person_gate").value
        self._cached_person_boxes = []

        self.window_size = int(self.get_parameter("window_size").value)
        self.enter_threshold = float(self.get_parameter("enter_threshold").value)
        self.exit_threshold = float(self.get_parameter("exit_threshold").value)
        self.track_match_max_dist = float(self.get_parameter("track_match_max_dist").value)
        self.track_max_missed_frames = int(self.get_parameter("track_max_missed_frames").value)
        self.min_window_fill_ratio = float(self.get_parameter("min_window_fill_ratio").value)
        self.min_window_fill = max(1, round(self.window_size * self.min_window_fill_ratio))
        self.min_yolo_confidence = float(self.get_parameter("min_yolo_confidence").value)
        self._track_manager = FaceTrackManager(
            window_size=self.window_size, enter_threshold=self.enter_threshold,
            exit_threshold=self.exit_threshold, track_match_max_dist=self.track_match_max_dist,
            track_max_missed_frames=self.track_max_missed_frames, min_window_fill=self.min_window_fill,
        )
        # 트랙별 "마지막으로 확정 로그를 남긴 stable_label"(전환 감지용) — 트랙이
        # 사라지면 같이 정리한다.
        self._last_stable_label = {}

        self.enable_approach = self.get_parameter("enable_approach").value
        self.approach_target_distance = float(self.get_parameter("approach_target_distance_m").value)
        self.approach_max_attempts = int(self.get_parameter("approach_max_attempts").value)
        self.approach_window_sec = float(self.get_parameter("approach_window_sec").value)
        self.approach_verify_timeout_sec = float(self.get_parameter("approach_verify_timeout_sec").value)
        self.approach_verify_max_samples = int(self.get_parameter("approach_verify_max_samples").value)
        self.average_face_width_m = float(self.get_parameter("average_face_width_m").value)
        # 접근 시도 시각(monotonic) 기록 — approach_window_sec 안의 개수로 전역 rate-limit.
        self._approach_attempts = deque()
        # 지금 진행 중인 접근/재판별 상태(한 번에 하나만) — None이면 접근 중 아님.
        self._approach = None

        self.require_patrol_active = self.get_parameter("require_patrol_active").value
        self._patrol_active = not self.require_patrol_active
        self._current_run_id = None

        # 접근 목표 계산에 필요한 실시간 정보. 카메라/라이다는 각도만 필요하므로
        # base_link 기준 yaw 오프셋을 한 번만 tf2로 구해서 캐시한다(고정 장착이라
        # 매 프레임 다시 조회할 이유가 없음).
        self._camera_info = None
        self._latest_scan = None
        self._robot_pose = None  # (x, y, yaw) in map frame, /amcl_pose 기준
        self._camera_to_base_yaw = None
        self._laser_to_base_yaw = None
        self._tf_buffer = tf2_ros.Buffer()
        self._tf_listener = tf2_ros.TransformListener(self._tf_buffer, self)

        self.device = "cuda" if torch.cuda.is_available() else "cpu"
        self.get_logger().info(f"device={self.device}")

        from ultralytics import YOLO  # 지연 import — 노드 파일 로드만으로 무거운 라이브러리 안 끌고오게
        self.get_logger().info(f"YOLO 얼굴 검출 가중치 로딩: {yolo_weights}")
        self.yolo = YOLO(yolo_weights)

        if self.enable_person_gate:
            self.get_logger().info(f"[완화③] COCO 사전학습 person 검출 YOLO 로딩: {person_yolo_weights}")
            self.person_yolo = YOLO(person_yolo_weights)
        else:
            self.person_yolo = None
            self.get_logger().warn("사람 전신 게이트가 꺼져 있습니다 — 비교/테스트 용도가 아니면 켜두세요.")

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
        self.centroids = {rid: c.to(self.device) for rid, c in cent["centroids"].items()}
        self.recipient_names = cent.get("names", {})
        if not self.centroids:
            self.get_logger().warn(
                "centroid 파일에 등록된 대상자가 0명입니다 — 모든 얼굴이 unknown(침입자)로 "
                "판정됩니다. build_person_centroids.py --robot-code로 이 로봇 담당 대상자를 "
                "먼저 등록했는지 확인하세요."
            )
        else:
            names_s = ", ".join(f"{name}(id={rid})" for rid, name in self.recipient_names.items())
            self.get_logger().info(f"centroid 로딩 완료: {len(self.centroids)}명 — {names_s}")

        self.bridge = CvBridge()
        self.frame_count = 0
        # 침입자 확정 시점(_finalize_decision)에 캡처용으로 쓴다 — on_frame 콜백과
        # _finalize_decision 호출이 대부분 같은 프레임 처리 흐름 안에서 동기적으로
        # 이어지므로(접근 도착 후 재판별 타임아웃 경로만 약간의 시차가 있을 수 있음)
        # "직전 프레임"이면 증거 사진으로 충분하다.
        self._last_frame = None
        self.sub = self.create_subscription(
            CompressedImage, camera_topic, self.on_frame, qos_profile_sensor_data
        )
        self.create_subscription(String, PATROL_RECORDING_TOPIC, self._on_patrol_trigger, 10)
        self._intruder_pub = self.create_publisher(String, INTRUDER_STATUS_TOPIC, 10)
        # 2단계 서보모터 트리거 — careguard_hw 쪽 아두이노 브리지 노드 전용,
        # 로봇 로컬(서버로 안 올라감). PERSON_DETECTED_TOPIC 주석 참고.
        self._person_detected_pub = self.create_publisher(Bool, PERSON_DETECTED_TOPIC, 10)
        # 대상자 인식 캡처(증거 사진, care_event 없음) — 서버로 relay됨. PERSON_RECOGNIZED_TOPIC 참고.
        self._person_recognized_pub = self.create_publisher(String, PERSON_RECOGNIZED_TOPIC, 10)
        # 침입자 확정 순간 로봇이 그 자리에서 바로 음성 안내 + 영상 증거를 남기는 용도
        # (2026-09-16 추가) — 예전엔 서버에 care_event(INTRUDER)만 올라가고 로봇
        # 자신은 아무 반응이 없었다(음성도, 녹화도 안 됨). patrol_task.py가 이미 쓰는
        # 것과 같은 토픽들이라 서버 왕복 없이 바로 발행할 수 있다(medication_alert처럼
        # "오늘 몇 번째"를 서버 DB에서 세어야 하는 값이 아니라, 지금 이 순간 확정된
        # 사실 자체라 로봇이 판단을 즉시 알 수 있음).
        self._voice_pub = self.create_publisher(String, VOICE_TOPIC, 10)
        self._record_trigger_pub = self.create_publisher(String, TRIGGER_TOPIC, 10)

        if self.enable_approach:
            # camera_info/scan은 카메라 드라이버가 이미지와 함께 보통 같은 QoS(대개
            # best-effort)로 발행한다 — uplink_node.py가 /scan을 best-effort로 구독하는
            # 것과 같은 이유(실 로봇에서 기본(reliable) QoS로는 매칭이 안 된 전례가 있음).
            self.create_subscription(
                CameraInfo, camera_info_topic, self._on_camera_info, qos_profile_sensor_data
            )
            self.create_subscription(LaserScan, scan_topic, self._on_scan, qos_profile_sensor_data)
            self.create_subscription(PoseWithCovarianceStamped, "/amcl_pose", self._on_amcl_pose, 10)
            self._approach_request_pub = self.create_publisher(String, APPROACH_REQUEST_TOPIC, 10)
            self._approach_release_pub = self.create_publisher(String, APPROACH_RELEASE_TOPIC, 10)
            self.create_subscription(String, APPROACH_ARRIVED_TOPIC, self._on_approach_arrived, 10)
            self.get_logger().info(
                f"[접근] 활성화 — 목표거리={self.approach_target_distance}m "
                f"제한={self.approach_max_attempts}회/{self.approach_window_sec:.0f}초"
            )
        else:
            self.get_logger().warn("[접근] enable_approach=false — 원거리 1회 판정을 그대로 최종으로 씁니다.")

        self.get_logger().info(
            f"구독 시작: {camera_topic} — require_patrol_active={self.require_patrol_active} "
            f"(순찰 시작 신호는 {PATROL_RECORDING_TOPIC}에서 받음)"
        )

    # ---------- 순찰 시작/종료 게이팅 ----------

    def _on_patrol_trigger(self, msg: String):
        try:
            payload = json.loads(msg.data) if msg.data else {}
        except ValueError:
            self.get_logger().warn(f"순찰 녹화 트리거 JSON 파싱 실패, 무시합니다: {msg.data!r}")
            return

        action = payload.get("action")
        if action == "start":
            self._patrol_active = True
            self._current_run_id = payload.get("run_id")
            self.get_logger().info(f"순찰 시작 — 침입자 감지 활성화(run_id={self._current_run_id})")
        elif action == "stop":
            self._patrol_active = not self.require_patrol_active
            self._current_run_id = None
            self.get_logger().info("순찰 종료 — 침입자 감지 비활성화")
            # 순찰이 끝났으니 트랙/접근 상태도 다음 순찰에 영향 안 주게 정리한다.
            # patrol_task.py도 자기 쪽 _pending_approach를 같은 시점에 비운다 —
            # 이 노드가 응답을 기다리는 접근이 있었다면 그냥 고아로 버려둔다(다음
            # 프레임 처리부터 다시 정상 동작).
            self._track_manager.tracks = {}
            self._last_stable_label = {}
            self._approach = None

    # ---------- 식별 파이프라인 ----------

    def embed_crop(self, face_bgr):
        # face_identify_test_node.embed_crop()과 동일한 crop/resize 방식(PIL 기본
        # BICUBIC) — face_embedding_diagnostic.py 학습 파이프라인과도 정합이 맞다.
        face_rgb = cv2.cvtColor(face_bgr, cv2.COLOR_BGR2RGB)
        face_rgb = np.asarray(PILImage.fromarray(face_rgb).resize((EMBED_SIZE, EMBED_SIZE)))
        t = torch.from_numpy(face_rgb).permute(2, 0, 1).float().unsqueeze(0).to(self.device)
        t = self.standardize(t)
        with torch.no_grad():
            emb = self.embedder(t)
            adapted = F.normalize(self.adapter(emb), p=2, dim=1)
        return adapted[0]

    def identify(self, adapter_emb: torch.Tensor):
        """등록된 recipient centroid 전체와 비교해 가장 가까운 쪽을 고른다.
        Returns: (label, dist) — label은 recipient_id(str) 또는 "unknown"."""
        if not self.centroids:
            return "unknown", None
        dists = {rid: float(1.0 - torch.dot(adapter_emb, c)) for rid, c in self.centroids.items()}
        best_rid, best_dist = min(dists.items(), key=lambda kv: kv[1])
        label = best_rid if best_dist <= self.threshold else "unknown"
        return label, best_dist

    def _detect(self, image):
        """YOLO 얼굴 검출 + 완화②(confidence)/③(사람 전신 게이트) + 임베딩/신원판단까지
        한 프레임에 대해 전부 수행하고 detections 리스트를 반환한다(face_pipeline_common의
        FaceTrackManager.update()에 그대로 넘길 수 있는 dict 형태)."""
        results = self.yolo.predict(image, verbose=False)[0]

        person_boxes = []
        if self.enable_person_gate:
            if (self.frame_count - 1) % self.person_gate_interval == 0:
                person_results = self.person_yolo.predict(
                    image, classes=[COCO_PERSON_CLASS_ID], conf=self.person_min_confidence, verbose=False
                )[0]
                person_boxes = [tuple(float(v) for v in b.xyxy[0]) for b in person_results.boxes]
                self._cached_person_boxes = person_boxes
            else:
                person_boxes = self._cached_person_boxes

        detections = []
        for box in results.boxes:
            x1, y1, x2, y2 = [float(v) for v in box.xyxy[0]]
            conf = float(box.conf[0])
            center = ((x1 + x2) / 2.0, (y1 + y2) / 2.0)

            if conf < self.min_yolo_confidence:
                detections.append({"box": (x1, y1, x2, y2), "center": center, "conf": conf,
                                    "raw_label": None, "low_conf": True})
                continue

            if self.enable_person_gate and not face_in_any_person_box(
                (x1, y1, x2, y2), person_boxes, self.person_gate_mode, self.person_gate_overlap_ratio
            ):
                detections.append({"box": (x1, y1, x2, y2), "center": center, "conf": conf,
                                    "raw_label": None, "low_conf": False})
                continue

            face = crop_with_margin(image, (x1, y1, x2, y2))
            if face.size == 0:
                continue

            adapter_emb = self.embed_crop(face)
            raw_label, dist = self.identify(adapter_emb)
            detections.append({"box": (x1, y1, x2, y2), "center": center, "conf": conf,
                                "raw_label": raw_label, "low_conf": False, "dist": dist})

        return detections

    # ---------- 프레임 처리 ----------

    def on_frame(self, msg: CompressedImage):
        if not self._patrol_active:
            return

        image = self.bridge.compressed_imgmsg_to_cv2(msg, "bgr8")
        self._last_frame = image
        self.frame_count += 1
        detections = self._detect(image)

        if self._approach is not None:
            # 접근 이동 중이거나 도착 후 재판별 대기 중 — 평소 트랙 관리/새 접근
            # 트리거는 멈추고, "도착 후 재판별" 단계에서만 이 프레임을 표본으로 쓴다.
            if self._approach["phase"] == "verifying":
                self._collect_verify_sample(detections)
            return

        new_tracks = self._track_manager.update(detections)
        for new in new_tracks:
            self.get_logger().info(
                f"[신규 감지] track_id={new['track_id']} (frame#{self.frame_count}) "
                f"yolo_conf={new['conf']:.2f}"
            )

        self._handle_track_transitions(detections)
        self._prune_stale_track_state()

    def _handle_track_transitions(self, detections):
        for det in detections:
            track = det.get("track")
            if track is None or track.stable_label == DECIDING_LABEL:
                continue
            # 완화②/③으로 이번 프레임 자체를 건너뛴 det(raw_label=None)는 track의
            # 기존 stable_label을 안 건드리므로, 여기서도 그 det 기준으로는 아무 판단을
            # 안 내린다 — 다음에 raw_label이 있는 det가 왔을 때 처리된다.
            if det["raw_label"] is None:
                continue

            stable = track.stable_label
            previous = self._last_stable_label.get(track.track_id)
            if stable == previous:
                continue
            self._last_stable_label[track.track_id] = stable

            # 처음 안정화된 순간(상태 전환) — 바로 최종 확정하지 않고, 정확도를
            # 높이기 위해 우선 접근을 시도한다(모듈 docstring 참고).
            self._maybe_start_approach(track.track_id, stable, det)

    # ---------- 정지→접근→재판별 ----------

    def _maybe_start_approach(self, track_id, far_field_label, det):
        if not self.enable_approach:
            self._finalize_decision(track_id, far_field_label, verified=False, det=det)
            return

        distance, bearing, source = self._estimate_distance_and_bearing(det)
        if distance is None or bearing is None:
            self.get_logger().warn(
                f"[접근] track_id={track_id} — 거리/방향 추정 실패, 원거리 판정을 최종으로 사용"
            )
            self._finalize_decision(track_id, far_field_label, verified=False, det=det)
            return

        if distance <= self.approach_target_distance + MIN_APPROACH_TRIP_M:
            # 이미 목표 거리 근처(또는 더 가까움) — 접근할 필요 없이 지금 이 프레임
            # 자체가 이미 "근접" 판정이나 다름없다.
            self._finalize_decision(track_id, far_field_label, verified=True, det=det)
            return

        if not self._approach_rate_limit_ok():
            self.get_logger().warn(
                f"[접근 제한] 최근 {self.approach_window_sec:.0f}초 내 {self.approach_max_attempts}회 "
                "초과 — 접근 없이 원거리 판정을 최종으로 사용"
            )
            self._finalize_decision(track_id, far_field_label, verified=False, det=det)
            return

        target = self._compute_approach_target(distance, bearing)
        if target is None:
            self._finalize_decision(track_id, far_field_label, verified=False, det=det)
            return

        self._approach_attempts.append(time.monotonic())
        self._approach = {
            "track_id": track_id, "far_field_label": far_field_label, "phase": "moving",
        }
        tx, ty, tyaw = target
        payload = {"track_id": track_id, "x": tx, "y": ty, "yaw": tyaw}
        out = String()
        out.data = json.dumps(payload)
        self._approach_request_pub.publish(out)
        self.get_logger().info(
            f"[접근 요청] track_id={track_id} 목표=({tx:.2f},{ty:.2f}) "
            f"거리추정={distance:.2f}m(출처={source})"
        )

    def _approach_rate_limit_ok(self) -> bool:
        now = time.monotonic()
        while self._approach_attempts and now - self._approach_attempts[0] > self.approach_window_sec:
            self._approach_attempts.popleft()
        return len(self._approach_attempts) < self.approach_max_attempts

    def _on_amcl_pose(self, msg: PoseWithCovarianceStamped):
        yaw = _yaw_from_quaternion(msg.pose.pose.orientation)
        self._robot_pose = (msg.pose.pose.position.x, msg.pose.pose.position.y, yaw)

    def _on_camera_info(self, msg: CameraInfo):
        self._camera_info = msg

    def _on_scan(self, msg: LaserScan):
        self._latest_scan = msg

    def _lookup_yaw_offset(self, target_frame, source_frame):
        try:
            tf = self._tf_buffer.lookup_transform(target_frame, source_frame, rclpy.time.Time())
        except (tf2_ros.LookupException, tf2_ros.ConnectivityException, tf2_ros.ExtrapolationException):
            return None
        return _yaw_from_quaternion(tf.transform.rotation)

    def _estimate_distance_and_bearing(self, det):
        """det(bbox)로부터 (거리(m), base_link 기준 bearing(rad), 출처) 를 추정한다.
        실패하면 (None, None, None). 모듈 docstring의 "단순화" 참고 — yaw만 맞추는
        근사치다."""
        if self._camera_info is None or self._camera_info.k[0] <= 0:
            return None, None, None

        x1, _, x2, _ = det["box"]
        bbox_center_x = (x1 + x2) / 2.0
        bbox_width = x2 - x1
        fx = self._camera_info.k[0]
        cx = self._camera_info.k[2]
        bearing_cam = math.atan2(bbox_center_x - cx, fx)

        if self._camera_to_base_yaw is None:
            self._camera_to_base_yaw = self._lookup_yaw_offset("base_link", self._camera_info.header.frame_id)
            if self._camera_to_base_yaw is None:
                return None, None, None
        bearing_base = bearing_cam + self._camera_to_base_yaw

        lidar_dist = self._lidar_range_at_bearing(bearing_base)
        if lidar_dist is not None:
            return lidar_dist, bearing_base, "lidar"

        if bbox_width <= 0:
            return None, bearing_base, None
        bbox_dist = (self.average_face_width_m * fx) / bbox_width
        return bbox_dist, bearing_base, "bbox"

    def _lidar_range_at_bearing(self, bearing_base):
        scan = self._latest_scan
        if scan is None or scan.angle_increment == 0:
            return None
        if self._laser_to_base_yaw is None:
            self._laser_to_base_yaw = self._lookup_yaw_offset("base_link", scan.header.frame_id)
            if self._laser_to_base_yaw is None:
                return None
        bearing_scan = bearing_base - self._laser_to_base_yaw
        idx = round((bearing_scan - scan.angle_min) / scan.angle_increment)
        n = len(scan.ranges)
        candidates = []
        for offset in range(-2, 3):
            j = idx + offset
            if 0 <= j < n:
                r = scan.ranges[j]
                if math.isfinite(r) and scan.range_min <= r <= scan.range_max:
                    candidates.append(r)
        if not candidates:
            return None
        candidates.sort()
        return candidates[len(candidates) // 2]  # median

    def _compute_approach_target(self, distance, bearing_base):
        if self._robot_pose is None:
            return None
        rx, ry, ryaw = self._robot_pose
        trip = distance - self.approach_target_distance
        if trip < MIN_APPROACH_TRIP_M:
            return None
        target_bearing_map = ryaw + bearing_base
        tx = rx + trip * math.cos(target_bearing_map)
        ty = ry + trip * math.sin(target_bearing_map)
        return tx, ty, target_bearing_map

    def _on_approach_arrived(self, msg: String):
        if self._approach is None:
            return
        try:
            payload = json.loads(msg.data)
        except ValueError:
            return
        if payload.get("track_id") != self._approach["track_id"]:
            return

        if not payload.get("arrived"):
            self.get_logger().warn(
                f"[접근 실패] track_id={self._approach['track_id']} — 원거리 판정을 최종으로 사용"
            )
            self._finalize_decision(
                self._approach["track_id"], self._approach["far_field_label"], verified=False
            )
            self._approach = None
            return

        self.get_logger().info(f"[접근 도착] track_id={self._approach['track_id']} — 재판별 시작")
        self._approach["phase"] = "verifying"
        self._approach["arrived_at"] = time.monotonic()
        self._approach["close_detections"] = []

    def _collect_verify_sample(self, detections):
        approach = self._approach
        best = max(
            (d for d in detections if d.get("raw_label") is not None),
            key=lambda d: d["conf"], default=None,
        )
        if best is not None:
            approach["close_detections"].append((best["raw_label"], best.get("dist")))

        elapsed = time.monotonic() - approach["arrived_at"]
        enough_samples = len(approach["close_detections"]) >= self.approach_verify_max_samples
        timed_out = elapsed >= self.approach_verify_timeout_sec
        if not (enough_samples or timed_out):
            return

        if approach["close_detections"]:
            labels = [label for label, _dist in approach["close_detections"]]
            final_label = Counter(labels).most_common(1)[0][0]
            dists = [d for label, d in approach["close_detections"] if label == final_label and d is not None]
            final_dist = sum(dists) / len(dists) if dists else None
            self._finalize_decision(approach["track_id"], final_label, verified=True, dist=final_dist)
        else:
            self.get_logger().warn(
                f"[접근 재판별] track_id={approach['track_id']} — 근접 재확인 실패(회피 추정), "
                "원거리 판정을 최종으로 사용"
            )
            self._finalize_decision(approach["track_id"], approach["far_field_label"], verified=False)

        self._release_approach(approach["track_id"])
        self._approach = None

    def _release_approach(self, track_id):
        out = String()
        out.data = json.dumps({"track_id": track_id})
        self._approach_release_pub.publish(out)

    # ---------- 최종 판정 ----------

    def _finalize_decision(self, track_id, label, verified, det=None, dist=None):
        if dist is None and det is not None:
            dist = det.get("dist")

        if label in self.centroids:
            name = self.recipient_names.get(label, label)
            self.get_logger().info(
                f"[인식됨] track_id={track_id} -> {name}(recipient_id={label}) "
                f"— {'근접 재확인' if verified else '원거리 판정(재확인 없음)'}으로 확정"
            )
            # 2단계 서보모터 트리거 — 팀원 제공 스펙(person.png)대로 Bool true 한 번만
            # 쏘고 끝. 같은 사람이 계속 프레임에 잡히면 판정될 때마다(디바운스 없이)
            # 매번 다시 발행된다 — 서보 쪽에서 중복 수신에 안전하게 설계돼 있어야 한다
            # (예: 이미 열려있으면 다시 열어도 무해해야 함, 팀원 하드웨어 구현 책임).
            bool_msg = Bool()
            bool_msg.data = True
            self._person_detected_pub.publish(bool_msg)

            image_b64 = self._capture_face_image(det)
            payload = {
                "type": "person_recognized",
                "track_id": track_id,
                "recipient_id": label,
                "verified_at_close_range": verified,
                "nearest_dist": round(dist, 4) if dist is not None else None,
                "run_id": self._current_run_id,
            }
            if image_b64 is not None:
                payload["image_base64"] = image_b64
            out = String()
            out.data = json.dumps(payload)
            self._person_recognized_pub.publish(out)
            return

        payload = {
            "type": "intruder_detected",
            "track_id": track_id,
            "verified_at_close_range": verified,
            "nearest_dist": round(dist, 4) if dist is not None else None,
            "run_id": self._current_run_id,
        }
        image_b64 = self._capture_face_image(det)
        if image_b64 is not None:
            payload["image_base64"] = image_b64
        out = String()
        out.data = json.dumps(payload)
        self._intruder_pub.publish(out)
        self.get_logger().warn(
            f"[침입자 감지] track_id={track_id} verified={verified} — 등록 대상자와 불일치, "
            f"care_event 신고 발행{'' if image_b64 is not None else ' (사진 없음)'}"
        )

        # care_event(INTRUDER)가 emergency로 만들어지는 것과 같은 심각도로 그 자리에서
        # 바로 음성 안내 + 이벤트 클립 녹화를 켠다. care_event_id는 서버가 나중에
        # INSERT하면서 만드는 값이라 여기서는 모른다(video_recorder.py는 None이어도
        # 사이드카에만 영향 있을 뿐 녹화 자체는 정상 동작한다).
        voice_msg = String()
        voice_msg.data = json.dumps({'text': '침입자가 감지되었습니다.'})
        self._voice_pub.publish(voice_msg)

        trigger_msg = String()
        trigger_msg.data = json.dumps({
            'care_event_id': None, 'severity': 'emergency', 'reason': '침입자 감지',
            # downlink_node.py::_handle_medication_alert와 같은 이유 — 위에서 이미
            # 더 구체적인 문구를 안내했으므로 video_recorder.py의 일반 문구("긴급
            # 이벤트 발생! 영상 녹화를 시작합니다")는 겹쳐 나오지 않게 끈다.
            'skip_announce': True,
        })
        self._record_trigger_pub.publish(trigger_msg)

    def _capture_face_image(self, det):
        """대상자 인식/침입자 확정 순간의 얼굴 캡처를 JPEG+base64로 인코딩해 반환한다.

        WS/HTTP 왕복 없이 intruder_detected/person_recognized 메시지 자체에 이미지를
        실어 보낸다 — care_event(침입자 쪽만 해당)가 서버에서 만들어지기 전엔
        entity_id로 쓸 id를 아직 몰라서, 파일부터 올리고 나중에 연결하는 map_image
        방식(placeholder entity_id 재연결)을 쓰려면 레이스 컨디션을 감수해야 한다.
        대신 이 메시지 안에 이미지를 같이 실으면 서버가 (필요하면) id를 만든 "직후"
        바로 files 행을 만들 수 있다 — 재연결 단계 자체가 필요 없다.

        실패해도(프레임이 아직 없거나 인코딩 오류) None을 반환해 판정/신고 자체는
        사진 없이 계속 진행되게 한다 — 증거 사진은 있으면 좋은 것이지 판정을 막을
        이유는 아니다.
        """
        if self._last_frame is None:
            return None
        try:
            image = self._last_frame
            box = det.get("box") if det is not None else None
            crop = crop_with_margin(image, box) if box is not None else image
            if crop.size == 0:
                crop = image
            ok, buf = cv2.imencode(".jpg", crop, [cv2.IMWRITE_JPEG_QUALITY, 85])
            if not ok:
                return None
            return base64.b64encode(buf.tobytes()).decode("ascii")
        except Exception:
            self.get_logger().warn("[침입자 캡처] 이미지 인코딩 실패 — 사진 없이 신고만 진행합니다")
            return None

    def _prune_stale_track_state(self):
        live_ids = set(self._track_manager.tracks.keys())
        for tid in list(self._last_stable_label.keys()):
            if tid not in live_ids:
                del self._last_stable_label[tid]


def main(args=None):
    rclpy.init(args=args)
    node = PatrolIntruderWatchNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    node.destroy_node()
    if rclpy.ok():
        rclpy.shutdown()


if __name__ == "__main__":
    main()
