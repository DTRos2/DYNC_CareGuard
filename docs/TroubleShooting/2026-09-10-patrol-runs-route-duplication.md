# 2026-09-10 트러블슈팅 — `GET /patrol-runs` 라우트 중복 등록으로 "경로순찰 로그" 엔드포인트가 죽은 코드였던 문제

> 작성자: 도희정
> "순찰 취소 파이프라인" 작업 브랜치를 `develop`에 병합하던 중 `patrol.py`를 손으로 병합하다가 발견. 이 브랜치가 P6 `refreshRobotState()`용으로 새로 추가한 `list_patrol_runs`(`GET /patrol-runs`, robot_id/status 필터)와, `develop`에 먼저 들어와 있던 "경로순찰 로그" 조회용 `list_patrol_runs`(`GET /patrol-runs`, start/end/q 필터)가 같은 이름·같은 경로로 겹쳤다. 두 브랜치 각각은 정상이었지만 병합 순간 충돌한, `COMMIT_RULES.md`가 이미 경고해둔 바로 그 유형(2026-09-09 RobotItem 스키마 중복과 같은 계열)의 버그.
> 대상 파일: `pc_careguard/server/app/api/v1/patrol.py`, `pc_careguard/dashboard/src/pages/p7_settings_page.{h,cpp}`

**증상**

실사용 중 관찰된 게 아니라, `patrol.py` 병합 직후 커밋을 시도하자 pre-commit 훅(flake8)이 바로 막았다:
```
pc_careguard/server/app/api/v1/patrol.py:484:1: F811 redefinition of unused 'list_patrol_runs' from line 307
```
만약 이 훅이 없었다면, P7 "경로순찰 로그" 탭은 조용히 엉뚱한 응답(시작/종료 시각 필터도 없고 route_name·recipient_name도 없는 `PatrolRunItem` 목록)을 받아 화면이 깨지거나 빈 채로 남았을 것이다.

**진단 과정**

1. `patrol.py`에서 같은 경로(`GET /patrol-runs`)에 데코레이터가 두 번 등록돼 있었다 — 각각 다른 시그니처의 `list_patrol_runs`.
2. FastAPI(Starlette)는 라우트를 등록 순서대로 매칭해 **먼저 등록된 것**을 우선 채택한다 — 따라서 나중에 등록된 "경로순찰 로그" 핸들러는 영원히 호출될 수 없는 죽은 코드였다.
3. `git blame` 확인 결과, 로그 조회 핸들러는 이미 `develop` 최신 커밋(임계값·알림/경로순찰 로그 조회 API 추가)에 들어와 있던 것이었고, P7 대시보드는 이미 이 죽은 엔드포인트를 호출하고 있었다 — 즉 이 브랜치가 병합하기 전부터, `develop` 단독으로는 문제가 없었지만 두 브랜치가 합쳐지는 순간 발생한 충돌이었다.

**원인**

두 기능(P6의 "로봇이 지금 주행 중인지" 폴링, P7의 "경로순찰 로그" 조회)이 서로 다른 브랜치에서 독립적으로 같은 함수 이름·같은 경로 문자열로 구현됐다. 각 브랜치 단독으로는 `list_patrol_runs`가 한 번만 정의되니 flake8도, 로컬 실행도 아무 문제를 못 잡는다 — 병합해서 한 파일에 둘 다 들어가야만 이름 충돌이 성립한다.

**해결**

로그 조회 쪽을 `list_patrol_run_logs`로 이름을 바꾸고 경로도 `GET /patrol-runs/log`로 분리했다. P7 쪽 호출부(`p7_settings_page.cpp`의 URL, `p7_settings_page.h`의 주석·구조체명)도 새 경로에 맞춰 함께 수정.

**검증**

현재 코드에서 확인:
- `patrol.py`에 `@runs_router.get("")`(333행, `list_patrol_runs`)와 `@runs_router.get("/log")`(510행, `list_patrol_run_logs`)가 서로 다른 경로로 분리돼 등록돼 있음.
- `p7_settings_page.cpp`가 `/patrol-runs/log`를 호출하고 있음.

— **해결 완료**. `flake8 --select=F` 재실행 시 F811 없이 통과함도 확인.

---

**교훈**: 두 브랜치가 "각자는" 문제없이 같은 이름·같은 라우트를 추가하면, 로컬에서 각 브랜치를 따로 테스트해도 절대 못 잡는다. 이번엔 pre-commit 훅(flake8)이 병합 커밋 시점에 잡아줘서 다행이었지만, 같은 클래스의 버그(2026-09-09 RobotItem)는 훅을 우회할 수 있는 F811의 맹점(재정의 전에 한 번이라도 "사용"되면 안 걸림) 때문에 GitHub Actions까지 가서야 잡혔다 — pre-commit 훅과 CI 둘 다 필요한 이유가 이 두 사례로 각각 증명된 셈이다.
