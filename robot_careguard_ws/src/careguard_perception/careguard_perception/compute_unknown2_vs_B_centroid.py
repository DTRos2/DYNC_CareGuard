#!/usr/bin/env python3
"""
진단 3번 지원 스크립트: unknown_2 -> person_B centroid 코사인 거리를
face_identify_test_node.py의 identify()와 정확히 같은 정의로 재계산한다.

face_embedding_full_rediagnosis.compute_group_metrics()가 계산하는 d_U_B는
"unknown 크롭 vs B 크롭" pairwise 거리(크롭-크롭)인 반면, 실시간 노드의 identify()는
"crop 임베딩 vs person_B centroid(등록 벡터 평균)" 거리를 쓴다 — 정의가 다르므로
이 스크립트는 후자(centroid 기준)로, 실시간과 1:1 비교 가능한 숫자를 만든다.

크롭 방식은 face_embedding_diagnostic.py / face_embedding_full_rediagnosis.py와
완전히 동일(라벨 bbox + 25% margin + PIL 160x160 resize, 재사용만 함 — 새로 안 짬).
adapter.pt(v2)/person_centroids.pt도 로봇에 배포된 것과 동일한 파일(경로: ADAPTER_OUT
env var 또는 기본값 ~/careguard_data/runs/face_embedding_adapter/).

사용법:
    <venv>/bin/python3 compute_unknown2_vs_B_centroid.py
"""
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).resolve().parent))

from face_embedding_diagnostic import DEVICE, build_embedder, embed_crops  # noqa: E402
from face_embedding_full_rediagnosis import NEW_DIR, collect_from_dir  # noqa: E402
from face_embedding_adapter_model import ResidualAdapter, ADAPTER_PATH  # noqa: E402

CENTROIDS_PATH = ADAPTER_PATH.parent / "person_centroids.pt"
IDENTITY_THRESHOLD = 0.40  # face_identify_test_node.py DEFAULT_IDENTITY_THRESHOLD와 동일


def main():
    print(f"ADAPTER_PATH    = {ADAPTER_PATH}")
    print(f"CENTROIDS_PATH  = {CENTROIDS_PATH}")
    print(f"DEVICE          = {DEVICE}\n")

    print("unknown_2 크롭 수집 중 (라벨 bbox + 25% margin + PIL bicubic 160x160, 학습/진단과 동일 파이프라인)...")
    u2_crops = collect_from_dir(NEW_DIR, "unknown_2", "unknown_2")
    print(f"  unknown_2: {len(u2_crops)}장\n")
    if not u2_crops:
        print("⚠️  unknown_2 크롭을 찾지 못했습니다 — NEW_DIR 경로를 확인하세요:", NEW_DIR)
        return

    print("FaceNet(frozen) 로딩 및 zero-shot 임베딩 추출...")
    model, standardize = build_embedder()
    zs_emb = embed_crops(u2_crops, model, standardize)

    print(f"adapter(v2) 로딩: {ADAPTER_PATH}")
    ckpt = torch.load(ADAPTER_PATH, map_location=DEVICE)
    adapter = ResidualAdapter(dim=ckpt["dim"], bottleneck=ckpt["bottleneck"]).to(DEVICE)
    adapter.load_state_dict(ckpt["state_dict"])
    adapter.eval()
    with torch.no_grad():
        emb = F.normalize(adapter(torch.from_numpy(zs_emb).to(DEVICE)), p=2, dim=1).cpu().numpy()

    print(f"centroid 로딩: {CENTROIDS_PATH}")
    cent = torch.load(CENTROIDS_PATH, map_location=DEVICE)
    centroid_A = cent["person_A"].cpu().numpy()
    centroid_B = cent["person_B"].cpu().numpy()

    dist_to_B = 1.0 - emb @ centroid_B
    dist_to_A = 1.0 - emb @ centroid_A

    print(f"\n{'='*72}")
    print("[in-sample] unknown_2 -> person_B centroid 코사인 거리")
    print("(face_identify_test_node.identify()가 실시간에 쓰는 것과 완전히 같은 정의: 1 - dot(emb, centroid))")
    print(f"{'='*72}")
    print(f"  n={len(dist_to_B)}  mean={dist_to_B.mean():.4f}  median={np.median(dist_to_B):.4f}  "
          f"std={dist_to_B.std():.4f}  [{dist_to_B.min():.4f}, {dist_to_B.max():.4f}]")
    fa_rate = float((dist_to_B <= IDENTITY_THRESHOLD).mean())
    print(f"  identity_threshold={IDENTITY_THRESHOLD} 이하 비율(=B로 오수용되는 비율): {fa_rate*100:.1f}%")

    print(f"\n[참고] unknown_2 -> person_A centroid 코사인 거리")
    print(f"  n={len(dist_to_A)}  mean={dist_to_A.mean():.4f}  median={np.median(dist_to_A):.4f}  "
          f"std={dist_to_A.std():.4f}  [{dist_to_A.min():.4f}, {dist_to_A.max():.4f}]")

    print(f"\n[파일별 상세]")
    print(f"  {'filename':30s} {'dist_to_B':>10s} {'dist_to_A':>10s}  {'threshold 이하?'}")
    for c, dB, dA in sorted(zip(u2_crops, dist_to_B, dist_to_A), key=lambda t: t[1]):
        flag = "  <-- B로 오수용" if dB <= IDENTITY_THRESHOLD else ""
        print(f"  {c['filename']:30s} {dB:10.4f} {dA:10.4f}{flag}")


if __name__ == "__main__":
    main()
