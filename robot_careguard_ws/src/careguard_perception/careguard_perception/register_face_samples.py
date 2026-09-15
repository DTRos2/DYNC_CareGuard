#!/usr/bin/env python3
"""careguard_perception/register_face_samples.py

관리자용 1회성 CLI 도구. auto_face_capture_node.py로 로컬에 모아둔 얼굴 촬영본
(YOLO 라벨 포함, images/{split}/{person_label}_%04d.jpg + labels/{split}/*.txt)을
특정 recipient.id에 연결된 "등록 얼굴 샘플"로 서버 DB에 반영한다.

person_A/person_B 같은 촬영 세션 라벨과 실제 recipient.id는 서로 다른 개념이다
(촬영은 대상자 등록 전에도 할 수 있고, 같은 person_label을 재사용해 여러 세션을
찍을 수도 있음) — 이 스크립트가 그 둘을 연결하는 유일한 지점이다. 한 recipient당
한 번만 실행하는 게 정상 사용 흐름이며, 재실행 시 중복 등록을 막기 위한 멱등성
로직은 없다(관리자가 직접 확인 후 실행하는 소규모 1회성 도구 스코프 — 필요해지면
files 행 개수를 먼저 조회해 경고하는 정도만 추가하면 됨).

크롭 방식은 face_embedding_diagnostic.py(학습/진단 파이프라인)와 동일하게 맞춘다
(라벨 bbox + CROP_MARGIN 여유 + EMBED_SIZE로 PIL 기본(BICUBIC) 리사이즈) — 실시간
노드(face_identify_test_node.embed_crop)와도 이미 정합이 맞춰져 있는 값이라
build_person_centroids.py가 이 결과물을 그대로 재사용할 수 있다. 즉 여기서 저장하는
파일은 "원본 사진"이 아니라 "이미 크롭+리사이즈된 얼굴 이미지"다.

files 테이블에는 entity_type='recipient_face_sample', entity_id=str(recipient_id)로
기록한다(스키마 코멘트 참고, CHECK 제약 없는 자유 문자열).

사용법:
    <venv>/bin/python3 register_face_samples.py --recipient-id 12 --person-label person_A

DB 연결 정보는 서버(app/core/config.py)와 같은 환경변수 이름을 그대로 쓴다
(POSTGRES_HOST/PORT/DB/USER/PASSWORD, STORAGE_DIR) — 서버를 이미 띄워본 환경이면
새로 외울 이름이 없다.

빌드/실행 미검증.
"""
import argparse
import os
import sys
import uuid
from pathlib import Path

import psycopg2
import psycopg2.extras
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_embedding_diagnostic import CROP_MARGIN, EMBED_SIZE, yolo_label_to_pixel_bbox  # noqa: E402  (재사용)

POSTGRES_HOST = os.getenv("POSTGRES_HOST", "localhost")
POSTGRES_PORT = os.getenv("POSTGRES_PORT", "5433")
POSTGRES_DB = os.getenv("POSTGRES_DB", "careguard")
POSTGRES_USER = os.getenv("POSTGRES_USER", "careguard")
POSTGRES_PASSWORD = os.getenv("POSTGRES_PASSWORD", "careguard")

# app/core/config.py::STORAGE_DIR 기본값(pc_careguard/server/storage)과 같은 값을
# 이 monorepo 상대경로로 재현한다 — 서버와 같은 머신에서 이 checkout으로 돌리는
# 관리자라면 STORAGE_DIR을 안 정해줘도 그대로 맞는다. 다른 머신이면 STORAGE_DIR
# 환경변수로 서버의 실제 storage 경로를 지정해야 한다.
_DEFAULT_STORAGE_DIR = (
    Path(__file__).resolve().parents[4] / "pc_careguard" / "server" / "storage"
)
STORAGE_DIR = Path(os.getenv("STORAGE_DIR", str(_DEFAULT_STORAGE_DIR))).expanduser()
FACE_SAMPLE_SUBDIR = "recipient_faces"
ENTITY_TYPE = "recipient_face_sample"


