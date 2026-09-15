#!/usr/bin/env python3
"""person_A/person_B의 zero-shot(어댑터 없이 frozen FaceNet 그대로) centroid를 만든다.

build_person_centroids.py와 똑같은 크롭 수집(옷1+옷2, stem collision 회피)을 쓰지만,
어댑터를 거치지 않은 순수 frozen 임베딩을 평균낸다 — 실시간 노드의 이중 확인(실험 C,
2026-09-15)에서 "adapter 판단"과 별개로 "zero-shot 판단"을 하기 위한 두 번째 centroid
세트. face_embedding_adapter_loo_test.py에서 확인된 adapter의 "B-attractor" 왜곡은
adapter(residual 학습)에서 생긴 것이라, 어댑터를 아예 안 거친 zero-shot 판단은 같은
방식으로 왜곡됐을 가능성이 낮다 — 두 판단이 서로 독립적인 오류를 낼 가능성이 높다는
전제로 이중 확인(AND) 게이트를 설계한다.

출력: zero_shot_centroids.pt — {"person_A": Tensor(512,), "person_B": Tensor(512,),
"n_A": int, "n_B": int} 형태. face_identify_test_node.py가 adapter centroid(person_
centroids.pt)와 별도로 로딩해서 쓴다.

사용법:
    <venv>/bin/python3 build_zero_shot_centroids.py
"""
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_embedding_diagnostic import DEVICE, build_embedder, embed_crops  # noqa: E402  (재사용)
from face_embedding_full_rediagnosis import collect_all_groups  # noqa: E402  (재사용)
from face_embedding_adapter_model import ADAPTER_PATH  # noqa: E402  (경로 상수만 재사용)

OUT_PATH = ADAPTER_PATH.parent / "zero_shot_centroids.pt"


def main():
    print("크롭 수집 (옷1+옷2 전체, stem collision 회피 로직 포함, build_person_centroids.py와 동일)...")
    groups = collect_all_groups()
    person_crops = {
        "person_A": groups["A1"] + groups["A2"],
        "person_B": groups["B1"] + groups["B2"],
    }
    print(f"  person_A: {len(person_crops['person_A'])}장  person_B: {len(person_crops['person_B'])}장")

    print("\nFaceNet(frozen, vggface2) 로딩...")
    model, standardize = build_embedder()

    centroids, counts = {}, {}
    for name, crops in person_crops.items():
        print(f"\n{name} zero-shot 임베딩 추출 중 ({len(crops)}장)...")
        emb = embed_crops(crops, model, standardize)  # 이미 L2-normalize된 상태로 반환됨
        centroid = emb.mean(axis=0)
        centroid = centroid / np.linalg.norm(centroid)
        centroids[name] = torch.from_numpy(centroid).float()
        counts[name] = len(crops)

        d = 1 - emb @ centroid
        print(f"  centroid까지 거리(zero-shot): mean={d.mean():.4f} median={np.median(d):.4f} max={d.max():.4f}")

    torch.save({
        "person_A": centroids["person_A"],
        "person_B": centroids["person_B"],
        "n_A": counts["person_A"],
        "n_B": counts["person_B"],
    }, OUT_PATH)
    print(f"\n저장 완료: {OUT_PATH}")


if __name__ == "__main__":
    main()
