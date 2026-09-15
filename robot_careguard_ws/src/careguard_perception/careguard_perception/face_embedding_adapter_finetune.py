#!/usr/bin/env python3
"""
person_A/person_B 얼굴 임베딩 어댑터 재학습 (v2) — unknown을 negative로 추가 + collapse 완화.

배경: v1 어댑터(backbone frozen + 작은 residual adapter, person_A/B 2-identity만으로 학습)는
새로 확보한 held-out 데이터(옷2, unknown_1/2)로 재진단한 결과 [1][2](같은 identity를 다른
옷/조명에서도 인식)는 잘 됐지만, open-set 거부(unknown_1/2가 A/B로 오인되는 비율)가 zero-shot
보다도 나빠졌다 (unknown->B 오수용율 99.7%, face_embedding_full_rediagnosis.py 참고) — A/B
둘만 있는 좁은 세계에 과적합되어 "낯선 사람을 거부"하는 능력이 오히려 망가진 것으로 보인다.

이번 v2에서 하는 두 가지:
  1. unknown_1/2(32장)를 학습에 negative로 포함 — "누구와도 안 닮아야 한다"만 배우게 하고,
     unknown_1과 unknown_2를 서로 같은 identity로 묶어 당기지는 않는다 (labels=-1로 표시,
     아래 pairwise_contrastive_loss의 same/diff 마스크가 unknown-unknown 쌍은 자동으로
     완전히 제외한다 — 당기지도 밀지도 않음).
  2. 학습률/스텝 수 그리드서치로 "약한" 학습 지점을 찾는다 — v1의 EER 0.1%는 backbone은
     안 건드렸어도 작은 어댑터 자체가 과적합(collapse)했다는 신호로 보고, val EER과
     unknown 오수용율을 같이 보면서 트레이드오프가 덜 나쁜 지점을 고른다.

- backbone은 여전히 완전히 동결 — 프리즈 임베딩을 한 번만 계산해 캐싱하고 그 위에서만
  학습 루프가 돈다 (v1과 동일한 구조, 그대로 유지).
- crop 추출은 face_embedding_diagnostic.collect_crops()(기존 A/B, combo_corrected.csv 기준)와
  face_embedding_full_rediagnosis.collect_from_dir()(unknown_1/2, 신규 촬영분 폴더 기준)를
  그대로 재사용한다 — 새로 안 짬.
- 학습 후에는 face_embedding_full_rediagnosis.compute_group_metrics()를 그대로 재사용해서
  [1][2][3][4] 전체를 v1(백업된 이전 어댑터)과 v2(이번 결과) 양쪽에 대해 다시 계산하고 비교표로
  보여준다.
- YOLO 검출 모델, auto_face_capture_node.py는 건드리지 않는다.

사용법:
    <venv>/bin/python3 face_embedding_adapter_finetune.py
"""
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_embedding_diagnostic import (  # noqa: E402  (재사용 — 새로 안 짬)
    DEVICE,
    COMBO_CSV,
    load_rows,
    collect_crops,
    build_embedder,
    embed_crops,
    cosine_distance_matrix,
    bucket_pairs,
    overlap_coefficient,
    eer_and_best_threshold,
)
from face_embedding_full_rediagnosis import (  # noqa: E402  (재사용 — 새로 안 짬)
    NEW_DIR,
    collect_from_dir,
    collect_all_groups,
    build_index,
    compute_group_metrics,
)
from face_embedding_adapter_model import ResidualAdapter, ADAPTER_PATH, BOTTLENECK  # noqa: E402

OUT_DIR = Path("/tmp/face_embedding_diagnostic")
OUT_DIR.mkdir(parents=True, exist_ok=True)
BACKUP_ADAPTER_PATH = ADAPTER_PATH.parent / "adapter_v1_original_backup.pt"  # v1 비교용 (수동 백업해둔 것)

MARGIN = 0.30
UNKNOWN_LABEL = -1
EVAL_EVERY = 20
SEED = 42

# (표시 이름, learning rate, step 수) — v1은 (1e-3, 600). 1/5, 1/10 lr과 조기종료를 같이 본다.
GRID = [
    ("baseline(v1과 동일 lr/steps)", 1e-3, 600),
    ("lr 1/5", 2e-4, 600),
    ("lr 1/5 + 조기종료", 2e-4, 150),
    ("lr 1/10", 1e-4, 300),
]


