#!/usr/bin/env python3
"""옷2(person_A 다른 옷, person_B 낮) + unknown_1/2를 포함한 전체 재진단.

face_embedding_diagnostic.py / face_embedding_adapter_finetune.py의 crop 추출·임베딩·
지표 계산 로직을 그대로 재사용한다 (새로 안 짬). 기존(옷1) identity는 이미 여러 차례
검수를 거친 ~/careguard_data/face_dataset(combo_corrected.csv 포함)에서, 신규(옷2 +
unknown)는 이번에 검수를 마친 ~/careguard_data_robot_pull_20260914/face_dataset에서
가져온다 — 후자에는 로봇에 있던 옷1 사본도 섞여 있으므로 combo_corrected.csv의
파일명 집합으로 제외한다 (파일명 기준 매칭, 로봇 사본은 검수를 거치지 않았으므로
identity 그룹에는 절대 안 씀 — bbox 품질이 dev-machine 사본과 다를 수 있음).

비교 그룹:
  1. A-옷1 vs A-옷2 (같은 identity, 옷만 다름 — confound 없는 비교)
  2. B-옷1 vs B-옷2 (같은 identity, 조명+옷 둘 다 다름 — confound 있는 참고용 비교)
  3. A 전체 vs B 전체 (identity 분리)
  4. unknown_1/2 vs A 전체, vs B 전체 (open-set 거부 확인)

zero-shot(frozen FaceNet)과 adapter-tuned(기존에 학습해 둔 어댑터) 둘 다에서 계산한다.
"""
import csv
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_embedding_diagnostic import (  # noqa: E402  (재사용 — 새로 안 짬)
    DEVICE,
    CROP_MARGIN,
    EMBED_SIZE,
    yolo_label_to_pixel_bbox,
    build_embedder,
    embed_crops,
    overlap_coefficient,
    eer_and_best_threshold,
)
from face_embedding_adapter_model import ResidualAdapter, ADAPTER_PATH  # noqa: E402  (순환 import 방지용 공통 모듈)

OLD_DIR = Path.home() / "careguard_data" / "face_dataset"
NEW_DIR = Path.home() / "careguard_data_robot_pull_20260914" / "face_dataset"


def collect_from_dir(base_dir: Path, person_prefix: str, group: str, exclude_filenames=None):
    """face_embedding_diagnostic.collect_crops()와 동일한 크롭 방식(라벨 bbox + margin,
    EMBED_SIZE로 리사이즈)을 person_prefix로 필터링해서 재사용."""
    crops = []
    for split in ("train", "val"):
        img_dir = base_dir / "images" / split
        lbl_dir = base_dir / "labels" / split
        if not img_dir.is_dir():
            continue
        for img_path in sorted(img_dir.glob(f"{person_prefix}_*.jpg")):
            if exclude_filenames and img_path.name in exclude_filenames:
                continue
            lbl_path = lbl_dir / f"{img_path.stem}.txt"
            if not lbl_path.is_file():
                continue
            lines = [ln.strip() for ln in lbl_path.read_text().splitlines() if ln.strip()]
            if len(lines) != 1:
                continue
            with Image.open(img_path) as im:
                im = im.convert("RGB")
                w, h = im.size
                _, (x1, y1, x2, y2) = yolo_label_to_pixel_bbox(lines[0], w, h, CROP_MARGIN)
                crop = im.crop((int(x1), int(y1), int(x2), int(y2))).resize((EMBED_SIZE, EMBED_SIZE))
            crops.append({"group": group, "filename": img_path.name, "image": crop})
    return crops


def collect_all_groups():
    """6개 그룹(A1,A2,B1,B2,U1,U2) crop 딕셔너리를 반환. 다른 스크립트에서도 재사용."""
    old_rows = list(csv.DictReader(open(OLD_DIR / "combo_corrected.csv", newline="", encoding="utf-8")))
    old_A_filenames = {r["filename"] for r in old_rows if r["person"] == "person_A"}
    old_B_filenames = {r["filename"] for r in old_rows if r["person"] == "person_B"}

    return {
        "A1": collect_from_dir(OLD_DIR, "person_A", "A_outfit1"),
        "A2": collect_from_dir(NEW_DIR, "person_A", "A_outfit2", exclude_filenames=old_A_filenames),
        "B1": collect_from_dir(OLD_DIR, "person_B", "B_outfit1"),
        "B2": collect_from_dir(NEW_DIR, "person_B", "B_outfit2", exclude_filenames=old_B_filenames),
        "U1": collect_from_dir(NEW_DIR, "unknown_1", "unknown_1"),
        "U2": collect_from_dir(NEW_DIR, "unknown_2", "unknown_2"),
    }


