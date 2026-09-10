"""careguard_perception/video_recorder.py

이벤트 전후(pre+post) 녹화 클립과, 순찰 시작~충전소 복귀까지 이어지는 연속
녹화를 모두 만드는 노드. 이벤트 클립은 트리거 전 N초를 링버퍼로 들고 있다가
트리거가 오면 그 버퍼 + 이후 M초를 이어붙이고, 순찰 녹화는 pre-roll 없이
시작 신호를 받은 시점부터 종료 신호까지 그대로 이어 쓴다. 두 녹화는 서로 다른
슬롯(_recording / _patrol_recording)으로 관리해 순찰 중 이벤트가 터져도(또는
그 반대도) 둘 다 동시에 진행된다.

카메라는 uplink_node.py가 이미 쓰는 /camera/color/image_raw/compressed(JPEG
압축)를 그대로 구독한다 — 별도 cv2.VideoCapture는 열지 않는다.

이벤트 트리거는 /careguard/perception/record_trigger(std_msgs/String, JSON)로
받는다. 아직 실제 발행 노드가 없어 수동 테스트용 예시:

    ros2 topic pub --once /careguard/perception/record_trigger std_msgs/msg/String \
        "data: '{\"care_event_id\": null, \"reason\": \"manual test\"}'"

payload에 severity("emergency"/"attention")가 실려 오면, 녹화를 시작하기 직전에
careguard_hmi/audio_player.py가 재생하는 /careguard/voice/announce로 "긴급/주의
이벤트 발생! 영상 녹화를 시작합니다" 안내를 먼저 쏜다(_ANNOUNCE_TEXT 참고).
audio_player는 downlink_node.py의 _ensure_voice_announcer_running()이 nav_prepare/
start_patrol 시점에 미리 띄워두므로, 순찰 중 발생하는 이벤트라면 이미 떠있다:

    ros2 topic pub --once /careguard/perception/record_trigger std_msgs/msg/String \
        "data: '{\"care_event_id\": 4, \"severity\": \"emergency\", \"reason\": \"manual test\"}'"

순찰 녹화 시작/종료는 /careguard/perception/patrol_recording(std_msgs/String,
JSON)로 받는다. careguard_mission/patrol_task.py가 순찰 시작 직후와 끝(성공/
실패 불문)에 발행한다:

    ros2 topic pub --once /careguard/perception/patrol_recording std_msgs/msg/String \
        "data: '{\"action\": \"start\", \"run_id\": 1}'"
    ros2 topic pub --once /careguard/perception/patrol_recording std_msgs/msg/String \
        "data: '{\"action\": \"stop\", \"run_id\": 1}'"

결과물은 둘 다 <output_dir>/<uuid>.mp4 + 같은 이름의 .json 사이드카(순찰
클립은 care_event_id를 비워서 media.py가 자동으로 "순찰" 태그를 붙이게 한다).
업로드(careguard_agent/media_uploader.py)는 이 노드가 아니라 uplink_node가 맡는다.

"""
import json
import time
import uuid
from collections import deque
from datetime import datetime, timezone
from typing import Optional, Union

import cv2
import numpy as np
import rclpy
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage
from std_msgs.msg import String

from . import config

