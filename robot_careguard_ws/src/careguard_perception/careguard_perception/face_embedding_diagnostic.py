#!/usr/bin/env python3
"""
person_A/person_B 얼굴 임베딩 분리 가능성 진단 스크립트 (파인튜닝 없음, 순수 진단용).

목적: WeGo LIMO 로봇의 고정된 무릎 아래 카메라 각도 조건에서, frozen pretrained 얼굴
임베딩(FaceNet, facenet-pytorch의 InceptionResnetV1, vggface2 pretrained)만으로
person_A/person_B가 벡터 공간에서 이미 분리되는지 확인한다.

- 얼굴 크롭은 YOLO 검출을 다시 돌리지 않고, 이미 수작업으로 검수/보정된 원본 라벨의
  bbox를 그대로 사용한다 (더 간단하고, 라벨 품질이 이미 검증되어 있음).
- identity 출처는 라벨의 class_id가 아니라 combo_corrected.csv의 person 필드다.
  (실행 중 확인한 사실: 이 dev-machine 사본의 현재 라벨은 09-12 face-detection(nc=1)
  파인튜닝을 위해 전부 class 0으로 리맵되어 있어 identity 정보가 없고, 리맵 전 백업조차
  combo_corrected.csv와 458장 중 60장이 어긋나 신뢰할 수 없었다 — 자세한 내용은
  collect_crops()의 docstring과 실행 시 출력 참고.)
- combo_corrected.csv의 (person, split, filename) 행을 그대로 순회해서 크롭 경로를
  구성한다 — 파일명만으로 매칭하지 않는다 (train/val 간 동일 파일명이 실제로는 다른
  사진인 stem collision 사례가 있었음, 예: person_B_0008.jpg).
- 이 스크립트는 YOLO 검출 모델, ROS 노드(auto_face_capture_node.py 등)를 전혀
  건드리지 않는다. 파인튜닝 코드나 학습 루프도 포함하지 않는다.

사용법:
    <venv>/bin/python3 face_embedding_diagnostic.py
"""
import csv
import os
import itertools
from pathlib import Path
from collections import defaultdict

import numpy as np
import torch
from PIL import Image

FACE_DATASET_DIR = Path(os.getenv(
    "FACE_DATASET_DIR", "~/careguard_data/face_dataset")).expanduser().resolve()
COMBO_CSV = Path(os.getenv("COMBO_CSV", str(FACE_DATASET_DIR / "combo_corrected.csv")))
OUT_DIR = Path(os.getenv("OUT_DIR", "/tmp/face_embedding_diagnostic")).expanduser()
OUT_DIR.mkdir(parents=True, exist_ok=True)

DEVICE = "cuda" if torch.cuda.is_available() else "cpu"
CROP_MARGIN = 0.25   # bbox 각 변으로 이 비율만큼 여유를 더 잘라낸다 (정렬 없이 쓰기 때문에 약간의 여유가 도움)
EMBED_SIZE = 160     # facenet-pytorch InceptionResnetV1 입력 크기

ID_TO_PERSON = {0: "person_A", 1: "person_B"}


