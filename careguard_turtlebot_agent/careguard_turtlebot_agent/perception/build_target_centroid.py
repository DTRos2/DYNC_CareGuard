#!/usr/bin/env python3
"""face_capture_node.py로 모은 대상자 얼굴 크롭들로 등록(enrollment) centroid를 만든다.

R-07의 build_person_centroids.py와 달리 adapter(ResidualAdapter)를 쓰지 않는다 —
adapter는 person_A/B "두 사람을 구분"하다 생긴 왜곡(B-attractor)을 고치려고 학습된
모델이라, "등록된 대상자 1명 vs 나머지 전부"만 판단하면 되는 R-06 문제에는 안 맞고
오히려 R-07에만 있는 검증된 adapter.pt 체크포인트에 의존하게 만든다. 그래서 frozen
FaceNet(zero-shot) 임베딩만으로 centroid를 만든다 — 얼굴 검증(verification)에서
흔히 쓰는 가장 단순한 방식(등록 사진들의 임베딩 평균 = 그 사람의 기준 벡터).

ROS2 노드가 아니라 순수 파이썬 스크립트다 — rclpy 불필요, torch/facenet_pytorch만
있으면 R-06에서든 개발 머신에서든 그대로 돌아간다.

출력: target_centroid.pt — {"target": Tensor(512,), "n": int} 형태로 저장. 다음
단계에서 만들 실시간 판정 노드가 이 파일을 로딩해서 새 사진의 임베딩과 코사인 거리를
비교한다.

사용법 (R-06에서, torch/facenet_pytorch 설치 후):
    python3 build_target_centroid.py \\
        --crops-dir ~/careguard_data/target_face_dataset \\
        --out ~/careguard_data/target_centroid.pt
"""
import argparse
import os
from pathlib import Path

import cv2
import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image as PILImage
from facenet_pytorch import InceptionResnetV1, fixed_image_standardization

EMBED_SIZE = 160


def load_crops(crops_dir: str):
    paths = sorted(Path(crops_dir).glob('*.jpg'))
    if not paths:
        raise FileNotFoundError(
            f"{crops_dir}에 .jpg가 없습니다 — 먼저 face_capture_node로 촬영하세요."
        )
    crops = []
    for p in paths:
        img = cv2.imread(str(p))
        if img is None:
            print(f"  [건너뜀] 읽기 실패: {p}")
            continue
        crops.append(img)
    return crops, paths


def embed_crops(crops, model):
    """face_capture_node가 이미 만들어둔 얼굴 크롭(BGR)들을 160x160으로 리사이즈 후
    frozen FaceNet에 통과시켜 L2-normalize된 임베딩을 뽑는다.

    cv2.resize(기본 bilinear)가 아니라 PIL Image.resize(기본 BICUBIC, Pillow>=9.1)를
    쓴다 — R-07 face_identify_test_node.py의 embed_crop()과 같은 이유: 등록(centroid
    생성) 때와 실제 판정 때 리사이즈 알고리즘이 다르면 임베딩이 미세하게 어긋나는
    문제가 R-07 쪽에서 실증됐다. 이후 만들 실시간 판정 노드도 반드시 이 함수와
    동일한 방식(PIL 기본 리사이즈)을 써야 한다.
    """
    tensors = []
    for img_bgr in crops:
        img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
        resized = np.asarray(PILImage.fromarray(img_rgb).resize((EMBED_SIZE, EMBED_SIZE)))
        t = torch.from_numpy(resized).permute(2, 0, 1).float()
        tensors.append(fixed_image_standardization(t))
    batch = torch.stack(tensors)
    with torch.no_grad():
        emb = model(batch)  # InceptionResnetV1 출력은 이미 L2-normalize된 상태
    return emb


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--crops-dir', default=os.path.expanduser('~/careguard_data/target_face_dataset'))
    parser.add_argument('--out', default=os.path.expanduser('~/careguard_data/target_centroid.pt'))
    args = parser.parse_args()

    print(f"크롭 로딩: {args.crops_dir}")
    crops, paths = load_crops(args.crops_dir)
    print(f"  {len(crops)}장 로드됨")

    print("FaceNet(frozen, vggface2) 로딩...")
    model = InceptionResnetV1(pretrained='vggface2').eval()

    print("임베딩 추출 중...")
    emb = embed_crops(crops, model)

    centroid = F.normalize(emb.mean(dim=0, keepdim=True), p=2, dim=1).squeeze(0)

    # 참고용: centroid에서 각 샘플까지의 거리 분포 — 등록 사진 자체가 얼마나
    # 일관된지 확인(값이 크게 튀는 사진이 있으면 그 파일을 골라내는 데 씀).
    dists = 1 - (emb @ centroid)
    print(f"centroid까지 거리: mean={dists.mean():.4f} median={dists.median():.4f} max={dists.max():.4f}")
    worst_idx = int(dists.argmax())
    print(f"  가장 먼 사진: {paths[worst_idx].name} (dist={dists[worst_idx]:.4f}) — "
          f"확 튀면 오염된 사진(다른 사람/블러)일 수 있으니 확인 권장")

    torch.save({'target': centroid, 'n': len(crops)}, args.out)
    print(f"\n저장 완료: {args.out}")


if __name__ == '__main__':
    main()
