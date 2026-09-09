"""careguard_perception/video_recorder.py

이벤트 전후(pre+post) 녹화 클립을 만드는 노드. 트리거 전 N초를 링버퍼로 들고
있다가, 트리거가 오면 그 버퍼 + 이후 M초를 이어붙여 파일로 저장한다.

카메라는 uplink_node.py가 이미 쓰는 /camera/color/image_raw/compressed(JPEG
압축)를 그대로 구독한다 — 별도 cv2.VideoCapture는 열지 않는다.

트리거는 /careguard/perception/record_trigger(std_msgs/String, JSON)로 받는다.
아직 실제 발행 노드가 없어 수동 테스트용 예시:

    ros2 topic pub --once /careguard/perception/record_trigger std_msgs/msg/String \
        "data: '{\"care_event_id\": null, \"reason\": \"manual test\"}'"

결과물은 <output_dir>/<uuid>.mp4 + 같은 이름의 .json 사이드카. 업로드
(careguard_agent/media_uploader.py)는 이 노드가 아니라 uplink_node가 맡는다.

"""
import json
import time
import uuid
from collections import deque
from datetime import datetime, timezone
from typing import Optional

import cv2
import numpy as np
import rclpy
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage
from std_msgs.msg import String

from . import config


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


class VideoRecorder(Node):
    def __init__(self):
        super().__init__('video_recorder')

        self._pre_event_sec = float(
            self.declare_parameter('pre_event_sec', config.PRE_EVENT_SEC_DEFAULT).value
        )
        self._post_event_sec = float(
            self.declare_parameter('post_event_sec', config.POST_EVENT_SEC_DEFAULT).value
        )

        # (monotonic 타임스탬프, jpeg 원본 바이트) 링버퍼. 디코딩은 쓸 때만 한다.
        self._pre_buffer = deque()
        self._recording: Optional[_ActiveRecording] = None

        config.VIDEO_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

        self.create_subscription(
            CompressedImage, config.CAMERA_TOPIC, self._on_frame, qos_profile_sensor_data
        )
        self.create_subscription(String, config.TRIGGER_TOPIC, self._on_trigger, 10)

        self.get_logger().info(
            f'video_recorder 시작: pre={self._pre_event_sec}s post={self._post_event_sec}s '
            f'출력={config.VIDEO_OUTPUT_DIR}'
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

    def _trim_pre_buffer(self, now: float):
        # 녹화 중엔 pre 구간을 이미 다 썼으니 더 안 다듬음 
        if self._recording is not None:
            return
        cutoff = now - self._pre_event_sec
        while self._pre_buffer and self._pre_buffer[0][0] < cutoff:
            self._pre_buffer.popleft()

    # ---------- 트리거 ----------

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
        path = config.VIDEO_OUTPUT_DIR / f'{clip_id}.mp4'
        writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*'mp4v'), fps, (width, height))
        if not writer.isOpened():
            self.get_logger().error(f'VideoWriter를 열지 못했습니다: {path}')
            return

        recording = _ActiveRecording(
            writer=writer,
            path=path,
            stop_at=time.monotonic() + post_event_sec,
            care_event_id=care_event_id,
            fps=fps,
        )

        # pre 버퍼는 지금 한 번에 다 써버린다 — 이후 실시간 프레임은 _on_frame이 이어 씀
        for _, jpeg_bytes in pre_frames:
            frame = self._decode(jpeg_bytes)
            if frame is not None:
                writer.write(frame)
                recording.frame_count += 1

        self._recording = recording
        self.get_logger().info(
            f'녹화 시작: {path.name} (pre {len(pre_frames)}프레임, care_event_id={care_event_id})'
        )

    def _write_frame(self, recording: _ActiveRecording, jpeg_data):
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
            'robot_code': config.ROBOT_CODE,
            'care_event_id': recording.care_event_id,
            'duration_sec': duration_sec,
            'captured_at': recording.captured_at.isoformat(),
            'original_filename': recording.path.name,
        }
        sidecar_path = recording.path.with_suffix('.json')
        sidecar_path.write_text(json.dumps(sidecar, ensure_ascii=False, indent=2))

        self.get_logger().info(
            f'녹화 종료: {recording.path.name} ({recording.frame_count}프레임, {duration_sec}s) '
            f'-> {sidecar_path.name} 기록'
        )

    # ---------- 유틸 ----------

    @staticmethod
    def _decode(jpeg_bytes: bytes):
        array = np.frombuffer(jpeg_bytes, dtype=np.uint8)
        return cv2.imdecode(array, cv2.IMREAD_COLOR)

    @staticmethod
    def _estimate_fps(pre_frames) -> float:
        """버퍼 프레임 간격으로 실제 fps를 추정한다. 프레임 2개 미만이면 10fps 기본값."""
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
