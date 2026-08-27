# Naming & Code Conventions

> 이 문서는 DYNC_CareGuard 팀이 합의한 네이밍·구조 규칙이다. 별도의 외부 규정 문서는 없다. Git/커밋 규칙은 [`COMMIT_RULES.md`](./COMMIT_RULES.md)를 본다. 규칙을 바꾸려면 [`COMMIT_RULES.md`](./COMMIT_RULES.md) 6절 절차를 따른다.

## 1. 항목 표기

| 표기 | 의미 |
|---|---|
| `[확정]` | 이미 리포에 반영돼 있거나 팀이 결정한 규칙 |
| `[제안]` | 아직 코드에 반영되지 않은 초안. 해당 부분 구현 착수 전에 합의해 `[확정]`으로 올린다 |

## 2. 공통 표기 원칙 [확정]

| 대상 | 표기 | 예시 |
|---|---|---|
| 패키지·노드·토픽·파라미터 | `snake_case` | `careguard_nav`, `battery_percent` |
| 상태명·이벤트명 | `UPPER_SNAKE_CASE` | `NAVIGATING`, `PILLBOX_OPENED` |
| C++ 클래스·Qt 위젯 | `PascalCase` | `RobotModel`, `P1FleetOverview` |
| DB 테이블·컬럼 | `snake_case` | `recipients`, `mission_id` |
| 브랜치명 | 소문자 + 하이픈 | `feature/pillbox-servo` |

**한글 금지**: 패키지, 노드, 토픽, 파라미터, 파일명, 브랜치명, 커밋 제목(`type`/`scope`)에 한글을 쓰지 않는다. 한글은 주석과 문서에만 쓴다. (단, 커밋 `subject`는 한글 — [`COMMIT_RULES.md`](./COMMIT_RULES.md) 참고)

## 3. 파일 관리 [확정]

### 3.1 커밋 대상과 제외 대상

| 구분 | 대상 | 이유 |
|---|---|---|
| 커밋 | 지도 파일(`.pgm`은 Git LFS, `.yaml`은 일반) | 재현에 필요한 산출물 |
| 커밋 | 웨이포인트, nav2 파라미터, launch·config 파일 | 실행 재현의 근거 |
| 커밋 | 펌웨어 `.ino`(`careguard_hw/firmware/pillbox`), DB 스키마 | 하드웨어·DB 구성의 유일한 기록 |
| 커밋 | API 계약(`shared/api_contract`), 시험 결과 CSV | 인터페이스와 시험 재현의 근거 |
| 제외 | `build/`, `install/`, `log/`, `__pycache__/`, `*.pyc` | 빌드로 재생성되며 충돌을 만든다 |
| 제외 | `dashboard/build/` | Qt 빌드 산출물 |
| 제외 | `server/storage/` (녹화 영상·스냅샷) | 용량이 크고 개인정보를 포함할 수 있다 |
| 제외 | `*.bag`, `*.db3` | rosbag/DB 덤프, 용량이 크고 레포 이력을 무겁게 한다 |
| 제외 | `*.pt` (YOLO 가중치) | 용량이 크고 재현 가능(학습/다운로드)하다 |
| 제외 | `.vscode/`, `.idea/` | 개인 환경 설정 |

(위 표는 리포 루트 `.gitignore`/`.gitattributes`와 동일하게 유지한다. 항목을 바꾸면 두 곳을 같이 고친다.)

### 3.2 비밀정보와 환경 설정

| 항목 | 규칙 |
|---|---|
| DB/서버 접속 정보 | `pc_careguard/server/.env`는 커밋하지 않고 `.env.example`에 키 이름만 남긴다 |
| 장비별 절대 경로 | 소스에 하드코딩하지 않고 launch 파라미터 또는 환경변수로 뺀다 |
| 새 설정 추가 시 | `.example` 파일을 함께 갱신하고 README 실행 절차에 반영한다 |

**경로 하드코딩 금지**: 개발 PC와 LIMO 로봇(rsync 배포 대상)의 계정명·경로가 다를 수 있다. 지도 파일 경로, 캘리브레이션 파일 경로, 시험 기록 출력 경로처럼 계정 경로가 들어가는 값은 반드시 파라미터로 노출한다.

