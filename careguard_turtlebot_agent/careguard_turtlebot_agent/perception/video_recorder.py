#!/usr/bin/env python3
"""careguard_turtlebot_agent/perception/video_recorder.py

R-07(LIMO)의 careguard_perception/video_recorder.py를 R-06(터틀봇)으로 포팅한 것.
이벤트 전후(pre+post) 녹화 클립과, 순찰 시작~종료까지 이어지는 연속 녹화를 모두
만든다. 핵심 로직(pre-roll 링버퍼, 두 녹화 슬롯 동시 진행)은 R-07과 동일 — 아래
차이점만 R-06에 맞춰 바꿨다:

  - 카메라 토픽/상하반전: face_capture_node.py의 CAMERA_TOPIC/CAMERA_FLIP_VERTICAL을
    그대로 재사용한다(R-06: /camera/image_raw/compressed, imx219라 상하반전 필요 —
    R-07의 /camera/color/image_raw/compressed와 다름). 녹화 영상이 거꾸로 나오지
    않도록 _decode()에서 디코딩 직후 항상 같은 조건으로 뒤집는다.
  - 음성 안내 없음: R-07은 이벤트 심각도(severity)가 실리면 /careguard/voice/announce로
    쏴서 audio_player.py(별도 노드)가 재생하는 구조인데, R-06은 그런 토픽 기반
    음성 노드가 없다(voice.py::VoiceAnnouncer가 동기 함수 직접 호출 방식이라
    patrol_controller.py가 이미 그렇게 쓰고 있음). 그래서 이 파일은 severity
    안내 관련 코드를 아예 뺐다 — 나중에 필요해지면 VoiceAnnouncer를 주입받아
    직접 호출하는 방식으로 추가하면 된다.
  - 업로드 연동 없음: R-07은 media_uploader.py(careguard_agent)가 완성된 클립을
    서버로 올리는데, R-06의 uplink_node.py엔 그 기능이 없다. 이 노드는 로컬
    저장(~/careguard_data/videos/)까지만 하고 끝난다 — 서버 업로드는 별도 작업.

이벤트 트리거: /careguard/perception/record_trigger(std_msgs/String, JSON):
    ros2 topic pub --once /careguard/perception/record_trigger std_msgs/msg/String \\
        "data: '{\"care_event_id\": null, \"reason\": \"manual test\"}'"

순찰 연속 녹화 시작/종료: /careguard/perception/patrol_recording(std_msgs/String, JSON).
patrol_controller.py가 _run_patrol() 시작 시 start, 끝(성공/실패/취소 불문)에
stop을 발행한다:
    ros2 topic pub --once /careguard/perception/patrol_recording std_msgs/msg/String \\
        "data: '{\"action\": \"start\", \"run_id\": 1}'"
    ros2 topic pub --once /careguard/perception/patrol_recording std_msgs/msg/String \\
        "data: '{\"action\": \"stop\", \"run_id\": 1}'"

결과물: <output_dir>/<uuid>.mp4 + 같은 이름의 .json 사이드카.
"""
import json
import os
import time
import uuid
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional, Union

import cv2
import numpy as np
import rclpy
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage
from std_msgs.msg import String

from .face_capture_node import CAMERA_TOPIC, CAMERA_FLIP_VERTICAL

VIDEO_OUTPUT_DIR = Path(
    os.environ.get('CAREGUARD_VIDEO_DIR', os.path.expanduser('~/careguard_data/videos'))
)
ROBOT_CODE = os.environ.get('CAREGUARD_ROBOT_CODE', 'R-06')

TRIGGER_TOPIC = '/careguard/perception/record_trigger'
PATROL_RECORDING_TOPIC = '/careguard/perception/patrol_recording'

PRE_EVENT_SEC_DEFAULT = 5.0
POST_EVENT_SEC_DEFAULT = 15.0


class _ActiveRecording:
    """트리거 이후 진행 중인 녹화 하나의 상태. 동시에 하나만 허용한다(최소 구현)."""

    def __init__(self, writer: cv2.VideoWriter, path, stop_at: float, care_event_id, fps: float):
        self.writer = writer
        self.path = path
        self.stop_at = stop_at  # time.monotonic() 기준 종료 시각
        self.care_event_id = care_event_id
        self.fps = fps
        self.frame_count = 0
        self.captured_at = datetime.now(timezone.utc)