def get_connection():
    return psycopg2.connect(
        host=POSTGRES_HOST, port=POSTGRES_PORT, dbname=POSTGRES_DB,
        user=POSTGRES_USER, password=POSTGRES_PASSWORD,
        cursor_factory=psycopg2.extras.RealDictCursor,
    )


def _iter_captures(face_dataset_dir: Path, person_label: str):
    """auto_face_capture_node.py 결과물(images/{split}/{person_label}_%04d.jpg +
    같은 stem의 labels/{split}/*.txt)을 순회한다."""
    for split in ("train", "val"):
        img_dir = face_dataset_dir / "images" / split
        lbl_dir = face_dataset_dir / "labels" / split
        if not img_dir.is_dir():
            continue
        for img_path in sorted(img_dir.glob(f"{person_label}_*.jpg")):
            lbl_path = lbl_dir / f"{img_path.stem}.txt"
            if not lbl_path.is_file():
                print(f"  건너뜀(라벨 없음): {img_path.name}")
                continue
            yield img_path, lbl_path


def register(recipient_id: int, person_label: str, face_dataset_dir: Path) -> int:
    dest_dir = STORAGE_DIR / FACE_SAMPLE_SUBDIR / str(recipient_id)
    dest_dir.mkdir(parents=True, exist_ok=True)

    registered = 0
    with get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT id FROM recipient WHERE id = %s AND deregistered_at IS NULL", (recipient_id,)
        )
        if cur.fetchone() is None:
            raise SystemExit(f"recipient_id={recipient_id}를 찾을 수 없습니다(등록 해제됐거나 오타).")

        for img_path, lbl_path in _iter_captures(face_dataset_dir, person_label):
            lines = [ln.strip() for ln in lbl_path.read_text().splitlines() if ln.strip()]
            if len(lines) != 1:
                print(f"  건너뜀(라벨 {len(lines)}줄, 1줄이어야 함): {img_path.name}")
                continue

            with Image.open(img_path) as im:
                im = im.convert("RGB")
                img_w, img_h = im.size
                _class_id, (x1, y1, x2, y2) = yolo_label_to_pixel_bbox(lines[0], img_w, img_h, CROP_MARGIN)
                crop = im.crop((int(x1), int(y1), int(x2), int(y2))).resize((EMBED_SIZE, EMBED_SIZE))

                dest_name = f"{uuid.uuid4().hex}.jpg"
                crop.save(dest_dir / dest_name, "JPEG", quality=95)

            storage_path = str((dest_dir / dest_name).relative_to(STORAGE_DIR))
            cur.execute(
                """
                INSERT INTO files (entity_type, entity_id, storage_path, original_filename)
                VALUES (%s, %s, %s, %s)
                """,
                (ENTITY_TYPE, str(recipient_id), storage_path, img_path.name),
            )
            registered += 1

        if registered > 0:
            cur.execute(
                "UPDATE recipient SET face_registered = TRUE WHERE id = %s", (recipient_id,)
            )
        conn.commit()

    return registered


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipient-id", type=int, required=True)
    parser.add_argument("--person-label", required=True, help="예: person_A (촬영 시 쓴 라벨)")
    parser.add_argument(
        "--face-dataset-dir",
        default=str(Path.home() / "careguard_data" / "face_dataset"),
        help="auto_face_capture_node.py의 save_dir과 동일 경로",
    )
    args = parser.parse_args()

    print(f"STORAGE_DIR={STORAGE_DIR}")
    count = register(args.recipient_id, args.person_label, Path(args.face_dataset_dir).expanduser())
    print(f"완료: recipient_id={args.recipient_id}에 샘플 {count}장 등록, face_registered=TRUE로 갱신")


if __name__ == "__main__":
    main()