# severity -> 음성 안내 문구. media.py의 SEVERITY_TAGS(media.py:37)와 같은 값 집합을
# 쓴다 — 목록에 없는 값(info 등)이나 severity가 아예 없는 트리거는 조용히 안내를 건너뛴다.
_ANNOUNCE_TEXT = {
    'emergency': '긴급 이벤트 발생! 영상 녹화를 시작합니다',
    'attention': '주의 이벤트 발생! 영상 녹화를 시작합니다',
}


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
    """순찰 시작~충전소 복귀까지 이어지는 연속 녹화 하나의 상태. _ActiveRecording과
    달리 stop_at(타이머 종료 시각)이 없다 — patrol_task.py의 명시적 stop 신호가
    올 때까지 무기한 이어 쓴다. pre-roll도 없다(트리거 전 구간을 미리 채워 넣는
    이벤트 클립과 달리, 순찰은 시작 신호가 온 바로 그 순간부터 찍으면 된다)."""

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

        self._pre_event_sec = float(
            self.declare_parameter('pre_event_sec', config.PRE_EVENT_SEC_DEFAULT).value
        )
        self._post_event_sec = float(
            self.declare_parameter('post_event_sec', config.POST_EVENT_SEC_DEFAULT).value
        )

        # (monotonic 타임스탬프, jpeg 원본 바이트) 링버퍼. 디코딩은 쓸 때만 한다.
        self._pre_buffer = deque()
        self._recording: Optional[_ActiveRecording] = None
        # 이벤트 클립(_recording)과 별개 슬롯 — 순찰 연속 녹화 중에 이벤트가 터져도
        # 둘 다 동시에 진행돼야 하므로 하나로 합치지 않는다.
        self._patrol_recording: Optional[_ActivePatrolRecording] = None

        config.VIDEO_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

        self.create_subscription(
            CompressedImage, config.CAMERA_TOPIC, self._on_frame, qos_profile_sensor_data
        )
        self.create_subscription(String, config.TRIGGER_TOPIC, self._on_trigger, 10)
        self.create_subscription(
            String, config.PATROL_RECORDING_TOPIC, self._on_patrol_trigger, 10
        )
        # patrol_task.py(_publish_voice)와 같은 패턴 — "완성된 문장을 그대로 받는" 토픽이라
        # 여기서 문구만 만들어 그대로 발행하면 된다.
        self._voice_pub = self.create_publisher(String, config.VOICE_TOPIC, 10)

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

        # 이벤트 클립과 독립적으로 진행 — 순찰 녹화 슬롯이 채워져 있으면 같은
        # 프레임을 그쪽 writer에도 그대로 이어 쓴다.
        if self._patrol_recording is not None:
            self._write_frame(self._patrol_recording, msg.data)

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

        self._announce_severity(payload.get('severity'))
        self._start_recording(care_event_id, post_event_sec)

    def _announce_severity(self, severity):
        text = _ANNOUNCE_TEXT.get(severity)
        if text is None:
            return  # severity 없음(순찰과 무관한 옛 방식 호출) 또는 안내 대상이 아닌 값
        msg = String()
        msg.data = json.dumps({'text': text})
        self._voice_pub.publish(msg)

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
            # patrol_task.py의 finally 블록이 안전망으로 stop을 중복 발행할 수 있는
            # 것처럼, start도 재시도 등으로 중복 도착할 수 있다 — 이미 진행 중이면
            # 기존 녹화를 그대로 이어가고 새로 시작하지 않는다(이벤트 클립의
            # "이미 녹화 중" 처리와 같은 방어).
            self.get_logger().warn(f'이미 순찰 녹화 중이라 새 시작 신호를 무시합니다(run_id={run_id})')
            return

        if not self._pre_buffer:
            self.get_logger().warn(
                '버퍼에 프레임이 없어 순찰 녹화를 시작할 수 없습니다 (카메라 토픽이 아직 안 들어왔나?)'
            )
            return

        # pre-roll이 없으므로(이벤트 클립과 다름) 지금 막 들어온 최신 프레임 하나로
        # 크기만 확인한다. fps는 그래도 최근 버퍼 간격으로 추정해야 실제 재생 속도와
        # 맞는 영상이 나온다.
        recent_frames = list(self._pre_buffer)
        fps = self._estimate_fps(recent_frames)

        first_frame = self._decode(recent_frames[-1][1])
        if first_frame is None:
            self.get_logger().error('최신 프레임 디코딩에 실패해 순찰 녹화를 시작할 수 없습니다')
            return
        height, width = first_frame.shape[:2]

        clip_id = uuid.uuid4().hex
        path = config.VIDEO_OUTPUT_DIR / f'{clip_id}.mp4'
        writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*'mp4v'), fps, (width, height))
        if not writer.isOpened():
            self.get_logger().error(f'VideoWriter를 열지 못했습니다: {path}')
            return

        self._patrol_recording = _ActivePatrolRecording(writer=writer, path=path, run_id=run_id, fps=fps)
        self.get_logger().info(f'순찰 녹화 시작: {path.name} (run_id={run_id})')

    def _finish_patrol_recording(self):
        recording = self._patrol_recording
        if recording is None:
            # goal_handle이 abort된 직후 finally에서 또 stop을 보내는 등, start 없이
            # stop만 오는 경우도 있을 수 있다 — 조용히 무시한다.
            self.get_logger().warn('순찰 녹화 종료 신호를 받았지만 진행 중인 녹화가 없어 무시합니다')
            return
        self._patrol_recording = None
        recording.writer.release()

        duration_sec = round(recording.frame_count / recording.fps) if recording.fps > 0 else None
        sidecar = {
            'robot_code': config.ROBOT_CODE,
            # care_event_id를 비워두면 media.py가 자동으로 "순찰" 태그·7일 보존
            # 정책(RETENTION_DAYS_PATROL_SNAPSHOT)으로 처리한다.
            'care_event_id': None,
            'duration_sec': duration_sec,
            'captured_at': recording.captured_at.isoformat(),
            'original_filename': recording.path.name,
        }
        sidecar_path = recording.path.with_suffix('.json')
        sidecar_path.write_text(json.dumps(sidecar, ensure_ascii=False, indent=2))

        self.get_logger().info(
            f'순찰 녹화 종료: {recording.path.name} ({recording.frame_count}프레임, {duration_sec}s, '
            f'run_id={recording.run_id}) -> {sidecar_path.name} 기록'
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
