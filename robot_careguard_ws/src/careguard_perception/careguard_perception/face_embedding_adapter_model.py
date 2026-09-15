#!/usr/bin/env python3
"""ResidualAdapter 모델 정의 + 경로 상수. face_embedding_adapter_finetune.py와
face_embedding_full_rediagnosis.py가 서로를 import해야 해서(학습 스크립트가 재진단
로직을, 재진단 스크립트가 어댑터 클래스를 재사용) 생기는 순환 import를 피하려고
공통 정의만 이 파일로 뺐다."""
import os
from pathlib import Path

import torch.nn as nn

BOTTLENECK = 64
ADAPTER_PATH = Path(
    os.getenv("ADAPTER_OUT", "~/careguard_data/runs/face_embedding_adapter/adapter.pt")
).expanduser()


class ResidualAdapter(nn.Module):
    """512차원 frozen 임베딩 위에 얹는 작은 residual bottleneck 어댑터.
    up projection을 0으로 초기화해서 학습 시작 시점 출력은 정확히 입력과 같다
    (=zero-shot과 동일한 상태에서 출발, 안전한 초기값)."""

    def __init__(self, dim=512, bottleneck=BOTTLENECK):
        super().__init__()
        self.down = nn.Linear(dim, bottleneck)
        self.act = nn.ReLU()
        self.up = nn.Linear(bottleneck, dim)
        nn.init.zeros_(self.up.weight)
        nn.init.zeros_(self.up.bias)

    def forward(self, x):
        return x + self.up(self.act(self.down(x)))