def pairwise_contrastive_loss(emb: torch.Tensor, labels: torch.Tensor, margin: float,
                               anchor: torch.Tensor = None, reg_lambda: float = 0.0):
    """emb: (N,512) 어댑터 출력(정규화 전). labels: (N,) — 0=person_A, 1=person_B,
    -1=unknown(negative, "누구와도 안 닮아야 함"만 배움).

    same 마스크는 labels가 같고 -1이 아닐 때만 True라서 A-A/B-B만 "같은 사람"으로 당기고,
    unknown-unknown(둘 다 -1)은 절대 여기 안 걸린다. diff 마스크는 labels가 다를 때 True인데
    -1은 0/1과 항상 다르므로 A-unknown/B-unknown 쌍은 자동으로 "밀어내기" 대상이 되고,
    unknown-unknown(둘 다 -1이라 label이 "같음")은 diff에서도 자동으로 빠진다 — 즉
    unknown-unknown 쌍은 당기지도 밀지도 않는 완전 중립으로 처리된다 (요청하신
    "unknown끼리 뭉치게 하지 말 것"을 마스크 설계만으로 만족).

    anchor/reg_lambda (2026-09-15 추가, 실험 B — L2 anchor 정규화): anchor는 보통
    adapter에 넣기 전의 frozen backbone 출력(emb과 같은 shape, 정규화 전) 그 자체.
    ResidualAdapter가 x + f(x) 형태라 reg_lambda*mean(||emb-anchor||^2)는 실질적으로
    잔차 f(x)의 크기를 직접 억제한다 — adapter가 zero-shot 임베딩에서 너무 멀리
    벗어나지 못하게 해서, leave-one-out 진단에서 발견된 "B-attractor"처럼 학습에
    없던 입력을 특정 방향으로 과하게 끌어당기는 왜곡을 완화하는 게 목적.
    reg_lambda=0(기본값)이면 항이 0이 되어 기존 동작과 완전히 동일 — 하위호환."""
    emb_n = F.normalize(emb, p=2, dim=1)
    sim = emb_n @ emb_n.t()
    n = emb_n.shape[0]
    idx_i, idx_j = torch.triu_indices(n, n, offset=1, device=emb_n.device)
    sim_pairs = sim[idx_i, idx_j]
    li, lj = labels[idx_i], labels[idx_j]

    same = ((li == lj) & (li != UNKNOWN_LABEL)).float()
    diff = (li != lj).float()

    pos_loss = (same * (1.0 - sim_pairs).pow(2)).sum() / same.sum().clamp(min=1)
    neg_loss = (diff * torch.clamp(sim_pairs - margin, min=0.0).pow(2)).sum() / diff.sum().clamp(min=1)

    reg_loss = emb.new_zeros(())
    if anchor is not None and reg_lambda > 0:
        reg_loss = reg_lambda * (emb - anchor).pow(2).sum(dim=1).mean()

    total = pos_loss + neg_loss + reg_loss
    return total, pos_loss.item(), neg_loss.item(), reg_loss.item()


def split_crops(crops, split):
    return [c for c in crops if c["split"] == split]


def to_label_tensor(crops):
    def lab(c):
        person = c.get("person", c.get("group"))
        if person == "person_A":
            return 0
        if person == "person_B":
            return 1
        return UNKNOWN_LABEL  # unknown_1, unknown_2 등 그 무엇이든 negative

    return torch.tensor([lab(c) for c in crops], dtype=torch.long)


def val_eer_and_threshold(val_crops, val_emb: np.ndarray):
    dist_matrix = cosine_distance_matrix(val_emb)
    buckets = bucket_pairs(val_crops, dist_matrix)
    intra_diff = np.concatenate([
        np.array(buckets[("person_A", "diff_combo")]) if buckets[("person_A", "diff_combo")] else np.array([]),
        np.array(buckets[("person_B", "diff_combo")]) if buckets[("person_B", "diff_combo")] else np.array([]),
    ])
    inter = np.array(buckets[("inter", None)])
    _, th, eer, _ = eer_and_best_threshold(intra_diff, inter)
    return eer, th


