#!/usr/bin/env python3
"""
Leave-one-out(LOO) 진단 실험 — v2 adapter의 "unknown 오수용 0%"가 진짜 일반화
성능인지, 아니면 학습에 직접 쓰인 unknown_1/2 두 명에게만 통하는 in-sample
결과인지 확인한다.

배경: face_embedding_adapter_finetune.py(v2)는 unknown_1/2(32장) 둘 다를
negative로 학습에 포함시켰다. 그래서 v2에 대해 "unknown_2가 학습 때 person_B로
오수용된 비율 0%"라고 말하는 건, unknown_2가 이미 학습 과정에서 "B와 안 닮아야
한다"고 직접 배운 대상이라는 뜻이라 진짜 held-out 테스트가 아니다
(face_embedding_adapter_finetune.py 자신의 "중요한 한계" 주석 참고). 실제로
2026-09-15 실시간 진단에서 v2는 학습에 전혀 안 쓰인 새로운 사람 3명을 전부
person_B로 오인식했다.

이 스크립트가 하는 일 — 새 데이터 수집 없이, 이미 있는 unknown_1/unknown_2로
"학습에 안 쓰인 사람" 상황을 근사 재현한다:
  1. unknown_1만 negative로 포함해 어댑터 재학습 (unknown_2는 학습에서 완전히
     제외 — 순수 held-out). 나머지 설정(person_A/B 데이터, backbone frozen,
     그리드서치 lr/steps)은 face_embedding_adapter_finetune.py(v2)와 동일하게
     유지 — negative 인원수라는 변수만 바뀌게 함.
  2. 이 모델로 unknown_2(학습에 전혀 안 씀)에 대한 오수용율 측정
     (unknown_2->person_A, unknown_2->person_B).
  3. 반대 방향: unknown_2만 negative로 학습 -> unknown_1로 held-out 검증.
  4. v2(프로덕션, 둘 다 negative로 학습된 in-sample 결과)와 나란히 비교표 출력.

⚠️ 스코프: v2(프로덕션용 adapter.pt/person_centroids.pt)는 절대 건드리지
않는다 — 이 스크립트가 학습한 두 모델은 별도 파일(adapter_loo_negative_
{unknown_1,unknown_2}_only.pt)로만 저장되는 진단/실험 전용 산출물이다.
재학습 파라미터(GRID의 lr/steps)는 face_embedding_adapter_finetune.py에서
그대로 import해서 쓴다 — 여기서 새로 짜거나 값을 바꾸지 않는다.

사용법:
    <venv>/bin/python3 face_embedding_adapter_loo_test.py
"""
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_embedding_diagnostic import (  # noqa: E402  (재사용 — 새로 안 짬)
    DEVICE, COMBO_CSV, load_rows, collect_crops, build_embedder, embed_crops,
)
from face_embedding_full_rediagnosis import NEW_DIR, collect_from_dir, collect_all_groups  # noqa: E402
from face_embedding_adapter_model import ResidualAdapter, ADAPTER_PATH, BOTTLENECK  # noqa: E402
from face_embedding_adapter_finetune import (  # noqa: E402  (그리드서치/학습 루프 재사용 — 새로 안 짬)
    GRID, MARGIN, SEED, split_crops, to_label_tensor, train_one_config,
)

OUT_DIR = ADAPTER_PATH.parent  # v2와 같은 디렉터리, 파일명만 다르게 해서 v2 보존
IDENTITY_THRESHOLD = 0.40  # face_identify_test_node.py DEFAULT_IDENTITY_THRESHOLD와 동일

# ── 실험 B: L2 anchor 정규화 강도 그리드. 0.0은 기존 LOO(정규화 없음)와 동일 —
# 베이스라인으로 항상 포함해서 나머지 λ와 나란히 비교한다.
LAMBDA_GRID = [0.0, 0.01, 0.1, 1.0]