def build_index(groups: dict):
    """groups(dict of name->crops 리스트)를 하나로 이어붙인 crop 리스트와, 그 안에서
    각 그룹이 차지하는 (start,end) 슬라이스 dict를 함께 반환."""
    order = list(groups.keys())
    idx, cur = {}, 0
    all_crops = []
    for name in order:
        c = groups[name]
        idx[name] = (cur, cur + len(c))
        cur += len(c)
        all_crops.extend(c)
    return all_crops, idx


def stats(arr):
    if len(arr) == 0:
        return "n=0"
    return f"n={len(arr):6d}  mean={arr.mean():.4f}  median={np.median(arr):.4f}  std={arr.std():.4f}"


def compute_group_metrics(emb: np.ndarray, idx: dict, tag: str = None, verbose: bool = True):
    """emb(N,512)와 build_index()가 만든 idx로 [1][2][3][4] 지표를 전부 계산.
    face_embedding_adapter_finetune_v2.py 등 다른 스크립트에서도 그대로 재사용."""
    def sl(name):
        a, b = idx[name]
        return slice(a, b)

    def cross_dist(name_a, name_b):
        A = emb[sl(name_a)]
        B = emb[sl(name_b)]
        An = A / np.linalg.norm(A, axis=1, keepdims=True)
        Bn = B / np.linalg.norm(B, axis=1, keepdims=True)
        return (1 - An @ Bn.T).flatten()

    if verbose and tag:
        print(f"\n{'='*70}\n[{tag}]\n{'='*70}")

    d_A1A2 = cross_dist("A1", "A2")
    d_B1B2 = cross_dist("B1", "B2")
    A_all = np.concatenate([np.arange(*idx["A1"]), np.arange(*idx["A2"])])
    B_all = np.concatenate([np.arange(*idx["B1"]), np.arange(*idx["B2"])])
    An = emb[A_all] / np.linalg.norm(emb[A_all], axis=1, keepdims=True)
    Bn = emb[B_all] / np.linalg.norm(emb[B_all], axis=1, keepdims=True)
    d_AB = (1 - An @ Bn.T).flatten()

    U_all = np.concatenate([np.arange(*idx["U1"]), np.arange(*idx["U2"])])
    Un = emb[U_all] / np.linalg.norm(emb[U_all], axis=1, keepdims=True)
    d_U_A = (1 - Un @ An.T).flatten()
    d_U_B = (1 - Un @ Bn.T).flatten()

    if verbose:
        print("\n[1] A-옷1 vs A-옷2 (같은 사람, 옷만 다름 — confound 없음)")
        print("   ", stats(d_A1A2))
        print("\n[2] B-옷1 vs B-옷2 (같은 사람, 조명+옷 둘 다 다름 — confound 있음, 참고용)")
        print("   ", stats(d_B1B2))
        print("\n[3] A 전체 vs B 전체 (다른 사람)")
        print("   ", stats(d_AB))
        print("\n[4] unknown(1+2) vs A 전체 / vs B 전체")
        print("    vs A:", stats(d_U_A))
        print("    vs B:", stats(d_U_B))

    best_acc, best_th, eer, eer_th = eer_and_best_threshold(d_A1A2, d_AB)
    ovl = overlap_coefficient(d_A1A2, d_AB)
    if verbose:
        eer_s = "N/A" if eer is None else f"{eer*100:.1f}%"
        print(f"\n[정량: [1](A-옷1/옷2, 깨끗한 비교) vs [3](A/B 다른사람)]")
        print(f"    overlap={ovl:.3f}  EER={eer_s}  best_balanced_acc={best_acc*100:.1f}%  threshold={best_th:.3f}")

    best_acc2, best_th2, eer2, eer_th2 = eer_and_best_threshold(d_B1B2, d_AB)
    ovl2 = overlap_coefficient(d_B1B2, d_AB)
    if verbose:
        eer2_s = "N/A" if eer2 is None else f"{eer2*100:.1f}%"
        print(f"\n[정량(참고, confound 있음): [2](B-옷1/옷2) vs [3](A/B 다른사람)]")
        print(f"    overlap={ovl2:.3f}  EER={eer2_s}  best_balanced_acc={best_acc2*100:.1f}%  threshold={best_th2:.3f}")

    same_person_all = np.concatenate([d_A1A2, d_B1B2])
    _, unified_th, unified_eer, _ = eer_and_best_threshold(same_person_all, d_AB)
    fa_A = float(np.mean(d_U_A <= unified_th)) if unified_th is not None else float("nan")
    fa_B = float(np.mean(d_U_B <= unified_th)) if unified_th is not None else float("nan")
    if verbose:
        print(f"\n[open-set 오수용율] 같은사람([1]+[2]) vs 다른사람([3]) 기준 threshold={unified_th:.3f} 사용")
        print(f"    unknown이 A로 잘못 수용될 비율: {fa_A*100:.1f}%")
        print(f"    unknown이 B로 잘못 수용될 비율: {fa_B*100:.1f}%")

    return {
        "d_A1A2": d_A1A2, "d_B1B2": d_B1B2, "d_AB": d_AB, "d_U_A": d_U_A, "d_U_B": d_U_B,
        "ovl1": ovl, "eer1": eer, "acc1": best_acc,
        "ovl2": ovl2, "eer2": eer2, "acc2": best_acc2,
        "fa_A": fa_A, "fa_B": fa_B,
    }


