#!/usr/bin/env python3
"""얼굴 데이터셋 촬영 세션 로그 기록기.

세션(사람 x 시간대 x 장소 x 조명 조합) 끝날 때마다 한 번 실행한다. 노드의
saved_count는 재시작하면 0부터 다시 세서 파일명이 겹칠 수 있으므로(이전 세션
파일을 덮어쓸 위험), saved_count를 믿지 않고 디스크에 실제로 있는
"{person}_*.jpg" 파일 개수를 직접 세서 직전 누적치와 비교해 이번 세션에서
늘어난 장수를 계산한다.

사용 예:
    python3 log_capture_session.py --person person_A --time-of-day morning \
        --location wp1 --light on --window facing
"""
import argparse
import csv
import sys
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from config import FACE_DATASET_DIR  # noqa: E402

LOG_COLUMNS = [
    'date', 'person', 'time_of_day', 'location', 'light', 'window_side',
    'count', 'cumulative_total', 'note',
]


def count_person_images(data_dir: Path, person: str) -> int:
    total = 0
    for split in ('train', 'val'):
        total += len(list((data_dir / 'images' / split).glob(f'{person}_*.jpg')))
    return total


def previous_cumulative_total(log_path: Path, person: str) -> int:
    if not log_path.exists():
        return 0
    with log_path.open(newline='') as f:
        rows = [r for r in csv.DictReader(f) if r['person'] == person]
    if not rows:
        return 0
    return int(rows[-1]['cumulative_total'])


def append_log_row(log_path: Path, row: dict):
    is_new = not log_path.exists()
    with log_path.open('a', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=LOG_COLUMNS)
        if is_new:
            writer.writeheader()
        writer.writerow(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--person', required=True, help='예: person_A')
    parser.add_argument('--time-of-day', required=True, choices=['morning', 'afternoon', 'evening'])
    parser.add_argument('--location', required=True, help='예: white_bg, wp1, wp2, wp3')
    parser.add_argument('--light', required=True, choices=['on', 'off'])
    parser.add_argument('--window', required=True, choices=['facing', 'away'],
                         help='창문 방향 (facing=창문쪽, away=창문반대쪽)')
    parser.add_argument('--note', default='')
    parser.add_argument('--data-dir', type=Path, default=FACE_DATASET_DIR)
    args = parser.parse_args()

    data_dir = args.data_dir.expanduser()
    log_path = data_dir / 'capture_log.csv'

    current_total = count_person_images(data_dir, args.person)
    prev_total = previous_cumulative_total(log_path, args.person)
    session_count = current_total - prev_total

    if session_count <= 0:
        print(f'[경고] {args.person} 새로 늘어난 장수가 {session_count}장입니다 — '
              f'파일명 겹침(덮어쓰기)이나 잘못된 --person 값이 아닌지 확인하세요.',
              file=sys.stderr)

    row = {
        'date': date.today().isoformat(),
        'person': args.person,
        'time_of_day': args.time_of_day,
        'location': args.location,
        'light': args.light,
        'window_side': args.window,
        'count': session_count,
        'cumulative_total': current_total,
        'note': args.note,
    }
    append_log_row(log_path, row)
    print(f'[log_capture_session] {args.person} 이번 세션 {session_count}장 '
          f'(누적 {current_total}장) -> {log_path}')


if __name__ == '__main__':
    main()
