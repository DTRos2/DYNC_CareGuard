"""careguard_agent/media_uploader.py

완성된 SLAM 맵 이미지, 그리고 이벤트 전후 녹화 클립(video_recorder.py 결과물)을
서버에 업로드한다.

맵: POST /media/upload-map(멀티파트: robot_code/file)로 files 행을
entity_type='map_image'로 만든 뒤, 그 file_id를 slam_saved WS 메시지에 실어
보낸다. app/ws/robot_ws.py::_save_slam_result가 그 file_id로 files.entity_id를
새 map.id로 다시 연결한다.

(예전엔 맵 전용 엔드포인트가 없어 영상용 /media/upload를 임시로 재사용했는데,
그 경로는 entity_type을 'event_clip'으로 고정하고 camera_recording에도 불필요한
행을 만들어 이후 entity_type='map_image' 조건의 재연결이 조용히 실패했다 —
/media/upload-map로 교체해 해결)

영상: POST /media/upload로 files + camera_recording 행을 만든다.
start_video_upload_watcher()가 video_recorder.py 결과물(mp4+json)을 백그라운드
스레드로 주기적으로 찾아 업로드한다 — uplink_node.py가 시작 시 한 번만 부른다.

빌드/실행 미검증.
"""
import json
import threading
import time
from pathlib import Path
from typing import Any, Dict

import requests

from . import ws_config

# video_recorder.py가 만든 사이드카를 얼마나 자주 훑어볼지.
VIDEO_SCAN_INTERVAL_SEC = 5.0


def upload_map_image(image_path: str, robot_code: str = None) -> int:
    """map.pgm(또는 png)을 업로드하고 files.id를 반환한다.

    robot_code를 안 넘기면 이 로봇 자신의 ws_config.ROBOT_CODE를 쓴다 — 서버가
    robot_id를 따로 알려줄 필요가 없다(대시보드도 robot_id를 모르는 채로 같은
    엔드포인트를 쓸 수 있게 하려고 robot_id 대신 robot_code를 받도록 맞췄다).
    """
    with open(image_path, 'rb') as f:
        response = requests.post(
            f'{ws_config.SERVER_HTTP_URL}/media/upload-map',
            data={'robot_code': robot_code or ws_config.ROBOT_CODE},
            files={'file': f},
            timeout=30,
        )
    response.raise_for_status()
    return response.json()['file_id']


def upload_video_clip(video_path: Path, metadata: Dict[str, Any]) -> Dict[str, Any]:
    """video_recorder.py가 만든 mp4를 사이드카 메타데이터(dict)와 함께 업로드한다.

    metadata는 .json 사이드카 내용 그대로(robot_code, care_event_id,
    duration_sec). None인 필드는 폼에서 뺀다.
    """
    data = {'robot_code': metadata.get('robot_code') or ws_config.ROBOT_CODE}
    if metadata.get('care_event_id') is not None:
        data['care_event_id'] = metadata['care_event_id']
    if metadata.get('duration_sec') is not None:
        data['duration_sec'] = metadata['duration_sec']

    with open(video_path, 'rb') as f:
        response = requests.post(
            f'{ws_config.SERVER_HTTP_URL}/media/upload',
            data=data,
            files={'file': (video_path.name, f)},
            timeout=60,
        )
    response.raise_for_status()
    return response.json()


def start_video_upload_watcher(logger=None, interval_sec: float = VIDEO_SCAN_INTERVAL_SEC) -> threading.Thread:
    """ws_config.VIDEO_DIR을 주기적으로 스캔해 업로드하는 데몬 스레드를 시작한다.

    logger에 self.get_logger()를 넘기면 그걸로 로깅하고, 안 넘기면 print로 대체한다.
    """
    thread = threading.Thread(target=_watch_loop, args=(logger, interval_sec), daemon=True)
    thread.start()
    return thread


def _watch_loop(logger, interval_sec: float):
    while True:
        try:
            _scan_once(logger)
        except Exception as exc:  # noqa: BLE001 - 워처 스레드는 절대 죽으면 안 된다
            _log(logger, 'error', f'업로드 워처에서 예외 발생: {exc}')
        time.sleep(interval_sec)


def _scan_once(logger):
    if not ws_config.VIDEO_DIR.is_dir():
        return
    for sidecar_path in sorted(ws_config.VIDEO_DIR.glob('*.json')):
        _process_one(sidecar_path, logger)


def _process_one(sidecar_path: Path, logger):
    """.json 사이드카 하나를 업로드하고 성공하면 mp4+json을 지운다."""
    video_path = sidecar_path.with_suffix('.mp4')
    if not video_path.is_file():
        _log(logger, 'warn', f'사이드카는 있는데 영상 파일이 없습니다: {video_path.name}')
        return

    try:
        metadata = json.loads(sidecar_path.read_text())
    except ValueError as exc:
        _log(logger, 'error', f'사이드카 JSON 파싱 실패({sidecar_path.name}): {exc}')
        return

    try:
        result = upload_video_clip(video_path, metadata)
    except Exception as exc:  # noqa: BLE001 - 실패해도 다음 스캔에서 재시도해야 하므로 삼킨다
        _log(logger, 'warn', f'업로드 실패, 다음 스캔에서 재시도합니다({video_path.name}): {exc}')
        return

    _log(logger, 'info', f'업로드 완료: {video_path.name} -> camera_recording_id={result.get("camera_recording_id")}')
    # 서버가 이미 독립된 사본을 저장했으니 로봇 로컬 원본은 지워도 안전하다.
    video_path.unlink(missing_ok=True)
    sidecar_path.unlink(missing_ok=True)


def _log(logger, level: str, message: str):
    if logger is None:
        print(f'[media_uploader] {message}')
        return
    getattr(logger, level)(message)