def build_centroids(adapter, model, standardize, groups):
    """collect_all_groups()의 A(옷1+옷2)/B(옷1+옷2) 크롭을 주어진 adapter로 임베딩해
    centroid를 만든다. build_person_centroids.py와 동일한 로직 — 그 파일 자체는
    건드리지 않고 이 LOO 실험 전용으로 결과만 재사용."""
    centroids = {}
    for name, crops in (("person_A", groups["A1"] + groups["A2"]),
                         ("person_B", groups["B1"] + groups["B2"])):
        zs = embed_crops(crops, model, standardize)
        with torch.no_grad():
            adapted = F.normalize(adapter(torch.from_numpy(zs).to(DEVICE)), p=2, dim=1).cpu().numpy()
        c = adapted.mean(axis=0)
        c = c / np.linalg.norm(c)
        centroids[name] = torch.from_numpy(c).float()
    return centroids


def evaluate_against_centroids(crops, adapter, model, standardize, centroids):
    """identify()와 동일한 정의(1 - dot(emb, centroid))로 person_A/B까지의 거리와
    identity_threshold 기준 오수용율을 계산."""
    zs = embed_crops(crops, model, standardize)
    with torch.no_grad():
        emb = F.normalize(adapter(torch.from_numpy(zs).to(DEVICE)), p=2, dim=1).cpu().numpy()
    cA = centroids["person_A"].numpy()
    cB = centroids["person_B"].numpy()
    dist_A = 1.0 - emb @ cA
    dist_B = 1.0 - emb @ cB
    return {
        "n": len(crops), "dist_A": dist_A, "dist_B": dist_B,
        "fa_A": float((dist_A <= IDENTITY_THRESHOLD).mean()),
        "fa_B": float((dist_B <= IDENTITY_THRESHOLD).mean()),
    }


def run_grid_search(combined_train_crops, combined_train_emb, val_backbone_emb, val_crops, unknown_backbone_emb,
                     reg_lambda: float = 0.0):
    """v2와 완전히 동일한 GRID(lr/steps)로 학습하고 best config를 고른다 — v2의
    train_one_config/체크포인트 선택 로직을 그대로 재사용. reg_lambda(기본 0.0)를
    넘기면 실험 B(L2 anchor 정규화)가 적용된다 — 0.0이면 기존 LOO 실행과 완전히 동일."""
    train_labels = to_label_tensor(combined_train_crops).to(DEVICE)
    print(f"  학습 데이터: A={int((train_labels == 0).sum())}  B={int((train_labels == 1).sum())}  "
          f"unknown={int((train_labels == -1).sum())}  reg_lambda={reg_lambda}")

    results, trained_adapters = [], {}
    for name, lr, steps in GRID:
        adapter, res = train_one_config(
            name, lr, steps, combined_train_emb, train_labels,
            val_backbone_emb, val_crops, unknown_backbone_emb,
            reg_lambda=reg_lambda,
        )
        results.append(res)
        trained_adapters[name] = adapter

    print(f"\n  그리드 서치 결과 요약:")
    print(f"  {'config':30s} {'lr':>8s} {'steps':>6s} {'best@':>6s} {'val EER':>9s} {'unknown 오수용':>13s} {'score':>7s}")
    for r in results:
        print(f"  {r['name']:30s} {r['lr']:8.0e} {r['steps']:6d} {r['best_step']:6d} "
              f"{r['val_eer']*100:8.1f}% {r['unknown_fa']*100:12.1f}% {r['score']:7.3f}")

    best = min(results, key=lambda r: r["score"])
    print(f"  선택: '{best['name']}' (lr={best['lr']}, steps={best['steps']}, score={best['score']:.3f})")
    return trained_adapters[best["name"]], best


