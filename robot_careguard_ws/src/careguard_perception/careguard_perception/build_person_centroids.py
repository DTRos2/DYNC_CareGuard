#!/usr/bin/env python3
"""person_A/person_B 등록(enrollment)용 centroid 임베딩을 만든다.

가장 간단한 등록 방식: 각 사람의 크롭 전체(기존 옷1 + 신규 옷2, 다양한 조명/waypoint
포함)를 frozen FaceNet + adapter에 통과시켜 평균(centroid) 벡터 하나로 저장한다.

face_embedding_full_rediagnosis.collect_all_groups()를 그대로 재사용한다 — 이 함수가
이미 person_A/B의 옷1(구, combo_corrected.csv 기준)+옷2(신규, 로봇 재촬영분) 크롭을
파일명 기준(스템 콜리전 회피 포함)으로 정확히 모아준다. 새로 안 짬.

출력: person_centroids.pt — {"person_A": Tensor(512,), "person_B": Tensor(512,),
"n_A": int, "n_B": int, "adapter_ckpt": str} 형태로 저장. ROS2 노드(face_identify_test_node.py)가
그대로 로딩해서 쓴다.

사용법:
    <venv>/bin/python3 build_person_centroids.py
"""
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_embedding_diagnostic import DEVICE, build_embedder, embed_crops  # noqa: E402  (재사용)
from face_embedding_full_rediagnosis import collect_all_groups  # noqa: E402  (재사용 — 옷1+옷2, stem collision 회피 포함)
from face_embedding_adapter_model import ResidualAdapter, ADAPTER_PATH  # noqa: E402

OUT_PATH = ADAPTER_PATH.parent / "person_centroids.pt"


def main():
    print("크롭 수집 (옷1+옷2 전체, stem collision 회피 로직 포함)...")
    groups = collect_all_groups()
    person_crops = {
        "person_A": groups["A1"] + groups["A2"],
        "person_B": groups["B1"] + groups["B2"],
    }
    print(f"  person_A: 옷1 {len(groups['A1'])} + 옷2 {len(groups['A2'])} = {len(person_crops['person_A'])}")
    print(f"  person_B: 옷1 {len(groups['B1'])} + 옷2 {len(groups['B2'])} = {len(person_crops['person_B'])}")

    print("\nFaceNet(frozen) 로딩...")
    model, standardize = build_embedder()

    print(f"adapter 로딩: {ADAPTER_PATH}")
    ckpt = torch.load(ADAPTER_PATH, map_location=DEVICE)
    adapter = ResidualAdapter(dim=ckpt["dim"], bottleneck=ckpt["bottleneck"]).to(DEVICE)
    adapter.load_state_dict(ckpt["state_dict"])
    adapter.eval()

    centroids = {}
    counts = {}
    for name, crops in person_crops.items():
        print(f"\n{name} 임베딩 추출 중 ({len(crops)}장)...")
        zs_emb = embed_crops(crops, model, standardize)
        with torch.no_grad():
            adapted = F.normalize(adapter(torch.from_numpy(zs_emb).to(DEVICE)), p=2, dim=1).cpu().numpy()
        centroid = adapted.mean(axis=0)
        centroid = centroid / np.linalg.norm(centroid)
        centroids[name] = torch.from_numpy(centroid).float()
        counts[name] = len(crops)

        # 참고용: centroid에서 각 샘플까지의 거리 분포 (등록 데이터 자체가 얼마나 흩어져 있는지)
        d = 1 - adapted @ centroid
        print(f"  centroid까지 거리: mean={d.mean():.4f} median={np.median(d):.4f} max={d.max():.4f}")

    torch.save({
        "person_A": centroids["person_A"],
        "person_B": centroids["person_B"],
        "n_A": counts["person_A"],
        "n_B": counts["person_B"],
        "adapter_ckpt": str(ADAPTER_PATH),
    }, OUT_PATH)
    print(f"\n저장 완료: {OUT_PATH}")


if __name__ == "__main__":
    main()
