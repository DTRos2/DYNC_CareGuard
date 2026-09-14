#!/usr/bin/env python3
"""여러 단계(person x waypoint x 조명 x 방향)로 이루어진 촬영 세션을 계획표(csv) 순서대로
안내하며 자동 실행하는 오케스트레이터.

배경: 세션 하나가 이제 person_B 낮 촬영 + person_A 다른 옷 대조군 + 미등록자(unknown)
촬영까지 섞여 20단계가 넘어가면서, 매 단계마다 `ros2 run ... --ros-args -p ...`와
`log_capture_session.py --person ... --time-of-day ... ...`를 손으로 치는 게 너무 번거로워짐.
이 스크립트는 그 두 명령을 계획표 각 행마다 자동으로 실행해준다 — 사람이 할 일은
"로봇 옮기기 / 조명 켜고 끄기 / 촬영 대상자 위치시키기" 같은 물리적 동작뿐이고, 그때마다
Enter로 확인만 해주면 된다.

이 스크립트는 auto_face_capture_node.py와 log_capture_session.py를 그대로 호출만
한다 — 두 파일 자체는 건드리지 않는다.

계획표(csv) 컬럼: step,waypoint,light,person_label,class_id,direction_label,
window_arg(facing/away/ASK),note,max_images,time_of_day
- window_arg가 ASK면 그 단계에서 대화식으로 물어본다 (예: 미등록자 "정면 위주" 촬영처럼
  창문 방향 기준(facing/away)이 애매한 경우 — 미리 값을 정해서 CSV에 채워 넣거나, 그냥
  실행 중에 답해도 된다).

사용법 (로봇 위에서, ROS2 환경 소싱한 상태로):
    python3 run_capture_session.py capture_session_plan_20260915.csv

중간에 Ctrl+C로 세션을 멈춰도 다음에 다시 실행하면 --start-step으로 이어서 할 수 있다:
    python3 run_capture_session.py capture_session_plan_20260915.csv --start-step 9
"""
import argparse
import csv
import subprocess
import sys
from pathlib import Path

LOG_SCRIPT = Path(__file__).resolve().parent / "log_capture_session.py"


def ask_window(step: str) -> str:
    while True:
        v = input(f"  [step {step}] window_side를 정하세요 (facing=창문쪽 / away=반대쪽): ").strip()
        if v in ("facing", "away"):
            return v
        print("  facing 또는 away만 입력하세요.")


def run_step(row: dict) -> bool:
    """이 step을 실행했으면 True, (대상자가 없어) 건너뛰었으면 False를 반환한다."""
    wp, light = row["waypoint"], row["light"]

    print(f"\n--- Step {row['step']}: {wp} / 조명 {light} / {row['person_label']}"
          f"({row['direction_label']}) — 목표 {row['max_images']}장 ---")
    if row["note"]:
        print(f"  note: {row['note']}")
    answer = input("  촬영 대상자가 카메라 앞에 준비되면 Enter "
                    "(대상자가 없어 이 단계를 건너뛰려면 skip 입력) ").strip().lower()
    if answer in ("skip", "s"):
        print(f"  ⏭️  step {row['step']} 건너뜀 (촬영/로그 기록 없음)")
        return False

    window_arg = row["window_arg"]
    if window_arg == "ASK":
        window_arg = ask_window(row["step"])

    subprocess.run([
        "ros2", "run", "careguard_perception", "auto_face_capture_node",
        "--ros-args",
        "-p", f"person_label:={row['person_label']}",
        "-p", f"class_id:={row['class_id']}",
        "-p", f"max_images:={row['max_images']}",
    ], check=True)

    log_cmd = [
        sys.executable, str(LOG_SCRIPT),
        "--person", row["person_label"],
        "--time-of-day", row["time_of_day"],
        "--location", wp,
        "--light", light,
        "--window", window_arg,
    ]
    if row["note"]:
        log_cmd += ["--note", row["note"]]
    subprocess.run(log_cmd, check=True)
    print(f"  ✅ step {row['step']} 완료, capture_log.csv에 기록됨.")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("plan_csv", type=Path)
    parser.add_argument("--start-step", type=int, default=1,
                         help="이 step 번호부터 재개 (중간에 끊겼을 때 처음부터 다시 안 하려고)")
    args = parser.parse_args()

    rows = list(csv.DictReader(open(args.plan_csv, encoding="utf-8")))
    rows = [r for r in rows if int(r["step"]) >= args.start_step]
    if not rows:
        print(f"step {args.start_step} 이후 행이 없습니다.")
        return

    total = len(rows)
    skipped_steps = []
    last_wp, last_light = None, None
    for i, row in enumerate(rows, 1):
        wp, light = row["waypoint"], row["light"]
        if wp != last_wp:
            input(f"\n=== [{i}/{total}, step {row['step']}] 로봇을 {wp}(으)로 이동시키세요. "
                  f"이동 완료 후 Enter ===")
            last_light = None
        if light != last_light:
            action = "켜" if light == "on" else "꺼"
            input(f"[{i}/{total}, step {row['step']}] 조명을 {action}주세요. 완료 후 Enter")
        last_wp, last_light = wp, light

        try:
            done = run_step(row)
            if not done:
                skipped_steps.append(row["step"])
        except subprocess.CalledProcessError as e:
            print(f"\n⚠️  step {row['step']} 실행 중 오류: {e}\n"
                  f"   문제 해결 후 다음 명령으로 이어서 실행하세요:\n"
                  f"   python3 {Path(__file__).name} {args.plan_csv} --start-step {row['step']}")
            sys.exit(1)

    print(f"\n전체 세션 완료 ({total}단계).")
    if skipped_steps:
        print(f"건너뛴 단계: {', '.join(skipped_steps)} — 나중에 대상자 확보되면 그 step 번호로 "
              f"--start-step 돌려서 그 단계만 따로 다시 하면 됩니다 (단, 그 사이 다른 step도 "
              f"다시 실행되지 않도록 계획표를 그 step들만 남긴 별도 csv로 잘라서 쓰는 걸 추천).")


if __name__ == "__main__":
    main()
