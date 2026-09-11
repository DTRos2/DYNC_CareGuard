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
STORAGE_DIR = Path(os.getenv("STORAGE_DIR", str(Path(__file__).resolve().parent.parent / "storage")))
STORAGE_DIR.mkdir(parents=True, exist_ok=True)
