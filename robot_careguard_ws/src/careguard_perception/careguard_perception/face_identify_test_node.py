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

화면 표시 끄기(2026-09-15 추가, headless 순찰 대비): `enable_display:=false`면
bbox/라벨을 그리는 cv2.rectangle/putText 연산 자체를 건너뛰고(불필요한 연산
부담 제거), cv2.imshow/waitKey도 show_window 값과 무관하게 전부 안 뜬다.
annotated 토픽은 계속 발행되지만 그림 없는 원본 프레임이 나간다. 검출·신원
판단(adapter/zero-shot/사람전신 게이트)·raw score CSV·콘솔 로그는 이 값과
무관하게 항상 그대로 동작 — 순수하게 "화면에 그려서 보여주는" 부분만 분리한
것.

raw score CSV 크기 관리(2026-09-15 추가): raw_score_max_size_mb(기본
50, 0 이하면 끔) — CSV가 이 크기를 넘으면 매 프레임 기록 직후 자동으로
`_rotated_<timestamp>`를 붙여 보존하고 헤더부터 새로 시작한다(스키마 변경
시의 `_old_schema_<timestamp>` 아카이브와 같은 방식, 별도 이름으로 구분).
시간 기준 자동 삭제/압축 대신 크기 기준 롤오버를 골랐다 — 디스크를 못 채우게
막는 게 실제 운영 리스크의 핵심이고(며칠 켜놔도 특정 크기 이상은 절대 안
쌓임), 디렉터리 스캔·파일명 날짜 파싱·삭제 스케줄링이 필요한 시간 기준
방식보다 구현이 훨씬 단순하다 — 이미 있는 os.path.getsize() 체크 한 줄과
스키마-변경 시 쓰던 것과 동일한 rename 패턴만 재사용하면 됨.

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

등장 초반 vs 안정화 이후 비교 (2026-09-15 추가):
  트랙(사람)이 화면에 처음 잡히는 순간 `[신규 감지] track_id=... 최초 등장 ...`
  로그가 한 번 찍히고(bbox 크기 + yolo_conf 포함), 그 track_id의 raw score CSV
  행에는 `track_age_frames`(1부터 증가, 감지 안 된 프레임은 카운트 안 함)와
  `window_fill`/`min_window_fill`, `low_conf`, `yolo_confidence`, `bbox_w/h/area`가
  같이 남는다. 같은 track_id로 `track_age_frames`가 작은(등장 초반) 행과 큰
  (`stable_label`이 DECIDING_LABEL을 벗어난 이후, 안정화 구간) 행을 비교하면
  ①YOLO confidence가 초반에 유독 낮은지 ②bbox 크기가 초반에 작거나 들쭉날쭉한지
  ③window_fill이 min_window_fill 미만(=완화① 콜드스타트 게이팅이 실제로 작동
  중)인지를 CSV만으로 바로 확인할 수 있다. 스키마가 바뀌면(컬럼 추가 등) 기존
  CSV는 자동으로 `_old_schema_<timestamp>`로 보존되고 새 파일로 다시 시작한다.

⚠️ 2026-09-15 leave-one-out 진단에서 확인된 "B-attractor": 학습에 안 쓴 사람이
  person_B로 극단적으로 오수용되는 왜곡이 adapter(v2)에 있음이 실증됨
  (face_embedding_adapter_loo_test.py 참고 — held-out unknown_1이 dist_person_B
  0.03 근처로 100% 오수용). 같은 세션에서 시도한 L2 anchor 정규화(실험 B,
  face_embedding_adapter_finetune.pairwise_contrastive_loss의 reg_lambda)는
  λ=0.01~1.0 전 구간에서 이 왜곡을 거의 완화하지 못했다(최악 오수용율이 93.8~100%
  범위에 머묾) — 정규화만으로는 근본 해결이 안 된다는 뜻.

실험 C — zero-shot 이중 확인(ensemble) 게이트 (2026-09-15 추가):
  adapter의 왜곡은 adapter 자체(작은 residual 학습)에서 생긴 것이라, 어댑터를
  아예 안 거친 zero-shot(frozen FaceNet 그대로) 판단은 같은 방식으로는 안 왜곡될
  가능성이 높다는 전제로, 두 판단이 서로 동의할 때만 최종 확정하는 AND 게이트를
  추가했다:
    - adapter 판단: 기존과 동일(embed_crop의 adapter-tuned 임베딩 vs person_
      centroids.pt, identity_threshold 기준)
    - zero-shot 판단: embed_crop의 adapter 적용 전 frozen 임베딩 vs zero_shot_
      centroids.pt(build_zero_shot_centroids.py 출력), zero_shot_identity_
      threshold 기준 — 독립적으로 계산
    - 최종 raw_label: 두 판단이 같은 특정 인물로 일치할 때만 그 인물, 그 외
      (한쪽이 unknown이거나 서로 다른 인물)에는 전부 unknown.
  zero-shot 자체는 분리력이 약해서(참고: A/B val 자기-centroid 거리 p90=0.49,
  unknown 오수용율이 threshold=0.40에서 약 25%) 단독 분류기로는 못 쓰지만,
  "adapter가 틀렸을 때 그 오류에 zero-shot까지 우연히 동의할 확률은 낮다"는 게
  이중 확인의 취지 — 두 번째 안전장치일 뿐 attractor의 근본 원인을 없애지는
  않는다. 화면/CSV에 adapter 판단과 zero-shot 판단을 모두 표시해서 언제 게이트가
  막았는지 눈으로 확인 가능.
  사전 준비물 추가: zero_shot_centroids.pt(build_zero_shot_centroids.py로 생성
  후 로봇에 scp, person_centroids.pt와 같은 디렉터리).