## 4. 네이밍 규칙 [제안]

> 아직 코드가 없으므로 이 절은 전체 `[제안]`이다. 해당 패키지/노드를 실제로 만들 때 이 규칙을 따르고, 필요하면 그 시점에 팀 합의로 고쳐 `[확정]`으로 올린다.

### 4.1 패키지

| 규칙 | 내용 |
|---|---|
| 접두 | 프로젝트 고유 ROS2 패키지는 `careguard_`로 시작한다 |
| 분리 기준 | 기능 영역 단위로 나눈다. 노드 하나마다 패키지를 만들지 않는다 |
| 구성 | `careguard_bringup`, `careguard_msgs`, `careguard_nav`, `careguard_mission`, `careguard_perception`, `careguard_hmi`, `careguard_health`, `careguard_hw`, `careguard_agent` |
| 메시지 패키지 | 커스텀 msg/srv/action은 `careguard_msgs` 하나에 모은다 |

패키지별 상세는 [`docs/FOLDER_STRUCTURE.md`](./docs/FOLDER_STRUCTURE.md) 참고.

### 4.2 노드

| 접미 | 역할 | 예시 |
|---|---|---|
| `_manager` | 상태를 들고 판단하는 노드 | `waypoint_manager`, `mission_scheduler` |
| `_task` | 시나리오 하나를 처음부터 끝까지 실행 | `medication_task`, `patrol_task` |
| `_detector` / `_checker` | 인지·판별 | `intruder_detector`, `pill_taken_checker` |
| `_controller` | 하드웨어 직접 제어 | `pillbox_controller` |
| `_node` | ROS2 밖(FastAPI 등)과 연결하는 브리지 | `uplink_node`, `downlink_node` |
| 접미 없음 | 고유 역할이 이름에 이미 드러나는 경우 | `dock_return`, `vital_monitor`, `face_display` |

### 4.3 토픽·서비스·액션

| 구분 | 형식 | 예시 |
|---|---|---|
| 프로젝트 토픽 | `/careguard/<도메인>/<대상>` | `/careguard/mission/state`, `/careguard/health/heart_rate` |
| 도메인 목록 | `nav · mission · perception · health · hmi · hw` | |
| 서비스 | 동사로 끝낸다 | `/careguard/mission/cancel` |
| 액션 | 동작 명사 | `/careguard/mission/deliver_medication` |
| 표준 토픽 | 바꾸지 않는다 | `/cmd_vel`, `/scan`, `/odom`, `/amcl_pose`, `/tf` |

외부 패키지(Nav2, LIMO 베이스, 카메라 드라이버)가 이미 정한 토픽 이름은 그대로 쓴다. 리매핑으로 `/careguard/` 밑에 억지로 넣지 않는다. 규칙은 새로 만드는 토픽에만 적용한다.

### 4.4 파라미터·프레임

| 대상 | 규칙 | 예시 |
|---|---|---|
| 파라미터 키 | `snake_case`, 단위를 이름에 포함 | `pickup_wait_sec`, `obstacle_stop_distance_m` |
| 임계값 파라미터 | 하드코딩 금지, launch 또는 yaml로 노출 | `battery_reject_percent` |
| TF 프레임 | REP-105 표준 유지 | `map`, `odom`, `base_link`, `base_scan` |
| 추가 프레임 | 부착 위치를 이름에 | `camera_link`, `pillbox_link` |

### 4.5 상태명·식별자

| 대상 | 형식 | 예시 |
|---|---|---|
| 복약 미션 상태 | 대문자, 한글로 번역하지 않음 | `IDLE, VALIDATING, DISPATCHED, NAVIGATING, ARRIVED, PILLBOX_OPENED, WAITING_PICKUP, MED_TAKEN, RETURNING_HOME, DOCKED, COMPLETED, FAILED` |
| 미션 식별자 | UUID | `mission_id` |
| 이벤트 코드 (`MedEvent`, `IntruderAlert`) | `shared/api_contract`에서 정의, 여기서 재정의하지 않는다 | |
| 웨이포인트 식별자 | 이름 기반, 충전소는 `HOME` | `HOME`, `LIVING_ROOM` |
| DB enum | DB에도 대문자 그대로 저장하며 변환하지 않는다 | `status = 'COMPLETED'` |