def run_loo_for_lambda(reg_lambda, train_crops, val_crops, train_backbone_emb, val_backbone_emb,
                        u1_crops, u2_crops, u1_backbone_emb, u2_backbone_emb, model, standardize, groups):
    """주어진 reg_lambda로 양방향 LOO(unknown_1만 학습→unknown_2 held-out,
    unknown_2만 학습→unknown_1 held-out)를 모두 돌리고 held-out 평가 결과를 반환.
    각 어댑터는 adapter_v3_regularized_lambda{λ}_negative_{group}_only.pt로 저장
    (v2/adapter.pt는 절대 안 건드림)."""
    experiments = [
        {"train_group": "unknown_1", "held_out_group": "unknown_2",
         "train_crops": u1_crops, "train_emb": u1_backbone_emb, "held_out_crops": u2_crops},
        {"train_group": "unknown_2", "held_out_group": "unknown_1",
         "train_crops": u2_crops, "train_emb": u2_backbone_emb, "held_out_crops": u1_crops},
    ]

    results = {}
    for exp in experiments:
        train_group, held_out_group = exp["train_group"], exp["held_out_group"]
        print(f"\n{'-'*78}\n[λ={reg_lambda}] negative={train_group}만 학습, "
              f"held-out={held_out_group} ({len(exp['held_out_crops'])}장)\n{'-'*78}")

        combined_train_crops = train_crops + exp["train_crops"]
        combined_train_emb = torch.cat([train_backbone_emb, exp["train_emb"]], dim=0)

        chosen_adapter, best_cfg = run_grid_search(
            combined_train_crops, combined_train_emb, val_backbone_emb, val_crops, exp["train_emb"],
            reg_lambda=reg_lambda,
        )

        out_path = OUT_DIR / f"adapter_v3_regularized_lambda{reg_lambda}_negative_{train_group}_only.pt"
        torch.save({
            "state_dict": chosen_adapter.state_dict(),
            "bottleneck": BOTTLENECK,
            "dim": train_backbone_emb.shape[1],
            "margin": MARGIN,
            "reg_lambda": reg_lambda,
            "config": best_cfg,
            "backbone": "facenet_pytorch.InceptionResnetV1(pretrained='vggface2')",
            "note": f"실험 B(L2 anchor 정규화, reg_lambda={reg_lambda}) LOO 진단 전용 — "
                    f"negative는 {train_group}만 학습에 포함, {held_out_group}는 학습에 전혀 "
                    "안 쓰고 held-out 검증용으로만 사용. 프로덕션 v2(adapter.pt)와는 별개 파일 "
                    "— v2는 건드리지 않음.",
        }, out_path)
        print(f"  저장: {out_path}")

        centroids = build_centroids(chosen_adapter, model, standardize, groups)
        held_out_eval = evaluate_against_centroids(exp["held_out_crops"], chosen_adapter, model, standardize, centroids)
        held_out_eval["val_eer"] = best_cfg["val_eer"]  # 이 λ/방향에서의 A/B 분리도 — 재사용
        results[held_out_group] = held_out_eval

        print(f"  [held-out] {held_out_group}: distA(mean={held_out_eval['dist_A'].mean():.4f}) "
              f"distB(mean={held_out_eval['dist_B'].mean():.4f})  "
              f"fa_A={held_out_eval['fa_A']*100:.1f}%  fa_B={held_out_eval['fa_B']*100:.1f}%  "
              f"(이 방향 val_eer={best_cfg['val_eer']*100:.1f}%)")

    return results


