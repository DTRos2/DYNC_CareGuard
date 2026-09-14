import os
from pathlib import Path

from dotenv import load_dotenv

load_dotenv()

POSTGRES_HOST = os.getenv("POSTGRES_HOST", "localhost")
POSTGRES_PORT = os.getenv("POSTGRES_PORT", "5433")
POSTGRES_DB = os.getenv("POSTGRES_DB", "careguard")
POSTGRES_USER = os.getenv("POSTGRES_USER", "careguard")
POSTGRES_PASSWORD = os.getenv("POSTGRES_PASSWORD", "careguard")

# ESP32 응급 호출 버튼이 발행하는 MQTT 브로커 주소. deploy/docker-compose.yml의
# mosquitto 서비스를 로컬 개발 기본값(localhost:1883)으로 가정한다.
MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

# 업로드된 영상/이미지 파일을 저장할 루트 경로. 기본값은 server/storage.
# 이 파일이 server/app/core/config.py라서 server/ 루트까지 올라가려면 parent가
# 3번 필요하다(core -> app -> server) — 2026-09-09 리팩터로 config.py가 app/
# 밑에서 app/core/ 밑으로 한 단계 더 들어가면서 parent.parent(2번)로는 server/app/
# storage(빈 폴더)를 가리키게 됐고, 실제 업로드 파일이 있는 server/storage는
# 조용히 못 찾는 상태였다.
STORAGE_DIR = Path(os.getenv("STORAGE_DIR", str(Path(__file__).resolve().parent.parent.parent / "storage")))
STORAGE_DIR.mkdir(parents=True, exist_ok=True)
