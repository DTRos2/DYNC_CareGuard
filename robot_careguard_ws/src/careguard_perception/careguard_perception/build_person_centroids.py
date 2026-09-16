#!/usr/bin/env python3
"""careguard_perception/build_person_centroids.py

recipient.id 기준 등록(enrollment) centroid 임베딩을 만든다.

이전 버전은 person_A/person_B라는 촬영 세션 라벨을 그대로 identity 키로 썼는데,
그 라벨과 실제 대상자(recipient)를 연결할 방법이 DB 어디에도 없었다(person_A가
recipient.id=12인지 27인지 코드로는 알 수 없음). register_face_samples.py가 그
연결을 files(entity_type=recipient_face_sample, entity_id=recipient.id)로 미리
만들어두므로, 이 스크립트는 recipient 테이블 기준으로 순회하며 그 recipient의
샘플만 모아 centroid를 계산한다 — person_A/B 문자열은 더 이상 어디에도 안 남는다.

recipient_face_sample 파일들은 register_face_samples.py가 이미 crop+resize(160x160,
PIL BICUBIC)까지 마친 상태로 저장해뒀으므로, 여기서는 그 이미지를 그대로 불러와
embed_crops()에 넘기기만 하면 된다(face_embedding_diagnostic.py의 crop 파이프라인과
자동으로 정합이 맞음 — 새로 안 짬).

--robot-code를 주면 그 로봇에 배정된(recipient.robot_id) 대상자만 포함한 centroid
파일을 만든다 — 로봇에는 "자기 담당 대상자"만 들어간 파일을 scp해서, 런타임에 DB
조회 없이 "이 recipient_id가 파일에 있으면 내 담당"으로 바로 판별하게 하기 위함
(patrol_intruder_watch_node.py가 이렇게 쓴다). 생략하면 얼굴 등록이 끝난 대상자
전원을 담는다(디버깅/전체 확인용).

출력 포맷 (이전 버전과 다름 — person_A/B 키가 아니라 recipient_id 문자열 키):
    {
        "centroids": {"12": Tensor(512,), "27": Tensor(512,), ...},
        "names": {"12": "홍길동", "27": "김영희", ...},
        "counts": {"12": 8, "27": 6, ...},
        "adapter_ckpt": str,
        "robot_code": str | None,
    }

사용법:
    <venv>/bin/python3 build_person_centroids.py --robot-code R-01
    <venv>/bin/python3 build_person_centroids.py  # 전체 등록자
"""
import argparse
import os
import sys
from pathlib import Path

import numpy as np
import psycopg2
import psycopg2.extras
import torch
import torch.nn.functional as F
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_embedding_diagnostic import build_embedder, embed_crops  # noqa: E402  (재사용 — 새로 안 짬)
from face_embedding_adapter_model import ADAPTER_PATH, ResidualAdapter  # noqa: E402
from register_face_samples import ENTITY_TYPE, STORAGE_DIR, get_connection  # noqa: E402  (DB/저장소 설정 재사용)

DEVICE = "cuda" if torch.cuda.is_available() else "cpu"
OUT_PATH = ADAPTER_PATH.parent / "person_centroids.pt"


def _fetch_recipients(cur, robot_code):
    if robot_code:
        cur.execute(
            """
            SELECT rc.id, rc.name FROM recipient rc
            JOIN robot r ON r.id = rc.robot_id
            WHERE rc.face_registered = TRUE AND rc.deregistered_at IS NULL
              AND r.code = %s AND r.deregistered_at IS NULL
            """,
            (robot_code,),
        )
    else:
        cur.execute(
            "SELECT id, name FROM recipient WHERE face_registered = TRUE AND deregistered_at IS NULL"
        )
    return cur.fetchall()


def _load_samples(cur, recipient_id):
    cur.execute(
        """
        SELECT storage_path FROM files
        WHERE entity_type = %s AND entity_id = %s AND deleted_at IS NULL
        ORDER BY display_order, id
        """,
        (ENTITY_TYPE, str(recipient_id)),
    )
    crops = []
    for row in cur.fetchall():
        path = STORAGE_DIR / row["storage_path"]
        if not path.is_file():
            print(f"  경고: 파일 없음(건너뜀) — {path}")
            continue
        with Image.open(path) as im:
            crops.append({"image": im.convert("RGB").copy()})
    return crops


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--robot-code", default=None, help="이 로봇에 배정된 대상자만 포함(생략 시 전체)")
    args = parser.parse_args()

    print(f"DB 연결... STORAGE_DIR={STORAGE_DIR}")
    with get_connection() as conn, conn.cursor() as cur:
        recipients = _fetch_recipients(cur, args.robot_code)
        if not recipients:
            scope = f"로봇 {args.robot_code}" if args.robot_code else "전체"
            raise SystemExit(f"{scope}에 얼굴 등록(face_registered=TRUE)된 대상자가 없습니다.")

        print(f"대상자 {len(recipients)}명: " + ", ".join(f"{r['name']}(id={r['id']})" for r in recipients))

        print("\nFaceNet(frozen) 로딩...")
        model, standardize = build_embedder()

        print(f"adapter 로딩: {ADAPTER_PATH}")
        ckpt = torch.load(ADAPTER_PATH, map_location=DEVICE)
        adapter = ResidualAdapter(dim=ckpt["dim"], bottleneck=ckpt["bottleneck"]).to(DEVICE)
        adapter.load_state_dict(ckpt["state_dict"])
        adapter.eval()

        centroids, names, counts = {}, {}, {}
        for r in recipients:
            rid_key = str(r["id"])
            crops = _load_samples(cur, r["id"])
            if not crops:
                print(f"  경고: {r['name']}(id={r['id']}) — 등록 샘플이 없어 건너뜀"
                      f"(register_face_samples.py를 먼저 실행했는지 확인하세요)")
                continue

            print(f"\n{r['name']}(id={r['id']}) 임베딩 추출 중 ({len(crops)}장)...")
            zs_emb = embed_crops(crops, model, standardize)
            with torch.no_grad():
                adapted = F.normalize(adapter(torch.from_numpy(zs_emb).to(DEVICE)), p=2, dim=1).cpu().numpy()
            centroid = adapted.mean(axis=0)
            centroid = centroid / np.linalg.norm(centroid)
            centroids[rid_key] = torch.from_numpy(centroid).float()
            names[rid_key] = r["name"]
            counts[rid_key] = len(crops)

            d = 1 - adapted @ centroid
            print(f"  centroid까지 거리: mean={d.mean():.4f} median={np.median(d):.4f} max={d.max():.4f}")

    if not centroids:
        raise SystemExit("등록 샘플이 있는 대상자가 한 명도 없어 centroid 파일을 만들지 못했습니다.")

    torch.save({
        "centroids": centroids,
        "names": names,
        "counts": counts,
        "adapter_ckpt": str(ADAPTER_PATH),
        "robot_code": args.robot_code,
    }, OUT_PATH)
    print(f"\n저장 완료: {OUT_PATH} (recipient {len(centroids)}명)")


if __name__ == "__main__":
    main()
