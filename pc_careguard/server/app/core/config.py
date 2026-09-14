import os
from pathlib import Path

from dotenv import load_dotenv

load_dotenv()

POSTGRES_HOST = os.getenv("POSTGRES_HOST", "localhost")
POSTGRES_PORT = os.getenv("POSTGRES_PORT", "5433")
POSTGRES_DB = os.getenv("POSTGRES_DB", "careguard")
POSTGRES_USER = os.getenv("POSTGRES_USER", "careguard")
POSTGRES_PASSWORD = os.getenv("POSTGRES_PASSWORD", "careguard")

# 업로드된 영상/이미지 파일을 저장할 루트 경로. 기본값은 server/storage.
# 이 파일이 server/app/core/config.py라서 server/ 루트까지 올라가려면 parent가
# 3번 필요하다(core -> app -> server) — 2026-09-09 리팩터로 config.py가 app/
# 밑에서 app/core/ 밑으로 한 단계 더 들어가면서 parent.parent(2번)로는 server/app/
# storage(빈 폴더)를 가리키게 됐고, 실제 업로드 파일이 있는 server/storage는
# 조용히 못 찾는 상태였다.
STORAGE_DIR = Path(os.getenv("STORAGE_DIR", str(Path(__file__).resolve().parent.parent.parent / "storage")))
STORAGE_DIR.mkdir(parents=True, exist_ok=True)