## 5. 리포 구조 [확정]

리포 최상위는 다음 다섯 개로 고정한다. 새로운 성격의 산출물이 생기면 최상위를 늘리지 않고 해당 폴더 하위에 한 겹을 둔다. 상세는 [`README.md`](./README.md), 패키지 단위 설계는 [`docs/FOLDER_STRUCTURE.md`](./docs/FOLDER_STRUCTURE.md) 참고.

| 경로 | 내용 | 실행 위치 |
|---|---|---|
| `robot_careguard_ws/` | ROS2 워크스페이스 전체 | LIMO 로봇 (`deploy/sync_to_limo.sh`로 rsync 배포) |
| `pc_careguard/` | FastAPI 서버, Qt 대시보드, DB 스키마 | 관제 PC |
| `shared/api_contract/` | OpenAPI 스펙, 이벤트 코드 정의 | 양쪽이 참조만 함 |
| `docs/` | 시나리오서, 와이어프레임, 상세 설계 문서 | - |
| `deploy/` | `docker-compose.yml`, `sync_to_limo.sh` | 관제 PC에서 실행 |

**최상위 신설 금지**: 새 산출물이 생기면 위 다섯 개 중 가장 맞는 폴더 하위에 넣는다. 최상위를 늘리면 이 표가 실제 리포와 어긋난다.

## 6. 기록 규칙 [제안]

### 6.1 시험 ID 체계

- 시험 ID는 `T-` + 두 자리 일련번호로 붙이고 한 번 부여한 번호는 재사용하지 않는다.
- 시험 문서와 측정 원본은 파일명을 시험 ID로 시작한다. 예: `T-07` 시험은 `docs/tests/T-07_medication_delivery.md`와 `docs/tests/data/T-07_log.csv`가 된다.
- 문서·데이터·커밋 scope(`COMMIT_RULES.md` 2절)가 같은 ID로 묶여야 수치의 출처를 되짚을 수 있다.

### 6.2 트러블슈팅 기록

- 이슈는 `docs/troubleshooting/<영역>.md`에 영역별로 나눠 기록한다. 영역은 `NAV · MISSION · PERCEPTION · HMI · HEALTH · HW · SERVER · DASHBOARD · DB · INFRA` 로 고정하며, 최상위 신설 없이 이 하위에서만 영역을 늘리거나 줄인다.
- 이슈 ID는 `TS-<영역>-` + 두 자리 일련번호로 붙이고, 종결되어도 번호를 재사용하지 않는다. 예: `TS-HW-01`.
- 심각도는 `BLOCKER / MAJOR / MINOR`, 상태는 `OPEN / INVESTIGATING / RESOLVED / WORKAROUND / WONTFIX`로만 쓴다.
- 종결하려면 원인란에 "확인됨" 또는 "미확인"을 명시하고, 무엇을 관측해 해결로 판단했는지 적는다. "정상 동작 확인"만으로는 종결하지 않는다.
- `BLOCKER`/`MAJOR`가 미해결로 남아 있으면 시험 인수를 진행하지 않는다. 진행해야 한다면 사유를 시험 기록에 남긴다.

### 6.3 README 규칙

- 위치: 레포 루트에 하나만 둔다. 패키지별 README는 만들지 않는다.
- 필수 내용: 프로젝트 소개, 서비스 시나리오, 리포 구조, 설치·빌드·실행 방법(로봇/서버/대시보드 각각), 배포 방법(`sync_to_limo.sh`), 토픽·서비스·액션 목록, 지도와 웨이포인트 설명, API 계약 위치, 시험 결과, 알려진 문제, 팀원 역할.
- 갱신 시점: 동결 시점마다 갱신한다. 실행 절차가 바뀌는 변경은 같은 PR에서 README도 함께 고친다.