def train_one_config(name, lr, steps, train_emb, train_labels, val_backbone_emb, val_crops, unknown_backbone_emb,
                      reg_lambda: float = 0.0):
    torch.manual_seed(SEED)
    adapter = ResidualAdapter(dim=train_emb.shape[1], bottleneck=BOTTLENECK).to(DEVICE)
    optimizer = torch.optim.Adam(adapter.parameters(), lr=lr)

    best_score, best_eer, best_fa, best_step = None, None, None, 0
    best_state = {k: v.clone() for k, v in adapter.state_dict().items()}

    print(f"\n--- config: {name}  (lr={lr}, steps={steps}, reg_lambda={reg_lambda}) ---")
    for step in range(1, steps + 1):
        adapter.train()
        optimizer.zero_grad()
        adapted = adapter(train_emb)
        loss, pos_l, neg_l, reg_l = pairwise_contrastive_loss(
            adapted, train_labels, MARGIN, anchor=train_emb, reg_lambda=reg_lambda
        )
        loss.backward()
        optimizer.step()

        if step % EVAL_EVERY == 0 or step == steps:
            adapter.eval()
            with torch.no_grad():
                val_adapted = F.normalize(adapter(val_backbone_emb), p=2, dim=1).cpu().numpy()
                unk_adapted = F.normalize(adapter(unknown_backbone_emb), p=2, dim=1).cpu().numpy()
            eer, th = val_eer_and_threshold(val_crops, val_adapted)
            eer_disp = eer if eer is not None else 1.0
            if th is not None:
                d_un_val = (1 - unk_adapted @ val_adapted.T).flatten()
                fa = float(np.mean(d_un_val <= th))
            else:
                fa = 1.0
            score = eer_disp + fa  # 둘 다 낮을수록 좋음 — 분리도 단독 최적화(=collapse 유발) 방지
            print(f"  step {step:4d}  loss={loss.item():.4f} (pos={pos_l:.4f} neg={neg_l:.4f} reg={reg_l:.4f})  "
                  f"val EER={eer_disp*100:5.1f}%  unknown 오수용율={fa*100:5.1f}%  score={score:.3f}")
            if best_score is None or score <= best_score:
                best_score, best_eer, best_fa, best_step = score, eer_disp, fa, step
                best_state = {k: v.clone() for k, v in adapter.state_dict().items()}

    adapter.load_state_dict(best_state)
    return adapter, {"name": name, "lr": lr, "steps": steps, "best_step": best_step,
                      "val_eer": best_eer, "unknown_fa": best_fa, "score": best_score,
                      "reg_lambda": reg_lambda}