def main():
    print("크롭 추출 중...")
    groups = collect_all_groups()
    for name, c in groups.items():
        print(f"  {name}: {len(c)}장")

    all_crops, idx = build_index(groups)
    print(f"총 {len(all_crops)}장")

    print("\nFaceNet(frozen) 로딩 및 zero-shot 임베딩 추출...")
    model, standardize = build_embedder()
    zs_emb = embed_crops(all_crops, model, standardize)  # (N,512) L2-normalized

    print("adapter 로딩 및 adapter-tuned 임베딩 계산...")
    ckpt = torch.load(ADAPTER_PATH, map_location=DEVICE)
    adapter = ResidualAdapter(dim=ckpt["dim"], bottleneck=ckpt["bottleneck"]).to(DEVICE)
    adapter.load_state_dict(ckpt["state_dict"])
    adapter.eval()
    with torch.no_grad():
        ft_emb = F.normalize(adapter(torch.from_numpy(zs_emb).to(DEVICE)), p=2, dim=1).cpu().numpy()

    zs_result = compute_group_metrics(zs_emb, idx, tag="ZERO-SHOT (frozen FaceNet)")
    ft_result = compute_group_metrics(ft_emb, idx, tag="ADAPTER-TUNED")

    print(f"\n{'='*70}\nzero-shot vs adapter-tuned 요약 비교\n{'='*70}")
    print(f"{'지표':38s} {'zero-shot':>12s} {'adapter-tuned':>14s}")
    def fmt_eer(v): return "N/A" if v is None else f"{v*100:.1f}%"
    print(f"{'[1]A옷1/옷2 vs [3] overlap':38s} {zs_result['ovl1']:12.3f} {ft_result['ovl1']:14.3f}")
    print(f"{'[1]A옷1/옷2 vs [3] EER':38s} {fmt_eer(zs_result['eer1']):>12s} {fmt_eer(ft_result['eer1']):>14s}")
    print(f"{'[2]B옷1/옷2 vs [3] overlap(참고)':38s} {zs_result['ovl2']:12.3f} {ft_result['ovl2']:14.3f}")
    print(f"{'[2]B옷1/옷2 vs [3] EER(참고)':38s} {fmt_eer(zs_result['eer2']):>12s} {fmt_eer(ft_result['eer2']):>14s}")
    print(f"{'open-set unknown->A 오수용율':38s} {zs_result['fa_A']*100:11.1f}% {ft_result['fa_A']*100:13.1f}%")
    print(f"{'open-set unknown->B 오수용율':38s} {zs_result['fa_B']*100:11.1f}% {ft_result['fa_B']*100:13.1f}%")

    print(f"\n{'='*70}\n해석 가이드 (자동 판단 아님 — 숫자 보고 판단할 것)\n{'='*70}")
    print("- [1](A, confound 없음)이 나쁘면(overlap 높음/EER 높음) -> 옷이 확실한 지름길이었을 가능성.")
    print("- [1]은 괜찮은데 [2](B, confound 있음)만 나쁘면 -> 조명 쪽 영향이 더 컸다는 '간접' 증거")
    print("  (확정 아님 — B는 조명+옷이 동시에 바뀐 비교라 원인을 분리할 수 없음).")
    print("- [2]만 보고 '옷 때문에 실패했다'고 결론 내리지 말 것 — B 비교에는 confound가 있음.")
    print("- open-set 오수용율이 0%에 가까울수록 좋음 (미등록자를 A/B로 잘못 받아들이지 않음).")


if __name__ == "__main__":
    main()