# ---------------------------------------------------------------------------
# 1. combo_corrected.csv 로드 (person, split, filename 기준 — 파일명 단독 매칭 금지)
# ---------------------------------------------------------------------------
def load_rows(csv_path: Path):
    with open(csv_path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def yolo_label_to_pixel_bbox(label_line: str, img_w: int, img_h: int, margin: float):
    class_id, cx, cy, w, h = label_line.split()
    class_id = int(class_id)
    cx, cy, w, h = float(cx), float(cy), float(w), float(h)
    w_m, h_m = w * (1 + margin), h * (1 + margin)
    x1 = (cx - w_m / 2) * img_w
    y1 = (cy - h_m / 2) * img_h
    x2 = (cx + w_m / 2) * img_w
    y2 = (cy + h_m / 2) * img_h
    x1, y1 = max(0, x1), max(0, y1)
    x2, y2 = min(img_w, x2), min(img_h, y2)
    return class_id, (x1, y1, x2, y2)


def collect_crops(rows):
    """반환: list of dict(person, combo, split, filename, image=PIL crop)

    identity 출처: combo_corrected.csv의 person 필드.
    라벨 파일(labels/{split}/*.txt)은 bbox 좌표만 가져오는 데 쓴다 — class_id는
    쓰지 않는다. 이유: 이 dev-machine 로컬 사본의 현재 라벨은 09-12 face-detection
    (nc=1) 파인튜닝을 위해 전부 class 0으로 리맵되어 identity 정보가 없고, 리맵 전
    백업(labels_backup_identity_encoded_20260912/)조차 combo_corrected.csv의 person
    필드와 458장 중 60장이 어긋난다 (person_B 25장은 09-12 bbox 보정 도구가 되돌려쓸 때
    class_id를 0으로 고정해버린 것으로 보이고, person_A 35장은 원인 미상 — 인덱스
    220~326 구간에 몰려 있어 그 구간 재촬영/삭제 이력과 관련 있을 가능성이 있음, 이번
    진단 범위 밖이라 원인 규명은 하지 않음). combo_corrected.csv는 여러 차례 수작업
    검증을 거친 값이라 여기서는 이걸 identity 정답으로 신뢰한다."""
    crops = []
    skipped = []
    for r in rows:
        img_path = FACE_DATASET_DIR / "images" / r["split"] / r["filename"]
        stem = Path(r["filename"]).stem
        lbl_path = FACE_DATASET_DIR / "labels" / r["split"] / f"{stem}.txt"
        if not img_path.is_file() or not lbl_path.is_file():
            skipped.append((str(img_path), img_path.is_file(), lbl_path.is_file()))
            continue

        lines = [ln.strip() for ln in lbl_path.read_text().splitlines() if ln.strip()]
        if len(lines) != 1:
            skipped.append((str(lbl_path), f"expected 1 label line, got {len(lines)}"))
            continue

        with Image.open(img_path) as im:
            im = im.convert("RGB")
            img_w, img_h = im.size
            _unused_class_id, (x1, y1, x2, y2) = yolo_label_to_pixel_bbox(lines[0], img_w, img_h, CROP_MARGIN)
            crop = im.crop((int(x1), int(y1), int(x2), int(y2))).resize((EMBED_SIZE, EMBED_SIZE))

        crops.append({
            "person": r["person"],   # combo_corrected.csv 기준 (신뢰 소스, 위 설명 참고)
            "combo": r["combo"],
            "split": r["split"],
            "filename": r["filename"],
            "image": crop,
        })

    return crops, skipped


# ---------------------------------------------------------------------------
# 2. frozen FaceNet(InceptionResnetV1, vggface2)로 임베딩 추출
# ---------------------------------------------------------------------------
def build_embedder():
    from facenet_pytorch import InceptionResnetV1, fixed_image_standardization
    model = InceptionResnetV1(pretrained="vggface2").eval().to(DEVICE)
    for p in model.parameters():
        p.requires_grad_(False)
    return model, fixed_image_standardization


def embed_crops(crops, model, standardize, batch_size=32):
    embeddings = np.zeros((len(crops), 512), dtype=np.float32)
    with torch.no_grad():
        for i in range(0, len(crops), batch_size):
            batch = crops[i:i + batch_size]
            arrs = [np.asarray(c["image"], dtype=np.float32).transpose(2, 0, 1) for c in batch]
            t = torch.from_numpy(np.stack(arrs)).to(DEVICE)
            t = standardize(t)
            out = model(t).cpu().numpy()
            embeddings[i:i + len(batch)] = out
    return embeddings


# ---------------------------------------------------------------------------
# 3. 코사인 거리 분포 계산: intra-A / intra-B (same-combo, diff-combo 구분) / inter-AB
# ---------------------------------------------------------------------------
def cosine_distance_matrix(embeddings: np.ndarray) -> np.ndarray:
    norm = embeddings / np.linalg.norm(embeddings, axis=1, keepdims=True)
    sim = norm @ norm.T
    return 1.0 - sim


def bucket_pairs(crops, dist_matrix):
    n = len(crops)
    buckets = defaultdict(list)
    for i, j in itertools.combinations(range(n), 2):
        a, b = crops[i], crops[j]
        d = dist_matrix[i, j]
        if a["person"] == b["person"]:
            same_combo = (a["combo"] == b["combo"])
            key = (a["person"], "same_combo" if same_combo else "diff_combo")
        else:
            key = ("inter", None)
        buckets[key].append(d)
    return buckets


# ---------------------------------------------------------------------------
# 4. 정량 판단: EER(equal error rate), overlap coefficient, best-threshold accuracy
# ---------------------------------------------------------------------------
def overlap_coefficient(a: np.ndarray, b: np.ndarray, n_bins=60):
    lo = min(a.min(), b.min())
    hi = max(a.max(), b.max())
    bins = np.linspace(lo, hi, n_bins + 1)
    ha, _ = np.histogram(a, bins=bins, density=True)
    hb, _ = np.histogram(b, bins=bins, density=True)
    width = bins[1] - bins[0]
    return float(np.sum(np.minimum(ha, hb)) * width)


def eer_and_best_threshold(same_d: np.ndarray, diff_d: np.ndarray):
    """same_d: 같은 identity 쌍의 거리, diff_d: 다른 identity 쌍의 거리.
    임계값(threshold) 이하이면 '같은 사람'으로 판정한다고 가정."""
    thresholds = np.linspace(0.0, 2.0, 400)
    best_acc, best_th = -1.0, None
    eer, eer_th = None, None
    prev_diff = None
    n_same, n_diff = len(same_d), len(diff_d)
    for th in thresholds:
        tpr = float(np.mean(same_d <= th))        # 같은 사람을 맞게 '같음'으로 판정
        fpr = float(np.mean(diff_d <= th))         # 다른 사람을 틀리게 '같음'으로 판정
        fnr = 1.0 - tpr
        balanced_acc = (tpr + (1 - fpr)) / 2.0
        if balanced_acc > best_acc:
            best_acc, best_th = balanced_acc, th
        diff = fpr - fnr
        if prev_diff is not None and prev_diff <= 0 <= diff or prev_diff is not None and prev_diff >= 0 >= diff:
            eer = float((fpr + fnr) / 2.0)
            eer_th = th
        prev_diff = diff
    return best_acc, best_th, eer, eer_th


def main():
    print(f"FACE_DATASET_DIR = {FACE_DATASET_DIR}")
    print(f"COMBO_CSV        = {COMBO_CSV}")
    print(f"DEVICE           = {DEVICE}\n")

    rows = load_rows(COMBO_CSV)
    print(f"combo_corrected.csv 행 수: {len(rows)}")

    print("⚠️  identity 출처 안내: 이 dev-machine 사본의 현재 라벨(labels/{train,val}/*.txt)은 "
          "09-12 face-detection(nc=1) 파인튜닝을 위해 전부 class 0으로 리맵되어 있어 identity "
          "정보가 없습니다. 리맵 전 백업(labels_backup_identity_encoded_20260912/)도 직접 확인해봤지만 "
          "combo_corrected.csv의 person 필드와 458장 중 60장이 어긋나 신뢰할 수 없었습니다 "
          "(person_B 25장은 09-12 bbox 보정 도구가 되돌려쓸 때 class_id를 0으로 고정해버린 것으로 "
          "보이고, person_A 35장은 원인 미상 — 별도 조사 필요, 이번 진단 범위 밖). 따라서 이 스크립트는 "
          "라벨 파일에서 bbox 좌표만 가져오고, identity는 여러 차례 수작업 검증을 거친 "
          "combo_corrected.csv의 person 필드를 신뢰 소스로 사용합니다.\n")

    crops, skipped = collect_crops(rows)
    print(f"크롭 추출 성공: {len(crops)} / 스킵: {len(skipped)}")
    if skipped:
        print("  스킵된 항목 예시:", skipped[:5])

    per_person = defaultdict(int)
    for c in crops:
        per_person[c["person"]] += 1
    print(f"\n크롭 수: " + ", ".join(f"{p}={n}" for p, n in sorted(per_person.items())))

    print("\nFaceNet(InceptionResnetV1, vggface2 pretrained, frozen) 로딩 중...")
    model, standardize = build_embedder()

    print("임베딩 추출 중...")
    embeddings = embed_crops(crops, model, standardize)
    print(f"임베딩 shape: {embeddings.shape}")

    dist_matrix = cosine_distance_matrix(embeddings)
    buckets = bucket_pairs(crops, dist_matrix)

    intra_A_same_combo = np.array(buckets[("person_A", "same_combo")])
    intra_A_diff_combo = np.array(buckets[("person_A", "diff_combo")])
    intra_B_same_combo = np.array(buckets[("person_B", "same_combo")])
    intra_B_diff_combo = np.array(buckets[("person_B", "diff_combo")])
    inter_AB = np.array(buckets[("inter", None)])

    intra_diff_combo_all = np.concatenate([intra_A_diff_combo, intra_B_diff_combo])
    intra_same_combo_all = np.concatenate([intra_A_same_combo, intra_B_same_combo])

    def stats(name, arr):
        print(f"  {name:28s} n={len(arr):6d}  mean={arr.mean():.4f}  median={np.median(arr):.4f}  "
              f"std={arr.std():.4f}  [{arr.min():.4f}, {arr.max():.4f}]")

    print("\n[코사인 거리 분포 요약] (0=완전히 같은 벡터, 값이 클수록 서로 다름)")
    stats("person_A 내부 (같은 combo)", intra_A_same_combo)
    stats("person_A 내부 (다른 combo)", intra_A_diff_combo)
    stats("person_B 내부 (같은 combo)", intra_B_same_combo)
    stats("person_B 내부 (다른 combo)", intra_B_diff_combo)
    stats("A/B 서로 다른 사람", inter_AB)

    print("\n⚠️  '같은 combo' 쌍은 같은 burst(연속 촬영)라 사실상 거의 동일한 프레임입니다 — "
          "이 값이 매우 작은 것은 임베딩 성능이 아니라 데이터 중복 때문입니다. "
          "실제 '같은 사람이지만 다른 촬영 조건'을 보려면 아래에서는 '다른 combo' 값을 기준으로 판단합니다.")

    # --- 정량 판단은 '다른 combo' 내부 거리 vs 다른 사람 거리로 수행 (더 정직한 비교) ---
    ovl = overlap_coefficient(intra_diff_combo_all, inter_AB)
    best_acc, best_th, eer, eer_th = eer_and_best_threshold(intra_diff_combo_all, inter_AB)

    print(f"\n[정량 지표] (같은 사람·다른 촬영조건 vs 다른 사람, 두 분포 비교)")
    print(f"  분포 겹침 비율 (overlap coefficient, 0=완전분리 1=완전겹침): {ovl:.3f}")
    print(f"  최적 threshold 기준 balanced accuracy: {best_acc*100:.1f}%  (threshold={best_th:.3f})")
    if eer is not None:
        print(f"  Equal Error Rate (EER): {eer*100:.1f}%  (threshold={eer_th:.3f})")
    else:
        print("  EER: 계산 불가 (분포가 threshold 범위 내에서 교차하지 않음)")

    # --- 시각화 ---
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    matplotlib.rcParams["font.family"] = "Noto Sans CJK JP"  # Hangul 글리프 포함 (KR 변형은 matplotlib font_manager가 ttc에서 인식 못함)
    matplotlib.rcParams["axes.unicode_minus"] = False

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))

    ax = axes[0]
    bins = np.linspace(0, max(intra_diff_combo_all.max(), inter_AB.max(), intra_same_combo_all.max()), 60)
    ax.hist(intra_same_combo_all, bins=bins, alpha=0.4, density=True,
            label=f"같은 사람·같은 combo (n={len(intra_same_combo_all)}, 참고용)", color="gray")
    ax.hist(intra_diff_combo_all, bins=bins, alpha=0.6, density=True,
            label=f"같은 사람·다른 combo (n={len(intra_diff_combo_all)})", color="tab:blue")
    ax.hist(inter_AB, bins=bins, alpha=0.6, density=True,
            label=f"다른 사람 (n={len(inter_AB)})", color="tab:red")
    ax.axvline(best_th, color="black", linestyle="--", linewidth=1,
               label=f"최적 threshold={best_th:.3f}")
    ax.set_xlabel("코사인 거리")
    ax.set_ylabel("밀도")
    ax.set_title("얼굴 임베딩 코사인 거리 분포")
    ax.legend(fontsize=8)

    ax2 = axes[1]
    ax2.boxplot(
        [intra_same_combo_all, intra_diff_combo_all, inter_AB],
        tick_labels=["같은사람\n같은combo", "같은사람\n다른combo", "다른사람"],
        showfliers=False,
    )
    ax2.set_ylabel("코사인 거리")
    ax2.set_title("거리 분포 boxplot")

    fig.suptitle(f"person_A/B FaceNet 임베딩 분리 진단 (overlap={ovl:.2f}, EER={eer*100:.1f}%)"
                 if eer is not None else "person_A/B FaceNet 임베딩 분리 진단")
    fig.tight_layout()
    plot_path = OUT_DIR / "face_embedding_diagnostic.png"
    fig.savefig(plot_path, dpi=130)
    print(f"\n플롯 저장: {plot_path}")

    # --- 최종 결론 ---
    print("\n" + "=" * 70)
    print("결론")
    print("=" * 70)
    print(f"판단 기준: overlap coefficient <= 0.30 그리고 EER <= 10% 이면 '분리됨'으로 판단.")
    print(f"실측값: overlap={ovl:.3f}, EER={'N/A' if eer is None else f'{eer*100:.1f}%'}, "
          f"best balanced accuracy={best_acc*100:.1f}%\n")

    separated = (ovl <= 0.30) and (eer is not None and eer <= 0.10)
    if separated:
        print("✅ 분포가 명확히 분리됨 → 이 카메라 각도 조건에서도 frozen pretrained 임베딩만으로 "
              "person_A/B 구분이 가능해 보입니다. 파인튜닝 없이 threshold 캘리브레이션만 진행하는 것을 권장합니다.")
    else:
        print("⚠️  분포가 상당히 겹침 → frozen pretrained 임베딩만으로는 이 각도 조건에서 안정적으로 "
              "구분하기 어려워 보입니다. 파인튜닝이 필요하거나, 무릎 아래 고정 카메라 각도 자체가 "
              "근본적인 한계일 가능성이 있습니다 (하드웨어 변경이 불가능하다는 제약과 함께 검토 필요).")
    print("=" * 70)


if __name__ == "__main__":
    main()
