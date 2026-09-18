# DYNC_CareGuard

2차 메인프로젝트 — 노인돌봄서비스. 실내 자율주행 로봇(LIMO/TurtleBot3)이 독거노인 가정을 정기 순찰하며 복약을 관리하고, 이상 상황(낙상·SOS·미등록 침입자 등)을 감지해 원격 관제 대시보드와 보호자에게 즉시 알린다.

## 프로젝트 개요

- **문제의식**: 독거노인 돌봄 공백 — 상시 대면 케어가 어려운 상황에서 복약 관리·안전 확인·응급 대응을 로봇과 원격 관제로 보완한다.
- **구성 요소**: 실내 자율주행 로봇(순찰·복약 전달·안면인식) ↔ FastAPI 중앙 서버(WebSocket 상시 연결, PostgreSQL) ↔ Qt 관제 대시보드(관제사) + 로봇 후면 키오스크(현장 표시) + 이메일 알림(보호자).
- **개발 방식**: 실제 로봇 하드웨어(LIMO, TurtleBot3)에 배포해 실기 검증하며 개발한다. 발견된 문제와 원인·해결 과정은 [`docs/TroubleShooting/`](./docs/TroubleShooting)에 전부 기록한다.

전체 사용 흐름(순찰·복약·SOS·침입자 대응·관제사 운영)은 [`docs/CareGuard_SCENARIO.md`](./docs/CareGuard_SCENARIO.md)에 정리돼 있다.

## 핵심 기능

- **정기 순찰**: 스케줄(요일·시각) 또는 수동 트리거로 로봇이 등록된 웨이포인트를 순회. 순찰 중 카메라로 대상자 안면을 인식해 정지→접근→재판별 후 등록자 확인 또는 침입자(`INTRUDER`) 이벤트 발생.
- **복약 관리**: 복약 스케줄에 맞춰 로봇이 대상자에게 이동 → 음성 안내 → 약통 슬롯 개방(아두이노 서보) → IR 센서로 수령 확인 → 결과를 `medication_log`에 기록. 미수령/연속 미수령은 이벤트로 승격.
- **응급 대응(SOS)**: 로봇 LCD 화면 버튼(`SOS_SCREEN`), 후면 물리 버튼(`SOS_BUTTON`), 음성 도움 요청(`SOS_VOICE`) 3가지 경로로 이벤트 발생 → 관제 대시보드 실시간 알림 → 관제사가 로봇과 스피커 통화 또는 119 모의 신고 시작 → 로봇 후면 키오스크에 상태 실시간 표시.
- **통합 관제 대시보드(Qt, P1~P8)**: 로봇/대상자 통합 현황(P1), 이벤트 큐 처리(P2), 대상자·보호자 관리(P3), 순찰 영상 확인(P4), SLAM 매핑(P5), 순찰 경로·스케줄 편집(P6), 로봇·계정·임계값 설정(P7), 감사 로그(P8).
- **보호자 알림/열람**: 이벤트 발생 시 보호자에게 이메일 발송(SMTP), 보호자 계정으로 로그인하면 자신의 대상자로 범위가 제한된 읽기 전용 화면 제공.
- **원격 텔레옵/맵핑**: SLAM으로 맵을 새로 만들고, 방향키 기반 원격 조종으로 로봇을 움직이며 웨이포인트(순찰 지점/충전 스테이션)를 등록.

기능별 판단 기준(상태 배지 우선순위, 상태별 퀵액션 등)은 [`docs/design/CareGuard_DESIGN_SYSTEM.md`](./docs/design/CareGuard_DESIGN_SYSTEM.md) 참고.

## 시스템 구성

```
┌──────────────────┐        WebSocket (상시 연결)        ┌───────────────────────┐
│  로봇 (LIMO /     │ ───────────────────────────────────▶│  FastAPI 서버           │
│  TurtleBot3)      │◀─────────────────────────────────── │  (pc_careguard/server) │
│  ROS2 + 아두이노   │        REST (명령/조회)              │  + PostgreSQL          │
└──────────────────┘                                      └───────┬───────────────┘
                                                                   │ WebSocket / REST
                                    ┌──────────────────────────────┼──────────────────┐
                                    ▼                              ▼                  ▼
                          Qt 관제 대시보드(P1~P8)        로봇 후면 키오스크        보호자 이메일(SMTP)
                          (관제사, pc_careguard/dashboard) (deploy/robot_kiosk)
```

- 로봇은 서버와의 유일한 통신구인 `careguard_agent`(uplink/downlink 노드)를 통해서만 외부와 통신한다 — 다른 ROS2 노드는 서버를 직접 모르는 구조.
- ESP32 등 저사양 응급 호출 버튼은 MQTT(mosquitto)로 별도 연결된다.

## 기술 스택