실사례 진단(2026-09-15, 스크린샷 3장)에서 확인: 전기 패널의 원형 빨간 버튼처럼
"둥근 고대비 물체"는 confidence(0.51~0.69, 진짜 얼굴 범위 0.50~0.87과 심하게
겹침)로도 bbox 종횡비(0.96, 진짜 얼굴 0.98과 사실상 동일)로도 못 거른다 —
이중 확인 게이트가 "그 물체를 person_A/B로 오인식"하는 것은 막아주지만
(zero-shot이 unknown이라고 반대하므로), 최종 결과가 "unknown 100%"로는 뜬다 —
침입자 감지 관점에선 이것도 오탐. 아래 완화③(사람 전신 게이트)은 이 문제를
YOLO confidence/bbox 종횡비가 아닌 **완전히 다른 신호**(사람 전신이 그 자리에
실제로 있는지)로 접근한다.

완화③ — 사람 전신 검출 게이트 (2026-09-15 추가):
  얼굴 검출(face_detect_v4_burstsafe)과 완전히 별개로, COCO 사전학습 YOLO
  (yolov8n.pt, person 클래스만 사용)를 매 프레임 병렬로 돌린다. 얼굴 bbox의
  중심점이 어떤 person bbox 안에도 안 들어가면(전기 패널 버튼처럼 사람이 없는
  곳에서 얼굴 모양 검출이 뜬 경우), 임베딩/신원판단 파이프라인 자체를 건너뛰고
  화면에 OBJECT_LABEL("물체")로만 표시한다 — LOW_CONF_LABEL(검출불안정, YOLO
  confidence 미달)이나 DECIDING_LABEL(판정중, window 미충전)과는 별개 상태.
  person_gate_mode 파라미터로 "center"(얼굴 중심점 포함 여부, 기본값) 또는
  "overlap_ratio"(얼굴 bbox 면적 대비 person bbox와 겹치는 비율이 person_gate_
  overlap_ratio 이상인지) 중 선택 가능. enable_person_gate:=false로 끌 수 있음
  (비교용). 얼굴 검출·adapter·zero-shot 로직은 전혀 안 건드림 — 이 게이트는
  그 앞단에 독립적으로 얹힌 것.
  사전 준비물 추가: person_yolo_weights(기본 ~/careguard_data/runs/
  yolov8n_coco/yolov8n.pt, 사전학습 COCO 가중치 그대로 — 재학습 없음).