def main():
    torch.manual_seed(SEED)
    np.random.seed(SEED)

    print("A/B 크롭 수집 (combo_corrected.csv 기준, v2와 동일)...")
    rows = load_rows(COMBO_CSV)
    crops, skipped = collect_crops(rows)
    train_crops = split_crops(crops, "train")
    val_crops = split_crops(crops, "val")
    print(f"  train={len(train_crops)} val={len(val_crops)} (스킵 {len(skipped)})")

    print("\nunknown_1/unknown_2 크롭 수집...")
    u1_crops = collect_from_dir(NEW_DIR, "unknown_1", "unknown_1")
    u2_crops = collect_from_dir(NEW_DIR, "unknown_2", "unknown_2")
    print(f"  unknown_1={len(u1_crops)}장  unknown_2={len(u2_crops)}장")

    print("\nFaceNet(frozen) 로딩 및 backbone 임베딩 캐싱 (한 번만, 두 LOO 실험이 공유)...")
    model, standardize = build_embedder()
    train_backbone_emb = torch.from_numpy(embed_crops(train_crops, model, standardize)).to(DEVICE)
    val_backbone_emb = torch.from_numpy(embed_crops(val_crops, model, standardize)).to(DEVICE)
    u1_backbone_emb = torch.from_numpy(embed_crops(u1_crops, model, standardize)).to(DEVICE)
    u2_backbone_emb = torch.from_numpy(embed_crops(u2_crops, model, standardize)).to(DEVICE)

    print("\ncentroid 계산용 A/B 전체(옷1+옷2) 크롭 수집...")
    groups = collect_all_groups()

    lambda_summary = []
    loo_results_by_lambda = {}
    for reg_lambda in LAMBDA_GRID:
        print(f"\n{'='*78}\nλ(reg_lambda)={reg_lambda} — 실험 B(L2 anchor 정규화) 전체 LOO\n{'='*78}")
        loo_results = run_loo_for_lambda(
            reg_lambda, train_crops, val_crops, train_backbone_emb, val_backbone_emb,
            u1_crops, u2_crops, u1_backbone_emb, u2_backbone_emb, model, standardize, groups,
        )
        loo_results_by_lambda[reg_lambda] = loo_results
        avg_val_eer = (loo_results["unknown_1"]["val_eer"] + loo_results["unknown_2"]["val_eer"]) / 2
        fa_values = [loo_results["unknown_1"]["fa_A"], loo_results["unknown_1"]["fa_B"],
                     loo_results["unknown_2"]["fa_A"], loo_results["unknown_2"]["fa_B"]]
        lambda_summary.append({
            "reg_lambda": reg_lambda,
            "avg_val_eer": avg_val_eer,
            "avg_fa": sum(fa_values) / len(fa_values),
            "worst_fa": max(fa_values),
            "u1_fa_B": loo_results["unknown_1"]["fa_B"],
            "u2_fa_B": loo_results["unknown_2"]["fa_B"],
        })

    # ── v2(프로덕션, 둘 다 negative로 학습된 in-sample 결과)와 비교 ──
    print(f"\n{'='*78}\nv2(프로덕션) 로딩 — in-sample 비교용, 이 스크립트는 v2를 절대 재학습/덮어쓰지 않음\n{'='*78}")
    v2_ckpt = torch.load(ADAPTER_PATH, map_location=DEVICE)
    v2_adapter = ResidualAdapter(dim=v2_ckpt["dim"], bottleneck=v2_ckpt["bottleneck"]).to(DEVICE)
    v2_adapter.load_state_dict(v2_ckpt["state_dict"])
    v2_adapter.eval()
    v2_centroids_path = ADAPTER_PATH.parent / "person_centroids.pt"
    v2_cent_raw = torch.load(v2_centroids_path, map_location=DEVICE)
    v2_centroids = {"person_A": v2_cent_raw["person_A"].cpu(), "person_B": v2_cent_raw["person_B"].cpu()}

    v2_eval_u1 = evaluate_against_centroids(u1_crops, v2_adapter, model, standardize, v2_centroids)
    v2_eval_u2 = evaluate_against_centroids(u2_crops, v2_adapter, model, standardize, v2_centroids)

    loo_lambda0 = loo_results_by_lambda[0.0]  # 정규화 없음 = 지난 세션 LOO 실험과 동일 조건
    print(f"\n{'='*78}\n참고 비교: v2(in-sample, 둘 다 학습에 씀) vs LOO λ=0.0(정규화 없음, 지난 실험과 동일)\n{'='*78}")
    print(f"{'지표':38s} {'v2 in-sample':>14s} {'LOO(λ=0) held-out':>18s}")
    rows_cmp = [
        ("unknown_1 -> person_A 오수용율", v2_eval_u1["fa_A"], loo_lambda0["unknown_1"]["fa_A"]),
        ("unknown_1 -> person_B 오수용율", v2_eval_u1["fa_B"], loo_lambda0["unknown_1"]["fa_B"]),
        ("unknown_2 -> person_A 오수용율", v2_eval_u2["fa_A"], loo_lambda0["unknown_2"]["fa_A"]),
        ("unknown_2 -> person_B 오수용율", v2_eval_u2["fa_B"], loo_lambda0["unknown_2"]["fa_B"]),
    ]
    for label, v2_val, loo_val in rows_cmp:
        print(f"{label:38s} {v2_val*100:13.1f}% {loo_val*100:17.1f}%")
    v2_avg = sum(v for _, v, _ in rows_cmp) / len(rows_cmp)
    print(f"{'-'*72}")
    print(f"{'평균(4개 조합)':38s} {v2_avg*100:13.1f}% {lambda_summary[0]['avg_fa']*100:17.1f}%")

    # ── 실험 B 핵심 결과: λ(정규화 강도)별 A/B 분리도 vs attractor 완화 트레이드오프 ──
    print(f"\n{'='*78}\n실험 B: λ(L2 anchor 정규화 강도) 그리드 — A/B 분리도 vs attractor 완화 트레이드오프\n{'='*78}")
    print(f"{'λ':>8s} {'avg val EER':>12s} {'평균 오수용율':>13s} {'최악 오수용율':>13s} "
          f"{'u1->B 오수용':>12s} {'u2->B 오수용':>12s}")
    for s in lambda_summary:
        print(f"{s['reg_lambda']:8g} {s['avg_val_eer']*100:11.1f}% {s['avg_fa']*100:12.1f}% "
              f"{s['worst_fa']*100:12.1f}% {s['u1_fa_B']*100:11.1f}% {s['u2_fa_B']*100:11.1f}%")

    baseline_eer = lambda_summary[0]["avg_val_eer"]  # LAMBDA_GRID[0] == 0.0 가정
    # 추천 기준: A/B 분리도(val EER)가 λ=0 대비 5%p 넘게 나빠지지 않는 후보들 중,
    # attractor가 가장 잘 완화된(최악 오수용율이 가장 낮은) λ를 고른다. 전부 5%p를
    # 넘기면(=정규화가 분리도를 심하게 훼손) 어쩔 수 없이 전체 중 최악 오수용율 최소로 폴백.
    candidates = [s for s in lambda_summary if s["avg_val_eer"] <= baseline_eer + 0.05]
    pool = candidates if candidates else lambda_summary
    recommended = min(pool, key=lambda s: s["worst_fa"])
    print(f"\n추천 λ = {recommended['reg_lambda']}  "
          f"(avg val EER={recommended['avg_val_eer']*100:.1f}%, "
          f"최악 오수용율={recommended['worst_fa']*100:.1f}%, "
          f"평균 오수용율={recommended['avg_fa']*100:.1f}%)")
    print("기준: A/B 분리도(val EER)가 λ=0 대비 5%p 이내로 유지되는 λ들 중 attractor가 "
          "가장 잘 완화된(최악 오수용율 최소) 것을 골랐다 — 분리 성능을 포기하면서까지 "
          "attractor만 억누르는 λ는 후보에서 제외.")
    print("\n이 스크립트는 추천만 하고 v2를 자동으로 교체하지 않는다 — 최종 채택 여부는 "
          "이 표를 보고 사용자가 결정.")


if __name__ == "__main__":
    main()