| 영역 | 스택 |
|---|---|
| 로봇 소프트웨어 | ROS2 (Python), Nav2, Cartographer(SLAM), OpenCV/YOLO 기반 안면인식, pyserial(아두이노 약통·서보) |
| 서버 | FastAPI, PostgreSQL, WebSocket, SMTP(보호자 이메일), MQTT(mosquitto) |
| 대시보드 | Qt6 C++ (Widgets/SvgWidgets/Multimedia/Network/WebSockets), Opus(음성 통화 코덱) |
| 로봇 후면 키오스크 | 정적 HTML/JS (빌드 없이 브라우저로 실행) |
| 인프라 | Docker Compose(postgres, mosquitto), rsync 기반 로봇 배포, systemd 서비스 |

## 리포지토리 구조

```
DYNC_CareGuard/                    # git repo 루트 (개발은 PC에서 전체를 checkout)
├── robot_careguard_ws/            # ROS2 워크스페이스(LIMO) → deploy/sync_to_limo.sh로 rsync 배포
│   └── src/  careguard_agent · careguard_bringup · careguard_mission · careguard_nav
│              careguard_perception · careguard_hw · careguard_hmi · careguard_msgs
├── careguard_turtlebot_agent/     # TurtleBot3(R-05/R-06) 전용 에이전트 패키지
├── pc_careguard/                  # PC에서 실행 (server + dashboard)
│   ├── server/                    # FastAPI
│   ├── dashboard/                 # Qt C++
│   └── db/                        # schema/(최신 1개 파일) · migrations/(이력)
├── shared/
│   └── api_contract/              # OpenAPI 스펙 · 이벤트 코드 정의 (양쪽 참조)
├── docs/                          # 시나리오서 · 설계 · 트러블슈팅 기록
│   ├── CareGuard_SCENARIO.md      # 전체 사용 시나리오
│   ├── design/                    # 디자인 시스템
│   └── TroubleShooting/           # 실기 개발 중 발견된 문제/원인/해결 기록
├── deploy/                        # docker-compose.yml · sync_to_limo.sh · robot_kiosk/
├── scripts/
│   └── setup-git-hooks.sh         # .githooks/ 활성화 (core.hooksPath 설정)
├── .githooks/                     # commit-msg · pre-commit (COMMIT_RULES.md 강제)
├── COMMIT_RULES.md                # 브랜치 전략 · 커밋 메시지 · PR 규칙
├── CONVENTIONS.md                 # 네이밍 · 리포 구조 · 파일 관리 규칙
└── .gitattributes                 # LFS 규칙 (*.stl, *.pgm)
```

- **개발/배포 방식**: LIMO/TurtleBot3에서 별도로 git checkout하지 않는다. 전체 리포는 PC에서 개발하고, 로봇 쪽 워크스페이스만 rsync로 로봇에 밀어넣어 실행한다. 로봇은 git 클라이언트 없이 rsync 수신 대상 역할만 한다.
- **.githooks/**: 클론 후 `./scripts/setup-git-hooks.sh` 한 번 실행해야 활성화됨. 자세한 내용은 [`COMMIT_RULES.md`](COMMIT_RULES.md) 5절 참고.

각 디렉토리 하위의 상세 패키지/파일 설계는 [`docs/FOLDER_STRUCTURE.md`](docs/FOLDER_STRUCTURE.md) 참고. 브랜치·커밋·PR 규칙은 [`COMMIT_RULES.md`](COMMIT_RULES.md), 네이밍·파일 관리 규칙은 [`CONVENTIONS.md`](CONVENTIONS.md) 참고.

## 개발 환경 실행

```bash
# 1) PostgreSQL + MQTT 브로커
cd deploy && cp .env.example .env   # 필요시 값 수정
docker compose up -d

# 2) FastAPI 서버
cd pc_careguard/server
pip install -r requirements.txt
uvicorn app.main:app --reload

# 3) Qt 대시보드 (Qt6 + libopus-dev 필요)
sudo apt install libopus-dev
cd pc_careguard/dashboard
cmake -B build && cmake --build build
./build/careguard_dashboard

# 4) 로봇 배포 (LIMO)
LIMO_HOST=user@<로봇IP> deploy/sync_to_limo.sh --build

# 5) 로봇 후면 키오스크 (빌드 불필요)
open deploy/robot_kiosk/index.html
```

DB 스키마는 `pc_careguard/db/schema/careguard_schema.sql` 하나가 항상 최신 상태를 유지하며(`docker-entrypoint-initdb.d`가 자동 반영), 이력은 `pc_careguard/db/migrations/`에 번호순으로 남긴다.

## 현재 상태 / 참고

- 실기 검증이 가장 많이 이뤄진 로봇은 **R-07(LIMO)**이며, TurtleBot3 계열(R-05/R-06)은 별도 PC(`careguard_turtlebot_agent`)에서 운용된다.
- 진행 중인 이슈와 재발 방지책은 [`docs/TroubleShooting/`](./docs/TroubleShooting)에서 날짜순으로 확인할 수 있다 — 특히 아직 코드 수정이 적용되지 않은 진단 기록은 파일명에 명시돼 있다.
- 보호자 로그인은 읽기 전용이며, 보호자 계정 자체는 시스템 설정(P7)에서 관리자가 발급한다.
