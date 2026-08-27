# Commit & Branch Rules

> 이 문서는 DYNC_CareGuard 팀이 합의한 Git 작업 규칙이다. 별도의 외부 규정 문서(PDF 등)는 없으며, 이 문서 자체가 원본이다. 규칙을 바꾸려면 5절 변경 절차를 따른다.
> 패키지/디렉토리 구조는 [`CONVENTIONS.md`](./CONVENTIONS.md), 리포 최상위 레이아웃은 [`README.md`](./README.md)와 [`docs/FOLDER_STRUCTURE.md`](./docs/FOLDER_STRUCTURE.md)를 본다.

## 1. 브랜치 전략 [확정]

| 브랜치 | 머지 방향 | 용도 |
|---|---|---|
| `main` | ← `develop` | 동작 확인된 소스만. 동결 시점에 태그를 붙인다 |
| `develop` | ← `feature/*` 등 | 통합 브랜치. 평소 작업은 여기로 모인다 |
| `feature/<기능명>` | → `develop` | 기능 개발 (`feature/pillbox-servo`) |
| `fix/<내용>` | → `develop` | 버그 수정 (`fix/nav2-goal-timeout`) |
| `docs/<문서명>` | → `develop` | 문서 작업 (`docs/api-contract`) |
| `chore/<내용>` | → `develop` | 빌드·설정 변경 (`chore/gitignore-update`) |

**절대 규칙: `main`에 직접 커밋하지 않는다.** 항상 `feature/`, `fix/`, `docs/`, `chore/` 중 하나로 브랜치를 만들고, `develop`을 거쳐 PR로만 `main`에 반영한다.

브랜치명 형식: 소문자 + 하이픈 (`feature/pillbox-servo`). 브랜치명 자체에는 한글을 쓰지 않는다.

## 2. 커밋 메시지 [확정]

형식:

```
<type>(<scope>): <subject>
```

- `type`, `scope`는 영문으로 쓴다.
- `scope`는 선택이며 아래 목록 중 하나만 쓴다. 리포 구조(디렉토리/패키지명)와 1:1로 대응한다.
- 시험 기록 커밋은 `scope`에 시험 ID(`T-` + 두 자리, 예: `T-07`)를 넣는다.
- `subject`는 **한글로**, 명령형·서술형으로 간결하게 쓰고 마침표를 붙이지 않는다.

| type | 설명 | 예시 |
|---|---|---|
| `feat` | 새 기능 추가 | `feat(mission): 복약 미션 상태머신 추가` |
| `fix` | 버그 수정 | `fix(nav): 초기 위치 자동발행 타이밍 오류 수정` |
| `docs` | 문서 변경 | `docs: API 계약 문서 추가` |
| `test` | 시험 추가·기록 | `test(T-07): 복약 배달 왕복 정확도 결과 기록` |
| `refactor` | 동작 변화 없는 구조 변경 | `refactor: 도킹 로직을 미션 매니저에서 분리` |
| `chore` | 빌드·설정·의존성 | `chore: 영상 스토리지 git 제외 처리` |

### scope 목록

| scope | 대상 |
|---|---|
| `bringup` | `careguard_bringup` (런치·nav2 설정·지도) |
| `msgs` | `careguard_msgs` (커스텀 msg/srv/action) |
| `nav` | `careguard_nav` (주행·웨이포인트·도킹) |
| `mission` | `careguard_mission` (복약·순찰 시나리오 실행) |
| `perception` | `careguard_perception` (침입자 감지·약 반출 확인) |
| `hmi` | `careguard_hmi` (LCD·스피커·연락망 표시) |
| `health` | `careguard_health` (심박 모니터링) |
| `hw` | `careguard_hw` (약통 서보·아두이노 펌웨어) |
| `agent` | `careguard_agent` (ROS2 ↔ FastAPI 브리지) |
| `server` | `pc_careguard/server` (FastAPI) |
| `dashboard` | `pc_careguard/dashboard` (Qt 대시보드) |
| `db` | `pc_careguard/db`, DB 스키마·마이그레이션 |
| `contract` | `shared/api_contract` (OpenAPI·이벤트 코드) |
| `deploy` | `deploy/` (docker-compose, sync_to_limo.sh) |
| `docs` | `docs/` 문서 작업 (이때는 `type`도 보통 `docs`) |

