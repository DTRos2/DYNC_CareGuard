"""careguard_agent/media_uploader.py

완성된 SLAM 맵 이미지를 서버에 업로드한다. POST /media/upload-map(멀티파트:
robot_id/file)로 files 행을 entity_type='map_image'로 만든 뒤, 그 file_id를
slam_saved WS 메시지에 실어 보낸다. app/ws/robot_ws.py::_save_slam_result가
그 file_id로 files.entity_id를 새 map.id로 다시 연결한다.

(예전엔 맵 전용 엔드포인트가 없어 영상용 /media/upload를 임시로 재사용했는데,
그 경로는 entity_type을 'event_clip'으로 고정하고 camera_recording에도 불필요한
행을 만들어 이후 entity_type='map_image' 조건의 재연결이 조용히 실패했다 —
/media/upload-map로 교체해 해결)

빌드/실행 미검증.
"""
import requests

from . import ws_config


def upload_map_image(image_path: str, robot_id: int) -> int:
    """map.pgm(또는 png)을 업로드하고 files.id를 반환한다."""
    with open(image_path, 'rb') as f:
        response = requests.post(
            f'{ws_config.SERVER_HTTP_URL}/media/upload-map',
            data={'robot_id': robot_id},
            files={'file': f},
            timeout=30,
        )
    response.raise_for_status()
    return response.json()['file_id']