def main():
    torch.manual_seed(SEED)
    np.random.seed(SEED)

    rows = load_rows(COMBO_CSV)
    crops, skipped = collect_crops(rows)
    train_crops = split_crops(crops, "train")
    val_crops = split_crops(crops, "val")
    print(f"기존 A/B: train={len(train_crops)} val={len(val_crops)} (스킵 {len(skipped)})")

    unknown_crops = (collect_from_dir(NEW_DIR, "unknown_1", "unknown_1")
                      + collect_from_dir(NEW_DIR, "unknown_2", "unknown_2"))
    print(f"unknown(negative, 학습에 포함): {len(unknown_crops)}장")

    print("\nFaceNet(frozen) 로딩...")
    model, standardize = build_embedder()
    for p in model.parameters():
        p.requires_grad_(False)
    model.eval()

    print("frozen backbone 임베딩 캐싱 중...")
    train_backbone_emb = torch.from_numpy(embed_crops(train_crops, model, standardize)).to(DEVICE)
    val_backbone_emb = torch.from_numpy(embed_crops(val_crops, model, standardize)).to(DEVICE)
    unknown_backbone_emb = torch.from_numpy(embed_crops(unknown_crops, model, standardize)).to(DEVICE)

    combined_train_crops = train_crops + unknown_crops
    combined_train_emb = torch.cat([train_backbone_emb, unknown_backbone_emb], dim=0)
    train_labels = to_label_tensor(combined_train_crops).to(DEVICE)
    print(f"학습 데이터 합계: {len(combined_train_crops)}장  "
          f"(A={int((train_labels==0).sum())}, B={int((train_labels==1).sum())}, "
          f"unknown={int((train_labels==-1).sum())})")

    print(f"\n{'='*78}\n그리드 서치: {len(GRID)}개 설정 (val EER + unknown 오수용율 조합으로 checkpoint 선택)\n{'='*78}")
    results = []
    trained_adapters = {}
    for name, lr, steps in GRID:
        adapter, res = train_one_config(name, lr, steps, combined_train_emb, train_labels,
                                          val_backbone_emb, val_crops, unknown_backbone_emb)
        results.append(res)
        trained_adapters[name] = adapter

    print(f"\n{'='*78}\n그리드 서치 결과 요약 (config별 최적 checkpoint)\n{'='*78}")
    print(f"{'config':30s} {'lr':>8s} {'steps':>6s} {'best@':>6s} {'val EER':>9s} {'unknown 오수용':>13s} {'score':>7s}")
    for r in results:
        print(f"{r['name']:30s} {r['lr']:8.0e} {r['steps']:6d} {r['best_step']:6d} "
              f"{r['val_eer']*100:8.1f}% {r['unknown_fa']*100:12.1f}% {r['score']:7.3f}")

    best = min(results, key=lambda r: r["score"])
    print(f"\n선택된 설정: '{best['name']}' (lr={best['lr']}, steps={best['steps']}, "
          f"score={best['score']:.3f}가 가장 낮음 — val EER과 unknown 오수용율을 더한 값)")
    chosen_adapter = trained_adapters[best["name"]]

    ADAPTER_PATH.parent.mkdir(parents=True, exist_ok=True)
    torch.save({
        "state_dict": chosen_adapter.state_dict(),
        "bottleneck": BOTTLENECK,
        "dim": train_backbone_emb.shape[1],
        "margin": MARGIN,
        "config": best,
        "backbone": "facenet_pytorch.InceptionResnetV1(pretrained='vggface2')",
        "note": "v2: backbone frozen, unknown_1/2(32) included as explicit negatives (never "
                "pulled together), grid-searched lr/steps against v1 to reduce collapse",
    }, ADAPTER_PATH)
    print(f"선택된 어댑터(v2) 저장: {ADAPTER_PATH}")

    print(f"\n{'='*78}\n전체 재진단: v1(이전) vs v2(신규) — [1][2][3][4] 전부 재계산\n{'='*78}")
    groups = collect_all_groups()
    all_crops2, idx2 = build_index(groups)
    zs_emb2 = embed_crops(all_crops2, model, standardize)

    v1_ckpt = torch.load(BACKUP_ADAPTER_PATH, map_location=DEVICE)
    v1_adapter = ResidualAdapter(dim=v1_ckpt["dim"], bottleneck=v1_ckpt["bottleneck"]).to(DEVICE)
    v1_adapter.load_state_dict(v1_ckpt["state_dict"])
    v1_adapter.eval()
    with torch.no_grad():
        v1_emb = F.normalize(v1_adapter(torch.from_numpy(zs_emb2).to(DEVICE)), p=2, dim=1).cpu().numpy()

    chosen_adapter.eval()
    with torch.no_grad():
        v2_emb = F.normalize(chosen_adapter(torch.from_numpy(zs_emb2).to(DEVICE)), p=2, dim=1).cpu().numpy()

    v1_result = compute_group_metrics(v1_emb, idx2, tag="v1 (이전 어댑터, 이번 셋으로 재계산)")
    v2_result = compute_group_metrics(v2_emb, idx2, tag="v2 (unknown negative + 약화된 학습)")

    def fmt_eer(v):
        return "N/A" if v is None else f"{v*100:.1f}%"

    print(f"\n{'='*78}\nv1 vs v2 최종 비교\n{'='*78}")
    print(f"{'지표':34s} {'v1(이전)':>12s} {'v2(신규)':>12s}")
    print(f"{'[1]A옷1/옷2 overlap':34s} {v1_result['ovl1']:12.3f} {v2_result['ovl1']:12.3f}")
    print(f"{'[1]A옷1/옷2 EER':34s} {fmt_eer(v1_result['eer1']):>12s} {fmt_eer(v2_result['eer1']):>12s}")
    print(f"{'[2]B옷1/옷2 overlap(참고,confound있음)':34s} {v1_result['ovl2']:12.3f} {v2_result['ovl2']:12.3f}")
    print(f"{'[2]B옷1/옷2 EER(참고,confound있음)':34s} {fmt_eer(v1_result['eer2']):>12s} {fmt_eer(v2_result['eer2']):>12s}")
    print(f"{'open-set unknown->A 오수용율':34s} {v1_result['fa_A']*100:11.1f}% {v2_result['fa_A']*100:11.1f}%")
    print(f"{'open-set unknown->B 오수용율':34s} {v1_result['fa_B']*100:11.1f}% {v2_result['fa_B']*100:11.1f}%")

    print(f"\n{'='*78}\n중요한 한계 (반드시 감안할 것)\n{'='*78}")
    print("- negative로 쓴 unknown은 unknown_1/2 단 두 명(32장)뿐입니다. 위 open-set 수치가")
    print("  좋게 나와도 '일반적인 낯선 사람 전체'를 거부한다는 보장이 아니라, '이 두 사람과")
    print("  다르게 생긴 방향으로 밀어내는 것'만 배웠을 가능성이 있습니다.")
    print("- 더구나 이 두 명은 학습에 직접 쓰였기 때문에, 위 open-set 재진단은 진짜 held-out")
    print("  테스트가 아니라 '학습이 의도대로 됐는지' 확인하는 in-sample 체크에 더 가깝습니다.")
    print("- 실제 배포 전에는 unknown_1/2가 아닌 완전히 새로운 제3의 인물로 반드시 재검증이")
    print("  필요합니다 — 이번 결과만으로 일반적인 open-set 성능을 보장할 수 없습니다.")


if __name__ == "__main__":
    main()