## 3. PR 및 머지 [확정]

| 구간 | 셀프 머지 | 조건 |
|---|---|---|
| `feature/*` → `develop` | 허용 | PR 생성 필수, 빌드/실행 확인, PR 본문에 작업 내용과 확인 방법 기재 |
| `develop` → `main` | 허용 | PR 생성 필수, 해당 동결 기준 충족, 머지 직후 동결 태그 부착 |

- 리뷰 승인을 머지 조건으로 두지 않는다. 대신 모든 머지가 PR을 거치게 해 변경 이력이 남도록 하고, 실행 확인을 승인 대신 조건으로 삼는다.
- 하나의 PR은 하나의 기능 또는 하나의 수정만 담는다.
- 되돌리기: 머지 후 동작이 깨지면 추가 수정보다 revert를 우선한다. `main`이 항상 실행 가능한 상태를 유지하는 것이 최우선이다.

## 4. 동결과 태그 [제안]

아래 표는 초안이다. 실제 일정(주차/마감일)이 정해지면 확정하고 `[확정]`으로 바꾼다.

| 시점 | 동결 대상 | 태그 |
|---|---|---|
| 1차 | 웨이포인트 이름, ROS2 토픽/메시지 필드, API 계약(`shared/api_contract`) | `v0.1` |
| 2차 | 미션 상태명, DB 스키마 | `v0.2` |
| 3차 | 대시보드 화면 구성 | `v0.3` |
| 최종 | 전체 (제출본) | `v1.0` |

동결 이후 변경 시 5절 변경 절차를 따르고, 영향 받는 노드와 시험 ID를 커밋 메시지에 함께 기록한다.

## 5. 강제(Git Hooks) 적용 방법

이 저장소에는 위 규칙을 강제하는 훅이 `.githooks/`에 들어 있다. 훅은 저장소 기본 `.git/hooks/`가 아니라 버전 관리되는 `.githooks/`에 있으므로, **클론 후 한 번** 아래 명령으로 활성화해야 한다.

```bash
./scripts/setup-git-hooks.sh
# 또는 직접
git config core.hooksPath .githooks
```

활성화하면:

- **`commit-msg`**: 커밋 메시지가 `<type>(<scope>): <subject>` 형식과 위 규칙(type 목록, scope 목록, subject 한글·마침표 금지)을 지키지 않으면 커밋을 거부한다.
- **`pre-commit`**: 현재 브랜치가 `main`이면 커밋을 거부하고, `feature/fix/docs/chore` 브랜치를 만들라는 안내를 출력한다. 브랜치명이 `feature/`, `fix/`, `docs/`, `chore/` 접두어 형식(소문자+하이픈)을 벗어나면 경고한다.

훅 스크립트를 수정해야 하면 `.githooks/commit-msg`, `.githooks/pre-commit`을 고치고, 변경 사유를 6절 변경 절차에 따라 기록한다.

## 6. 규칙 변경 절차

1. **제안**: 무엇을 어떤 이름으로 바꾸는지, 왜 지금 바꿔야 하는지 한 줄로 적는다. 취향 차이는 사유가 되지 않는다.
2. **확인**: 해당 이름/scope를 쓰는 코드, launch 파일, GUI, DB 필드, 훅 스크립트를 모두 찾는다. 하나라도 빠지면 실행 시점에 깨진다.
3. **반영**: 코드와 API 계약(`shared/api_contract`), 이 문서를 같은 PR에서 함께 고친다. 문서를 나중에 고치면 반드시 어긋난다.
4. **기록**: 커밋 메시지에 영향 받는 범위와 관련 시험 ID를 적는다.