"""
import csv
import os
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
DEFAULT_ZERO_SHOT_CENTROIDS_PATH = str(
    Path.home() / "careguard_data/runs/face_embedding_adapter/zero_shot_centroids.pt"
)
# ── 실험 C(이중 확인) zero-shot 판단용 threshold. build_zero_shot_centroids.py로
# 만든 centroid 기준 자체 보정 계산(2026-09-15): val A/B 자기-centroid 거리
# p90=0.49, 이 값(0.40)에서 진짜 A/B 자기 인식률 88%·unknown 오수용율 25% —
# zero-shot 단독으로는 약한 분류기지만(참고: 크롭-쌍 EER 22%), 이중 확인의
# 두 번째 게이트로만 쓰이므로 adapter와 같은 값(0.40)에서 시작 — 실측 후 조정 권장.
DEFAULT_ZERO_SHOT_IDENTITY_THRESHOLD = 0.40

# ── 완화③(사람 전신 게이트)용 COCO 사전학습 YOLO — 재학습 없이 그대로 사용.
DEFAULT_PERSON_YOLO_WEIGHTS = str(Path.home() / "careguard_data/runs/yolov8n_coco/yolov8n.pt")
DEFAULT_PERSON_MIN_CONFIDENCE = 0.5
COCO_PERSON_CLASS_ID = 0
# "center": 얼굴 bbox 중심점이 person bbox 안에 있는지(기본, 단순·강건).
# "overlap_ratio": 얼굴 bbox 면적 대비 person bbox와 겹치는 비율이 이 값 이상인지.
DEFAULT_PERSON_GATE_MODE = "center"
DEFAULT_PERSON_GATE_OVERLAP_RATIO = 0.5

DECIDING_LABEL = "판정중"  # 콜드스타트: window가 아직 덜 찬 트랙의 상태(사람도, unknown도 아님)
LOW_CONF_LABEL = "검출불안정"  # YOLO confidence가 threshold 미만이라 식별 파이프라인에 안 넘긴 프레임
OBJECT_LABEL = "물체"  # 완화③: 얼굴 bbox가 어떤 person bbox 안에도 없어서 식별을 건너뛴 프레임

BOX_COLOR_BGR = {
    "person_A": (60, 180, 75),
    "person_B": (66, 133, 244),
    "unknown": (60, 60, 220),
    DECIDING_LABEL: (0, 215, 255),   # 주황 — 아직 판정 보류 중
    LOW_CONF_LABEL: (128, 128, 128),  # 회색 — 이번 프레임 검출 자체를 신뢰 안 함
    OBJECT_LABEL: (255, 0, 255),     # 자홍 — 사람 전신이 근처에 없는 얼굴형 검출
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

# ── raw score CSV 크기 기준 롤오버(2026-09-15) — 0 이하면 끔.
DEFAULT_RAW_SCORE_MAX_SIZE_MB = 50

# ── raw score CSV 컬럼. track_age_frames(트랙이 감지된 프레임 수 누적, 안 보인
# 프레임 제외)로 "등장 초반"(작은 값) vs "안정화 이후"(큰 값) 구간을 구분한다.
# yolo_confidence/bbox_w/bbox_h/bbox_area는 low_conf 프레임(라벨 없음)에도 항상 남긴다 —
# 등장 초반에 confidence가 낮은지/bbox가 불안정한지 확인하는 게 목적이라 이 값들은
# 식별 성공 여부와 무관하게 필요함.
RAW_SCORE_CSV_HEADER = [
    "timestamp", "frame_count", "track_id", "track_age_frames", "low_conf",
    "raw_label", "stable_label", "stable_frac", "window_fill", "min_window_fill",
    "dist_person_A", "dist_person_B", "identity_threshold",
    "yolo_confidence", "bbox_w", "bbox_h", "bbox_area",
    # ── 실험C(이중 확인, 2026-09-15) — adapter 판단과 zero-shot 판단을 각각 남겨서
    # 언제 게이트가 최종 판정을 막았는지(둘이 불일치) 나중에 확인할 수 있게 함.
    "adapter_label", "zs_label", "zs_dist_person_A", "zs_dist_person_B", "zero_shot_identity_threshold",
    # ── 완화③(사람 전신 게이트, 2026-09-15) — 이 얼굴 검출이 person bbox 안에
    # 있었는지. low_conf라 애초에 안 봤으면 빈 값.
    "person_contained", "num_person_boxes",
]


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


def face_in_any_person_box(face_box, person_boxes, mode=DEFAULT_PERSON_GATE_MODE,
                            overlap_ratio_threshold=DEFAULT_PERSON_GATE_OVERLAP_RATIO):
    """완화③(사람 전신 게이트): face_box가 person_boxes 중 하나에 "포함"되는지 판단.

    - mode="center"(기본): face_box 중심점이 어떤 person box 안에 있으면 포함.
      단순·강건 — 얼굴이 몸통 bbox 안쪽에 있는 일반적인 경우를 잘 잡는다.
    - mode="overlap_ratio": 표준 IoU(교집합/합집합)를 안 쓴다 — 얼굴은 몸통 전체
      bbox 면적의 일부(보통 5~15%)라서 대칭 IoU로는 실제 포함된 경우도 항상 작게
      나온다. 대신 "얼굴 bbox 면적 대비 person bbox와 겹치는 면적의 비율"(비대칭)을
      써서 overlap_ratio_threshold 이상이면 포함으로 본다.
    """
    x1, y1, x2, y2 = face_box
    cx, cy = (x1 + x2) / 2.0, (y1 + y2) / 2.0
    face_area = max(0.0, x2 - x1) * max(0.0, y2 - y1)

    for px1, py1, px2, py2 in person_boxes:
        if mode == "center":
            if px1 <= cx <= px2 and py1 <= cy <= py2:
                return True
        else:
            ix1, iy1 = max(x1, px1), max(y1, py1)
            ix2, iy2 = min(x2, px2), min(y2, py2)
            inter = max(0.0, ix2 - ix1) * max(0.0, iy2 - iy1)
            if face_area > 0 and (inter / face_area) >= overlap_ratio_threshold:
                return True
    return False


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

    __slots__ = ("track_id", "center", "window", "stable_label", "stable_frac",
                 "missed_frames", "track_age_frames")

    def __init__(self, track_id, center, window_size):
        self.track_id = track_id
        self.center = center
        self.window = deque(maxlen=window_size)
        self.stable_label = DECIDING_LABEL  # 완화① — 갓 생긴 트랙은 아직 아무 판정도 안 함
        self.stable_frac = 0.0
        self.missed_frames = 0
        # ── 2026-09-15 추가: 이 트랙이 처음 감지된 뒤 몇 프레임째인지(감지된 프레임만
        # 카운트, 안 보인 프레임은 제외) — "등장 초반" vs "안정화 이후" 구간을 raw score
        # CSV에서 나중에 구분해서 비교하기 위함.
        self.track_age_frames = 0


class FaceIdentifyTestNode(Node):
    def __init__(self):
        super().__init__("face_identify_test_node")

        self.declare_parameter("camera_topic", CAMERA_TOPIC)
        self.declare_parameter("yolo_weights", DEFAULT_YOLO_WEIGHTS)
        self.declare_parameter("adapter_path", DEFAULT_ADAPTER_PATH)
        self.declare_parameter("centroids_path", DEFAULT_CENTROIDS_PATH)
        self.declare_parameter("identity_threshold", DEFAULT_IDENTITY_THRESHOLD)
        self.declare_parameter("zero_shot_centroids_path", DEFAULT_ZERO_SHOT_CENTROIDS_PATH)
        self.declare_parameter("zero_shot_identity_threshold", DEFAULT_ZERO_SHOT_IDENTITY_THRESHOLD)
        self.declare_parameter("enable_zero_shot_ensemble", True)
        self.declare_parameter("person_yolo_weights", DEFAULT_PERSON_YOLO_WEIGHTS)
        self.declare_parameter("person_min_confidence", DEFAULT_PERSON_MIN_CONFIDENCE)
        self.declare_parameter("person_gate_mode", DEFAULT_PERSON_GATE_MODE)
        self.declare_parameter("person_gate_overlap_ratio", DEFAULT_PERSON_GATE_OVERLAP_RATIO)
        self.declare_parameter("enable_person_gate", True)
        self.declare_parameter("enable_display", True)
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
        self.declare_parameter("raw_score_max_size_mb", DEFAULT_RAW_SCORE_MAX_SIZE_MB)
        self.declare_parameter("min_window_fill_ratio", DEFAULT_MIN_WINDOW_FILL_RATIO)
        self.declare_parameter("min_yolo_confidence", DEFAULT_MIN_YOLO_CONFIDENCE)

        camera_topic = self.get_parameter("camera_topic").value
        yolo_weights = self.get_parameter("yolo_weights").value
        adapter_path = self.get_parameter("adapter_path").value
        centroids_path = self.get_parameter("centroids_path").value
        self.threshold = self.get_parameter("identity_threshold").value
        zero_shot_centroids_path = self.get_parameter("zero_shot_centroids_path").value
        self.zero_shot_threshold = float(self.get_parameter("zero_shot_identity_threshold").value)
        self.enable_zero_shot_ensemble = self.get_parameter("enable_zero_shot_ensemble").value
        self.enable_display = self.get_parameter("enable_display").value
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

        person_yolo_weights = self.get_parameter("person_yolo_weights").value
        self.person_min_confidence = float(self.get_parameter("person_min_confidence").value)
        self.person_gate_mode = self.get_parameter("person_gate_mode").value
        self.person_gate_overlap_ratio = float(self.get_parameter("person_gate_overlap_ratio").value)
        self.enable_person_gate = self.get_parameter("enable_person_gate").value

        self.log_raw_scores = self.get_parameter("log_raw_scores").value
        self.raw_score_log_path = Path(self.get_parameter("raw_score_log_path").value).expanduser()
        raw_score_max_size_mb = float(self.get_parameter("raw_score_max_size_mb").value)
        self.raw_score_max_bytes = (
            int(raw_score_max_size_mb * 1024 * 1024) if raw_score_max_size_mb > 0 else None
        )

        self.get_logger().warn(
            "⚠️  [테스트/디버깅 전용] 이 identity 판단은 unknown_1/2 단 두 명(32장)만으로 "
            "negative 학습된 adapter를 씁니다 — 완전히 새로운 낯선 사람에 대한 open-set 거부 "
            "성능은 검증되지 않았습니다. 프로덕션 순찰 로직에 연결하지 마세요."
        )
        if self.log_raw_scores:
            self.raw_score_log_path.parent.mkdir(parents=True, exist_ok=True)
            self._open_raw_score_file(check_schema=True)
            size_note = (
                f"raw_score_max_size_mb={raw_score_max_size_mb}MB 넘으면 자동 롤오버"
                if self.raw_score_max_bytes else "크기 롤오버 꺼짐(raw_score_max_size_mb<=0)"
            )
            self.get_logger().info(
                f"raw 유사도 점수를 매 프레임 CSV로 기록합니다: {self.raw_score_log_path} ({size_note}). "
                "(진단③ — 학습 때 in-sample 검증 수치와 실시간 점수를 나중에 비교하기 위함. "
                "track_age_frames 컬럼으로 '등장 초반'(값이 작음) vs '안정화 이후'(값이 큼) "
                "구간을 나중에 필터링해서 비교할 수 있음)"
            )

        self.device = "cuda" if torch.cuda.is_available() else "cpu"
        self.get_logger().info(f"device={self.device}")

        from ultralytics import YOLO  # 지연 import — 노드 파일 로드만으로 무거운 라이브러리 안 끌고오게
        self.get_logger().info(f"YOLO 얼굴 검출 가중치 로딩: {yolo_weights}")
        self.yolo = YOLO(yolo_weights)

        if self.enable_person_gate:
            self.get_logger().info(f"[완화③] COCO 사전학습 person 검출 YOLO 로딩: {person_yolo_weights}")
            self.person_yolo = YOLO(person_yolo_weights)
            self.get_logger().info(
                f"[완화③] 사람 전신 게이트 활성화 — person_gate_mode={self.person_gate_mode} "
                + (f"overlap_ratio={self.person_gate_overlap_ratio} "
                   if self.person_gate_mode != "center" else "")
                + f"person_min_confidence={self.person_min_confidence} — 얼굴이 어떤 person bbox "
                f"안에도 없으면 식별 없이 '{OBJECT_LABEL}'로만 표시 "
                "(enable_person_gate:=false로 끌 수 있음, 비교용)."
            )
        else:
            self.person_yolo = None
            self.get_logger().warn(
                "[완화③] 사람 전신 게이트가 꺼져 있습니다 — 얼굴형 검출이면 사람 유무와 "
                "무관하게 전부 식별 파이프라인으로 넘어갑니다 (비교 테스트용)."
            )

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

        if self.enable_zero_shot_ensemble:
            zs_cent = torch.load(zero_shot_centroids_path, map_location=self.device)
            self.zero_shot_centroids = {name: zs_cent[name].to(self.device) for name in ("person_A", "person_B")}
            self.get_logger().info(
                f"[실험C] zero-shot centroid 로딩 완료: {zero_shot_centroids_path} "
                f"(n_A={zs_cent.get('n_A', '?')} n_B={zs_cent.get('n_B', '?')}) "
                f"zero_shot_identity_threshold={self.zero_shot_threshold}"
            )
            self.get_logger().info(
                "[실험C] 이중 확인(ensemble) 게이트 활성화 — adapter 판단과 zero-shot 판단이 "
                "동시에 같은 인물로 동의할 때만 최종 확정, 아니면 unknown "
                "(enable_zero_shot_ensemble:=false로 끌 수 있음, 비교용)."
            )
        else:
            self.zero_shot_centroids = None
            self.get_logger().warn(
                "[실험C] 이중 확인(ensemble) 게이트가 꺼져 있습니다 — adapter 판단만 씁니다 "
                "(B-attractor 완화 없이 v2 단독 동작, 비교 테스트용)."
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
        self._frame_time_accum = 0.0  # ── enable_display/log_raw_scores 부하 비교용 계측(초)
        self._frame_time_count = 0
        # 카메라 드라이버가 qos_profile_sensor_data(best-effort)로 발행함 — video_recorder.py와
        # 동일하게 맞춰야 구독이 실제로 붙는다. 기본(reliable) QoS로는 DDS가 매칭을 거부해서
        # 콜백이 아예 안 불릴 수 있다.
        self.sub = self.create_subscription(
            CompressedImage, camera_topic, self.on_frame, qos_profile_sensor_data
        )
        self.get_logger().info(f"구독 시작: {camera_topic} (qos=sensor_data/best_effort)")

        self.annotated_pub = self.create_publisher(Image, "face_identify_test/annotated", 10)

        if self.enable_display:
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
        else:
            self.get_logger().info(
                "[headless] enable_display=false — bbox/라벨 그리기 연산 자체를 건너뜁니다. "
                "'face_identify_test/annotated' 토픽엔 그림 없는 원본 프레임이 그대로 발행되고, "
                "cv2.imshow도 안 뜹니다. 검출/신원판단/CSV 로깅/콘솔 로그는 평소와 동일하게 동작합니다."
            )

    def _open_raw_score_file(self, check_schema):
        """raw_score_log_path에 CSV writer를 새로 연다(최초 오픈/롤오버 공용).

        check_schema=True(최초 오픈 시)면 기존 파일의 헤더가 현재
        RAW_SCORE_CSV_HEADER와 다를 때 `_old_schema_<timestamp>`로 보존 후
        새로 시작한다. 롤오버 호출(check_schema=False) 시점엔 이미 이 프로세스가
        쓴 파일이라 스키마가 같음이 보장되므로 그 검사를 생략한다."""
        if check_schema and self.raw_score_log_path.exists():
            with open(self.raw_score_log_path, newline="") as f:
                existing_header = f.readline().strip()
            if existing_header and existing_header != ",".join(RAW_SCORE_CSV_HEADER):
                # ── 스키마(컬럼)가 바뀌었는데 기존 파일에 이어 쓰면 컬럼이 어긋나서
                # 나중에 분석할 때 잘못 해석될 수 있음 — 옛 파일은 이름을 바꿔 보존하고
                # 새 헤더로 새로 시작한다.
                stamp = time.strftime("%Y%m%d_%H%M%S")
                archived = self.raw_score_log_path.with_name(
                    f"{self.raw_score_log_path.stem}_old_schema_{stamp}{self.raw_score_log_path.suffix}"
                )
                self.raw_score_log_path.rename(archived)
                self.get_logger().warn(
                    f"raw score CSV 스키마 변경 감지 — 기존 파일을 {archived}로 보존하고 "
                    "새 헤더로 새로 씁니다."
                )

        is_new_file = not self.raw_score_log_path.exists()
        self._raw_score_file = open(self.raw_score_log_path, "a", newline="")
        self._raw_score_writer = csv.writer(self._raw_score_file)
        if is_new_file:
            self._raw_score_writer.writerow(RAW_SCORE_CSV_HEADER)
            self._raw_score_file.flush()

    def _rotate_raw_score_file_if_needed(self):
        """raw_score_max_size_mb(2026-09-15 추가)를 넘으면 현재 파일을
        `_rotated_<timestamp>`로 보존하고 헤더부터 새로 시작한다 — 헤드리스로
        며칠씩 켜놔도 디스크를 한없이 못 채우게 크기로 상한을 둔다. 시간 기준
        자동 삭제 대신 이 방식을 고른 이유는 클래스 docstring 참고."""
        if self.raw_score_max_bytes is None:
            return
        size = os.path.getsize(self.raw_score_log_path)
        if size < self.raw_score_max_bytes:
            return

        self._raw_score_file.close()
        stamp = time.strftime("%Y%m%d_%H%M%S")
        rotated = self.raw_score_log_path.with_name(
            f"{self.raw_score_log_path.stem}_rotated_{stamp}{self.raw_score_log_path.suffix}"
        )
        # ── raw_score_max_size_mb를 작게 잡아 테스트하는 등, 같은 초(second) 안에
        # 롤오버가 여러 번 일어나면 타임스탬프만으론 파일명이 겹쳐서 이전 롤오버
        # 파일이 rename()에 덮여 조용히 사라질 수 있다 — 겹치면 번호를 붙여 피한다.
        n = 2
        while rotated.exists():
            rotated = self.raw_score_log_path.with_name(
                f"{self.raw_score_log_path.stem}_rotated_{stamp}_{n}{self.raw_score_log_path.suffix}"
            )
            n += 1
        self.raw_score_log_path.rename(rotated)
        self.get_logger().info(
            f"raw score CSV가 {size / 1024 / 1024:.1f}MB에 도달 — {rotated}로 보존하고 새로 시작합니다."
        )
        self._open_raw_score_file(check_schema=False)

    def embed_crop(self, face_bgr):
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
            zero_shot_emb = self.embedder(t)  # InceptionResnetV1 출력은 이미 L2-normalize된 상태
            adapter_emb = F.normalize(self.adapter(zero_shot_emb), p=2, dim=1)
        # ── 실험C(이중 확인)용: adapter 적용 전 zero-shot 임베딩도 같이 반환.
        return adapter_emb[0], zero_shot_emb[0]

    def identify(self, adapter_emb: torch.Tensor, zero_shot_emb: torch.Tensor):
        """adapter 판단과 zero-shot 판단을 각각 독립적으로 계산하고, 이중 확인
        (실험C, 2026-09-15)이 켜져 있으면 둘이 같은 특정 인물로 동의할 때만 그
        인물로 확정한다 — 2026-09-15 leave-one-out 진단에서 확인된 adapter의
        "B-attractor" 왜곡(학습에 안 쓴 사람이 person_B로 극단적으로 오수용되는
        현상)을 완화하기 위한 두 번째 안전장치.

        Returns: (final_label, adapter_dist, adapter_dists, adapter_label, zs_label, zs_dist, zs_dists)
        adapter_dists/zs_dists는 {"person_A": dist, "person_B": dist} 전체(로그/진단용).
        """
        adapter_dists = {name: float(1.0 - torch.dot(adapter_emb, c)) for name, c in self.centroids.items()}
        adapter_name, adapter_dist = min(adapter_dists.items(), key=lambda kv: kv[1])
        adapter_label = adapter_name if adapter_dist <= self.threshold else "unknown"

        if not self.enable_zero_shot_ensemble:
            return adapter_label, adapter_dist, adapter_dists, adapter_label, None, None, None

        zs_dists = {name: float(1.0 - torch.dot(zero_shot_emb, c)) for name, c in self.zero_shot_centroids.items()}
        zs_name, zs_dist = min(zs_dists.items(), key=lambda kv: kv[1])
        zs_label = zs_name if zs_dist <= self.zero_shot_threshold else "unknown"

        final_label = adapter_label if (adapter_label != "unknown" and adapter_label == zs_label) else "unknown"
        return final_label, adapter_dist, adapter_dists, adapter_label, zs_label, zs_dist, zs_dists

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
                new_track_id = self._next_track_id
                track = FaceTrack(new_track_id, det["center"], self.window_size)
                self.tracks[new_track_id] = track
                self._next_track_id += 1
                x1, y1, x2, y2 = det["box"]
                # ── 사람이 화면에 처음 등장하는 순간(새 트랙 생성 시점)을 로그로 남김 —
                # 이 시각/프레임 번호를 raw score CSV의 track_age_frames=1 행과
                # 맞춰보면 "등장 초반" 구간을 정확히 찾을 수 있다.
                self.get_logger().info(
                    f"[신규 감지] track_id={new_track_id} 최초 등장 (frame#{self.frame_count}) — "
                    f"bbox={x2 - x1:.0f}x{y2 - y1:.0f}px  yolo_conf={det['conf']:.2f}"
                    + ("  (⚠️ confidence 미달로 식별 파이프라인엔 안 들어감)" if det["low_conf"] else "")
                )

            track.center = det["center"]
            track.missed_frames = 0
            track.track_age_frames += 1

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
        frame_start = time.perf_counter()  # ── enable_display/log_raw_scores 부하 비교용 계측
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

        # ── 완화③ 사람 전신 게이트: 얼굴 검출과 완전히 별개로 COCO person 검출을
        # 매 프레임 한 번만 실행하고, 아래에서 얼굴마다 재사용한다.
        person_boxes = []
        if self.enable_person_gate:
            person_results = self.person_yolo.predict(
                image, classes=[COCO_PERSON_CLASS_ID], conf=self.person_min_confidence, verbose=False
            )[0]
            person_boxes = [tuple(float(v) for v in b.xyxy[0]) for b in person_results.boxes]
            if self.frame_count % 30 == 1:
                self.get_logger().info(f"[진단][완화③] person 검출 수: {len(person_boxes)}")

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
                    "adapter_label": None, "zs_label": None, "zs_dist": None, "zs_dists": None,
                    "person_contained": None, "num_person_boxes": None, "object_gated": False,
                })
                continue

            # ── 완화③ 사람 전신 게이트: 얼굴이 어떤 person bbox 안에도 없으면 크롭/
            # 임베딩/신원판단을 아예 건너뛴다 — 얼굴 검출·adapter·zero-shot 로직은
            # 그대로 두고 그 앞단에 독립적으로 얹은 게이트.
            if self.enable_person_gate and not face_in_any_person_box(
                (x1, y1, x2, y2), person_boxes, self.person_gate_mode, self.person_gate_overlap_ratio
            ):
                detections.append({
                    "box": (x1, y1, x2, y2), "center": center, "conf": conf,
                    "raw_label": None, "dist": None, "dists": None, "low_conf": False,
                    "adapter_label": None, "zs_label": None, "zs_dist": None, "zs_dists": None,
                    "person_contained": False, "num_person_boxes": len(person_boxes),
                    "object_gated": True,
                })
                continue

            face = crop_with_margin(image, (x1, y1, x2, y2))
            if face.size == 0:
                continue

            adapter_emb, zero_shot_emb = self.embed_crop(face)
            raw_label, dist, dists, adapter_label, zs_label, zs_dist, zs_dists = self.identify(
                adapter_emb, zero_shot_emb
            )
            detections.append({
                "box": (x1, y1, x2, y2), "center": center, "conf": conf,
                "raw_label": raw_label, "dist": dist, "dists": dists, "low_conf": False,
                "adapter_label": adapter_label, "zs_label": zs_label, "zs_dist": zs_dist, "zs_dists": zs_dists,
                "person_contained": True if self.enable_person_gate else None,
                "num_person_boxes": len(person_boxes) if self.enable_person_gate else None,
                "object_gated": False,
            })

        self._update_tracks(detections)

        if self.log_raw_scores:
            now = time.time()
            for det in detections:
                track = det["track"]
                x1, y1, x2, y2 = det["box"]
                bbox_w, bbox_h = x2 - x1, y2 - y1
                # low_conf 프레임은 임베딩/거리 계산 자체를 안 했으므로(dists=None) 거리
                # 칼럼은 비워두지만, 등장 초반 진단에 필요한 yolo_confidence/bbox 크기/
                # track_age_frames/window_fill은 low_conf 여부와 무관하게 항상 남긴다.
                dist_a = f"{det['dists']['person_A']:.4f}" if det["dists"] is not None else ""
                dist_b = f"{det['dists']['person_B']:.4f}" if det["dists"] is not None else ""
                # 실험C(이중 확인) 필드 — low_conf거나 게이트가 꺼져있으면(zs_dists=None) 비움.
                zs_dist_a = f"{det['zs_dists']['person_A']:.4f}" if det["zs_dists"] is not None else ""
                zs_dist_b = f"{det['zs_dists']['person_B']:.4f}" if det["zs_dists"] is not None else ""
                self._raw_score_writer.writerow([
                    f"{now:.3f}", self.frame_count, track.track_id, track.track_age_frames,
                    int(det["low_conf"]),
                    det["raw_label"] or "", track.stable_label, f"{track.stable_frac:.4f}",
                    len(track.window), self.min_window_fill,
                    dist_a, dist_b, self.threshold,
                    f"{det['conf']:.4f}", f"{bbox_w:.1f}", f"{bbox_h:.1f}", f"{bbox_w * bbox_h:.1f}",
                    det["adapter_label"] or "", det["zs_label"] or "",
                    zs_dist_a, zs_dist_b, self.zero_shot_threshold,
                    "" if det["person_contained"] is None else int(det["person_contained"]),
                    "" if det["num_person_boxes"] is None else det["num_person_boxes"],
                ])
            self._raw_score_file.flush()
            self._rotate_raw_score_file_if_needed()

        # ── 화면 표시 분리(2026-09-15): enable_display=False면 그리기 연산 자체를
        # 건너뛴다(단순히 imshow만 끄는 게 아니라 cv2.rectangle/putText 연산 부담도
        # 없앰). 검출/신원판단/CSV 로깅/annotated 토픽 발행은 이 값과 무관하게 그대로
        # 동작 — 이 분기 밖에서 이미 다 끝난 뒤라 영향 없음(headless 순찰 대비).
        if self.enable_display:
            self._draw_detections(image, detections)

        out_msg = self.bridge.cv2_to_imgmsg(image, encoding="bgr8")
        out_msg.header = msg.header
        self.annotated_pub.publish(out_msg)

        if self.enable_display and self.show_window:
            cv2.imshow("face_identify_test (q로 종료)", image)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                self.get_logger().info("q 입력 — 노드 종료")
                rclpy.shutdown()

        # ── enable_display/log_raw_scores 부하 비교용 계측: 30프레임마다 평균
        # on_frame() 처리 시간(ms)을 로그로 남긴다 — 두 설정(둘 다 켬 vs 둘 다 끔)으로
        # 각각 실행해서 이 로그만 비교하면 실측 부하 차이를 확인할 수 있음.
        self._frame_time_accum += time.perf_counter() - frame_start
        self._frame_time_count += 1
        if self._frame_time_count >= 30:
            avg_ms = self._frame_time_accum / self._frame_time_count * 1000
            self.get_logger().info(
                f"[성능] 최근 {self._frame_time_count}프레임 평균 on_frame() 처리시간: "
                f"{avg_ms:.1f}ms (enable_display={self.enable_display}, "
                f"log_raw_scores={self.log_raw_scores})"
            )
            self._frame_time_accum = 0.0
            self._frame_time_count = 0

    def _draw_detections(self, image, detections):
        """enable_display=True일 때만 호출됨 — bbox+라벨 그리기 전용, 검출/신원판단/
        로깅 로직과 완전히 분리."""
        for det in detections:
            x1, y1, x2, y2 = det["box"]
            track = det["track"]
            conf_s = f"yolo_conf={det['conf']:.2f}"

            if det["low_conf"]:
                # 완화②: 이번 프레임은 confidence 미달이라 식별 파이프라인에 안 넘겼음 —
                # track에 쌓여 있던 기존 판정과 무관하게 이번 프레임만 회색으로 표시.
                color = BOX_COLOR_BGR[LOW_CONF_LABEL]
                label = f"{LOW_CONF_LABEL} ({conf_s} < {self.min_yolo_confidence:.2f})"
            elif det.get("object_gated"):
                # 완화③: 얼굴형 검출이지만 어떤 person bbox 안에도 없었음 — 신원판단
                # 자체를 안 했으므로 track에 쌓인 기존 판정과 무관하게 자홍색으로 표시.
                color = BOX_COLOR_BGR[OBJECT_LABEL]
                label = f"{OBJECT_LABEL} ({conf_s}, person bbox {det['num_person_boxes']}개 중 미포함)"
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
                # 비교할 수 있게 함. 실험C(이중 확인)가 켜져 있으면 adapter/zero-shot 판단이
                # 서로 갈릴 때(게이트가 막을 때) 눈으로 바로 보이게 둘 다 표시.
                ensemble_s = ""
                if self.enable_zero_shot_ensemble and det["adapter_label"] is not None:
                    ensemble_s = f" [adapter:{det['adapter_label']} zs:{det['zs_label']}]"
                label = (f"{stable_label} {stable_frac * 100:.0f}% "
                         f"(raw:{det['raw_label']} {dist_s} {conf_s}){ensemble_s}")

            cv2.rectangle(image, (int(x1), int(y1)), (int(x2), int(y2)), color, 2)
            (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 2)
            ty = max(th + 4, int(y1) - 6)
            cv2.rectangle(image, (int(x1), ty - th - 6), (int(x1) + tw + 6, ty + 2), color, -1)
            cv2.putText(image, label, (int(x1) + 3, ty - 3),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)


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
