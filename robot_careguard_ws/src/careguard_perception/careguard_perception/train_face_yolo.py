#!/usr/bin/env python3
"""person_A 파이프라인 검증용 YOLO 학습 스크립트.

person_A 단일 클래스, 56장짜리 데이터로 "학습이 에러 없이 돌아가는지, 데이터
포맷이 맞는지"만 확인하는 용도다. B/C/D 데이터가 모이기 전까지는 실제 다중
인물 식별 성능을 측정할 수 없다 (class_id 0=person_A, 1=person_B, 2=person_C,
3=person_D — auto_face_capture_node.py와 동일한 매핑, 지금은 0만 데이터 존재).

ROS2 노드가 아니라 순수 python 스크립트라 colcon build 없이 바로 실행한다:
    python3 train_face_yolo.py
"""
import argparse
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from config import FACE_DATASET_DIR  # noqa: E402


def build_dataset_yaml(data_dir: Path) -> Path:
    yaml_path = data_dir / 'dataset.yaml'
    yaml_path.write_text(yaml.dump({
        'path': str(data_dir),
        'train': 'images/train',
        'val': 'images/val',
        'names': {0: 'person_A'},
    }))
    return yaml_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-dir', type=Path, default=FACE_DATASET_DIR,
                         help='images/{train,val}, labels/{train,val}을 담은 face_dataset 루트')
    parser.add_argument('--model', default='yolov8n.pt', help='시작 가중치 (기본: nano)')
    parser.add_argument('--epochs', type=int, default=50)
    parser.add_argument('--imgsz', type=int, default=640)
    parser.add_argument('--batch', type=int, default=8)
    parser.add_argument('--project', type=Path, default=Path.home() / 'careguard_data' / 'runs',
                         help='결과 저장 위치 (repo 밖 — *.pt는 재현 가능해서 git에 안 올림)')
    parser.add_argument('--name', default='face_person_A_smoketest')
    args = parser.parse_args()

    data_dir = args.data_dir.expanduser()
    for split in ('train', 'val'):
        img_dir = data_dir / 'images' / split
        if not any(img_dir.glob('*.jpg')):
            raise SystemExit(f'{img_dir} 에 이미지가 없습니다 — data-dir 경로를 확인하세요.')

    dataset_yaml = build_dataset_yaml(data_dir)
    print(f'[train_face_yolo] dataset.yaml -> {dataset_yaml}')

    from ultralytics import YOLO  # 무거운 import라 argparse 실패 시엔 건너뛰도록 뒤로 뺌
    model = YOLO(args.model)
    model.train(
        data=str(dataset_yaml),
        epochs=args.epochs,
        imgsz=args.imgsz,
        batch=args.batch,
        project=str(args.project),
        name=args.name,
    )


if __name__ == '__main__':
    main()