class _ActivePatrolRecording:
    """순찰 시작~종료까지 이어지는 연속 녹화 하나의 상태. patrol_controller.py의
    명시적 stop 신호가 올 때까지 무기한 이어 쓴다(pre-roll 없음)."""

    def __init__(self, writer: cv2.VideoWriter, path, run_id, fps: float):
        self.writer = writer
        self.path = path
        self.run_id = run_id
        self.fps = fps
        self.frame_count = 0
        self.captured_at = datetime.now(timezone.utc)


class VideoRecorder(Node):
    def __init__(self):
        super().__init__('video_recorder')

        self._pre_event_sec = float(self.declare_parameter('pre_event_sec', PRE_EVENT_SEC_DEFAULT).value)
        self._post_event_sec = float(self.declare_parameter('post_event_sec', POST_EVENT_SEC_DEFAULT).value)

        # (monotonic 타임스탬프, jpeg 원본 바이트) 링버퍼. 디코딩은 쓸 때만 한다.
        self._pre_buffer = deque()
        self._recording: Optional[_ActiveRecording] = None
        self._patrol_recording: Optional[_ActivePatrolRecording] = None

        VIDEO_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

        self.create_subscription(CompressedImage, CAMERA_TOPIC, self._on_frame, qos_profile_sensor_data)
        self.create_subscription(String, TRIGGER_TOPIC, self._on_trigger, 10)
        self.create_subscription(String, PATROL_RECORDING_TOPIC, self._on_patrol_trigger, 10)

        self.get_logger().info(
            f'video_recorder 시작: pre={self._pre_event_sec}s post={self._post_event_sec}s '
            f'출력={VIDEO_OUTPUT_DIR}'
        )

    # ---------- 카메라 프레임 ----------

    def _on_frame(self, msg: CompressedImage):
        now = time.monotonic()
        self._pre_buffer.append((now, bytes(msg.data)))
        self._trim_pre_buffer(now)

        if self._recording is not None:
            self._write_frame(self._recording, msg.data)
            if now >= self._recording.stop_at:
                self._finish_recording()

        if self._patrol_recording is not None:
            self._write_frame(self._patrol_recording, msg.data)

    def _trim_pre_buffer(self, now: float):
        if self._recording is not None:
            return
        cutoff = now - self._pre_event_sec
        while self._pre_buffer and self._pre_buffer[0][0] < cutoff:
            self._pre_buffer.popleft()

    # ---------- 이벤트 트리거 ----------

    def _on_trigger(self, msg: String):
        if self._recording is not None:
            self.get_logger().warn('이미 녹화 중이라 새 트리거를 무시합니다')
            return

        try:
            payload = json.loads(msg.data) if msg.data else {}
        except ValueError:
            self.get_logger().warn(f'트리거 JSON 파싱 실패, 빈 값으로 처리: {msg.data!r}')
            payload = {}

        care_event_id = payload.get('care_event_id')
        post_event_sec = float(payload.get('post_event_sec') or self._post_event_sec)

        if not self._pre_buffer:
            self.get_logger().warn(
                '버퍼에 프레임이 없어 pre 구간 없이 시작합니다 (카메라 토픽이 아직 안 들어왔나?)'
            )
            return

        self._start_recording(care_event_id, post_event_sec)

    def _start_recording(self, care_event_id, post_event_sec: float):
        pre_frames = list(self._pre_buffer)  # 스냅샷 — 이후 들어오는 프레임과 안 섞이게 복사
        fps = self._estimate_fps(pre_frames)

        first_frame = self._decode(pre_frames[0][1])
        if first_frame is None:
            self.get_logger().error('첫 프레임 디코딩에 실패해 녹화를 시작할 수 없습니다')
            return
        height, width = first_frame.shape[:2]

        clip_id = uuid.uuid4().hex
        path = VIDEO_OUTPUT_DIR / f'{clip_id}.mp4'
        writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*'mp4v'), fps, (width, height))
        if not writer.isOpened():
            self.get_logger().error(f'VideoWriter를 열지 못했습니다: {path}')
            return

        recording = _ActiveRecording(
            writer=writer, path=path, stop_at=time.monotonic() + post_event_sec,
            care_event_id=care_event_id, fps=fps,
        )

        for _, jpeg_bytes in pre_frames:
            frame = self._decode(jpeg_bytes)
            if frame is not None:
                writer.write(frame)
                recording.frame_count += 1

        self._recording = recording
        self.get_logger().info(
            f'녹화 시작: {path.name} (pre {len(pre_frames)}프레임, care_event_id={care_event_id})'
        )

    def _write_frame(self, recording: Union[_ActiveRecording, _ActivePatrolRecording], jpeg_data):
        frame = self._decode(bytes(jpeg_data))
        if frame is None:
            return
        recording.writer.write(frame)
        recording.frame_count += 1

    def _finish_recording(self):
        recording = self._recording
        self._recording = None
        recording.writer.release()

        duration_sec = round(recording.frame_count / recording.fps) if recording.fps > 0 else None
        sidecar = {
            'robot_code': ROBOT_CODE,
            'care_event_id': recording.care_event_id,
            'duration_sec': duration_sec,
            'captured_at': recording.captured_at.isoformat(),
            'original_filename': recording.path.name,
        }
        recording.path.with_suffix('.json').write_text(json.dumps(sidecar, ensure_ascii=False, indent=2))

        self.get_logger().info(
            f'녹화 종료: {recording.path.name} ({recording.frame_count}프레임, {duration_sec}s)'
        )

    # ---------- 순찰 연속 녹화 ----------

    def _on_patrol_trigger(self, msg: String):
        try:
            payload = json.loads(msg.data) if msg.data else {}
        except ValueError:
            self.get_logger().warn(f'순찰 녹화 트리거 JSON 파싱 실패, 무시합니다: {msg.data!r}')
            return

        action = payload.get('action')
        run_id = payload.get('run_id')
        if action == 'start':
            self._start_patrol_recording(run_id)
        elif action == 'stop':
            self._finish_patrol_recording()
        else:
            self.get_logger().warn(f'알 수 없는 순찰 녹화 action, 무시합니다: {action!r}')

    def _start_patrol_recording(self, run_id):
        if self._patrol_recording is not None:
            self.get_logger().warn(f'이미 순찰 녹화 중이라 새 시작 신호를 무시합니다(run_id={run_id})')
            return

        if not self._pre_buffer:
            self.get_logger().warn(
                '버퍼에 프레임이 없어 순찰 녹화를 시작할 수 없습니다 (카메라 토픽이 아직 안 들어왔나?)'
            )
            return

        recent_frames = list(self._pre_buffer)
        fps = self._estimate_fps(recent_frames)

        first_frame = self._decode(recent_frames[-1][1])
        if first_frame is None:
            self.get_logger().error('최신 프레임 디코딩에 실패해 순찰 녹화를 시작할 수 없습니다')
            return
        height, width = first_frame.shape[:2]

        clip_id = uuid.uuid4().hex
        path = VIDEO_OUTPUT_DIR / f'{clip_id}.mp4'
        writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*'mp4v'), fps, (width, height))
        if not writer.isOpened():
            self.get_logger().error(f'VideoWriter를 열지 못했습니다: {path}')
            return

        self._patrol_recording = _ActivePatrolRecording(writer=writer, path=path, run_id=run_id, fps=fps)
        self.get_logger().info(f'순찰 녹화 시작: {path.name} (run_id={run_id})')

    def _finish_patrol_recording(self):
        recording = self._patrol_recording
        if recording is None:
            self.get_logger().warn('순찰 녹화 종료 신호를 받았지만 진행 중인 녹화가 없어 무시합니다')
            return
        self._patrol_recording = None
        recording.writer.release()

        duration_sec = round(recording.frame_count / recording.fps) if recording.fps > 0 else None
        sidecar = {
            'robot_code': ROBOT_CODE,
            'care_event_id': None,
            'duration_sec': duration_sec,
            'captured_at': recording.captured_at.isoformat(),
            'original_filename': recording.path.name,
        }
        recording.path.with_suffix('.json').write_text(json.dumps(sidecar, ensure_ascii=False, indent=2))

        self.get_logger().info(
            f'순찰 녹화 종료: {recording.path.name} ({recording.frame_count}프레임, {duration_sec}s, '
            f'run_id={recording.run_id})'
        )

    # ---------- 유틸 ----------

    @staticmethod
    def _decode(jpeg_bytes: bytes):
        array = np.frombuffer(jpeg_bytes, dtype=np.uint8)
        frame = cv2.imdecode(array, cv2.IMREAD_COLOR)
        if frame is not None and CAMERA_FLIP_VERTICAL:
            frame = cv2.flip(frame, 0)
        return frame

    @staticmethod
    def _estimate_fps(pre_frames) -> float:
        if len(pre_frames) < 2:
            return 10.0
        span = pre_frames[-1][0] - pre_frames[0][0]
        if span <= 0:
            return 10.0
        return (len(pre_frames) - 1) / span


def main(args=None):
    rclpy.init(args=args)
    node = VideoRecorder()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
