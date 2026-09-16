#!/usr/bin/env python3
"""careguard_perception/face_pipeline_common.py

얼굴 검출/신원판단 파이프라인의 순수 로직 부분 — face_identify_test_node.py(테스트/
디버깅용)와 patrol_intruder_watch_node.py(프로덕션 순찰 감시용)가 공유한다.

이 모듈을 분리한 이유: 두 노드가 "같은 얼굴을 같은 기준으로 같은 사람이라고
판단해야" 하므로(안 그러면 테스트 노드에서 검증한 threshold/게이팅 동작이
프로덕션에서는 다르게 동작하는 drift가 생김), sliding-window 다수결+hysteresis,
사람 전신 게이트, crop 방식 같은 핵심 판정 로직은 하나로 유지하고 각 노드는
ROS 파라미터 선언·모델 로딩·카메라 구독·결과 소비(화면 표시 vs care_event
생성)만 각자 다르게 가져간다.

ROS(rclpy)에 의존하지 않는 순수 함수/클래스만 담는다 — 두 노드 모두 이 모듈을
그대로 import해서 쓴다.
"""
from collections import Counter, deque

# ── crop/식별 관련 공통 상수 ──
EMBED_SIZE = 160
CROP_MARGIN = 0.25

# ── identity 판단 threshold 기본값 — face_embedding_adapter_finetune.py 등에서
# 검증된 값. 두 노드 모두 이 기본값에서 시작하고, 필요하면 각자 ROS 파라미터로
# 덮어쓴다.
DEFAULT_IDENTITY_THRESHOLD = 0.40
DEFAULT_ZERO_SHOT_IDENTITY_THRESHOLD = 0.40

# ── 완화③(사람 전신 게이트) 기본값.
DEFAULT_PERSON_MIN_CONFIDENCE = 0.5
COCO_PERSON_CLASS_ID = 0
DEFAULT_PERSON_GATE_MODE = "center"
DEFAULT_PERSON_GATE_OVERLAP_RATIO = 0.5
DEFAULT_PERSON_GATE_INTERVAL = 5

# ── 멀티프레임 sliding-window 다수결 + hysteresis 기본값.
DEFAULT_WINDOW_SIZE = 15
DEFAULT_ENTER_THRESHOLD = 0.70
DEFAULT_EXIT_THRESHOLD = 0.40
DEFAULT_TRACK_MATCH_MAX_DIST = 80.0  # px
DEFAULT_TRACK_MAX_MISSED_FRAMES = 10

# ── 완화① 콜드스타트 게이팅 기본값 — enter_threshold와 같은 비율로 맞춤(둘 다
# 노드에서 이 상수를 그대로 쓰거나 필요시 각자 재정의할 수 있음).
DEFAULT_MIN_WINDOW_FILL_RATIO = DEFAULT_ENTER_THRESHOLD

# ── 완화② YOLO confidence 하한선 기본값.
DEFAULT_MIN_YOLO_CONFIDENCE = 0.5

# 라벨 상수 — 화면 표시(테스트 노드)와 로그/판단 분기(프로덕션 노드) 양쪽에서 쓴다.
DECIDING_LABEL = "판정중"  # 콜드스타트: window가 아직 덜 찬 트랙
LOW_CONF_LABEL = "검출불안정"  # YOLO confidence 미달로 식별 파이프라인에 안 넘긴 프레임
OBJECT_LABEL = "물체"  # 얼굴 bbox가 어떤 person bbox 안에도 없어서 식별을 건너뛴 프레임


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
    - mode="overlap_ratio": "얼굴 bbox 면적 대비 person bbox와 겹치는 면적의 비율"
      (비대칭)이 overlap_ratio_threshold 이상이면 포함으로 본다(표준 IoU는 얼굴이
      몸통 전체 bbox의 일부라 항상 작게 나와 못 씀).
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

    - current_state가 "unknown"일 때: 특정 인물로 "진입"하려면 그 인물 비율이
      enter_threshold 이상이어야 함.
    - current_state가 특정 인물일 때: 비율이 exit_threshold 이하로 떨어져야만
      "unknown"으로 이탈함(진입/이탈 기준을 다르게 둔 hysteresis).
    - 단, 다른 특정 인물이 즉시 enter_threshold 이상으로 지배적이면 곧바로 전환.

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
    bbox 중심 거리로 매칭하는 단순 구현."""

    __slots__ = ("track_id", "center", "window", "stable_label", "stable_frac",
                 "missed_frames", "track_age_frames")

    def __init__(self, track_id, center, window_size):
        self.track_id = track_id
        self.center = center
        self.window = deque(maxlen=window_size)
        self.stable_label = DECIDING_LABEL  # 갓 생긴 트랙은 아직 아무 판정도 안 함
        self.stable_frac = 0.0
        self.missed_frames = 0
        self.track_age_frames = 0


class FaceTrackManager:
    """FaceTrack들을 관리하는 컨테이너 — face_identify_test_node.py의
    _update_tracks() 로직을 그대로 옮긴 것. rclpy에 의존하지 않으므로(로깅은
    호출부가 반환값을 보고 알아서 남김) 두 노드가 동일하게 재사용할 수 있다.

    detections: 이번 프레임의 raw 판정 dict 리스트. 각 dict는 최소
    "center"(x, y)와 "raw_label"(문자열 또는 None, None이면 완화②로 이번 프레임
    신뢰 안 함)을 담고 있어야 한다. update()가 각 dict에 "track" 키를 채워 넣고,
    새로 생긴 트랙 정보(dict: track_id/center/conf 등 호출부가 로그用으로 쓸 수
    있게 detections에서 그대로 가져온 값)를 리스트로 반환한다.
    """

    def __init__(self, window_size, enter_threshold, exit_threshold,
                 track_match_max_dist, track_max_missed_frames, min_window_fill):
        self.window_size = window_size
        self.enter_threshold = enter_threshold
        self.exit_threshold = exit_threshold
        self.track_match_max_dist = track_match_max_dist
        self.track_max_missed_frames = track_max_missed_frames
        self.min_window_fill = min_window_fill
        self.tracks = {}
        self._next_track_id = 0

    def update(self, detections):
        unmatched_track_ids = set(self.tracks.keys())
        new_tracks = []

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
                new_tracks.append(dict(det, track_id=new_track_id))

            track.center = det["center"]
            track.missed_frames = 0
            track.track_age_frames += 1

            if det["raw_label"] is not None:
                track.window.append(det["raw_label"])
                if len(track.window) < self.min_window_fill:
                    track.stable_label = DECIDING_LABEL
                    track.stable_frac = len(track.window) / self.window_size
                else:
                    current_state = track.stable_label if track.stable_label != DECIDING_LABEL else "unknown"
                    track.stable_label, track.stable_frac = compute_stable_label(
                        track.window, current_state, self.enter_threshold, self.exit_threshold
                    )

            det["track"] = track

        for track_id in unmatched_track_ids:
            self.tracks[track_id].missed_frames += 1

        self.tracks = {
            tid: t for tid, t in self.tracks.items() if t.missed_frames <= self.track_max_missed_frames
        }

        return new_tracks
