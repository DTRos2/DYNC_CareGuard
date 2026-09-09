# 2026-09-09 트러블슈팅 — RobotItem 스키마 중복으로 인한 `GET /robots` 크래시

> `develop` 브랜치, P5(맵·로봇 매핑)/P6(순찰·경로) 페이지 코드 리뷰 중 `feature/p5-p6-map-patrol-pages`(PR #36) 병합 직후 발견한 문제의 증상·원인·해결과, 재발 방지를 위해 추가한 flake8 pre-commit/CI 훅, 그 훅을 테스트하다 발견한 한계, 그리고 "이런 중복이 또 있나" 전수 조사하다 찾은 미해결 항목 하나를 정리한 기록.
> 대상 파일: `pc_careguard/server/app/schemas.py`, `pc_careguard/server/app/robots.py`, `pc_careguard/server/app/api/v1/robots.py`(삭제), `pc_careguard/server/app/main.py`, `pc_careguard/server/app/api/v1/media.py`, `pc_careguard/server/app/api/v1/recipient.py`, `.githooks/pre-commit`, `.github/workflows/server-lint.yml`, `COMMIT_RULES.md`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | `GET /robots` 호출 시 500 에러 — P5 로봇 선택 드롭다운과 P7 로봇 관리 탭이 둘 다 깨짐 | `schemas.py`에 `class RobotItem`이 두 번 정의됨(서로 다른 브랜치가 각자 추가) + `main.py`가 같은 경로(`/robots`)에 라우터를 두 번 등록 | postwo의 기존 전체 스키마만 남기고 중복 클래스·중복 라우터 제거 |
| 2 | 이런 종류의 실수를 막을 자동 검사가 전혀 없음 | repo에 lint/CI 설정이 아예 없었음 | `.githooks/pre-commit`에 flake8(`--select=F`) 추가 + GitHub Actions `server-lint` 워크플로 신설 |
| 3 | 추가한 flake8 검사가 재현 테스트 중 한 번 안 걸림 | pyflakes의 `F811`(이름 재정의)은 "**미사용** 상태에서 재정의"만 잡는데, 재정의 전에 그 이름이 한 번이라도 참조(상속 등)되면 검사를 건너뜀 | 완전한 방어는 아님을 확인 — 문서화하고 안전망으로만 취급 |
| 4 (미해결) | 심각도(`info`/`attention`/`emergency`) 분류 규칙이 두 파일에 독립적으로 하드코딩돼 있어 드리프트 위험 | `media.py::SEVERITY_TAGS`(dict)와 `recipient.py`의 SQL `IN ('attention','emergency')`가 같은 규칙을 각자 따로 표현 | 아직 안 고침 — 이 문서에 기록만 남김 (요청에 따라 보류) |

---

## 1. `GET /robots` 500 에러 — P5 드롭다운과 P7 로봇 관리 탭이 동시에 깨짐

**증상**

`develop`을 pull 받아 P5(맵)/P6(순찰) 페이지를 다시 리뷰하던 중, 병합된 코드를 실제로 띄워 `GET /robots`를 호출하면 500 Internal Server Error가 남. 코드만 읽었을 때는 안 보였고, 서버를 직접 띄워 엔드포인트를 호출해봐야 재현됐다.

```
pydantic_core._pydantic_core.ValidationError: 1 validation error for RobotItem
is_online
  Field required [type=missing, input_value={'id': 1, 'code': 'R-01',...}, input_type=dict]
```

**진단 과정**

1. 스택 트레이스가 `app/api/v1/robots.py:30, list_robots()`를 가리킴 — 이 파일은 `id, code, name, model, ip, created_at` 6개 필드만 SELECT하는데, `RobotItem(**row)`를 만들다가 `is_online` 필드가 없다고 실패.
2. `schemas.py`에서 `RobotItem` 정의를 찾아보니 **두 곳**에 있었음:
   ```
   125: class RobotItem(BaseModel):   # id/code/name/model/ip/created_at
   342: class RobotItem(BaseModel):   # id/code/name/model/ros_domain_id/ip/map_recipient/is_online
   ```
   파이썬은 같은 이름을 나중 정의로 조용히 덮어쓰므로, `api/v1/robots.py`가 import하는 `RobotItem`은 실제로는 342행(더 많은 필드를 요구하는) 쪽이었음 — 그래서 6개 필드만 채운 코드가 크래시.
3. `git blame`으로 작성자 추적:
   - 125행(최소 버전): `yuna`, `6fdaec18`(2026-09-09) — "P5 매핑 페이지 로봇 선택 드롭다운 구현" 커밋에서 `api/v1/robots.py`와 함께 신설.
   - 342행(전체 버전): `postwo`, `cd3b612b`(2026-09-07) + `1e792870`(2026-09-08) — P7 로봇 관리 기능(`app/robots.py`)용으로 먼저 존재.
   - 서로 다른 브랜치에서 독립적으로 작업하다 같은 이름을 재사용한 것이 develop 병합 시점에 충돌로 드러남. 각 브랜치의 커밋만 보면 개별적으로는 문제가 없었다.
4. `main.py`도 같은 경로에 라우터가 두 번 등록돼 있었음:
   ```python
   app.include_router(robots_api.router)  # api/v1/robots.py — 먼저 등록
   ...
   app.include_router(robots_router)      # app/robots.py — 나중 등록
   ```
   FastAPI는 "Duplicate Operation ID" 경고를 띄우지만 막지는 않는다. OpenAPI 문서에는 **나중에 등록된** 라우터의 스키마가 보이지만, 실제 HTTP 요청은 Starlette가 등록 순서대로 매칭해서 **먼저 등록된**(=깨진) 쪽으로만 갔다 — 문서와 실제 동작이 다르게 보이는 함정이었다.
5. 영향 범위 확인: `P5MapPage::loadRobotsFromServer()`(신규 드롭다운)뿐 아니라, 기존에 잘 동작하던 `P7SettingsPage::fetchRobots()`(`is_online` 필드를 기대)도 항상 깨진 핸들러로만 라우팅되어 같이 죽어 있었음.

**원인**

서로 다른 브랜치가 각자 필요에 맞는 `RobotItem`을 독립적으로 추가했고(한쪽은 맵 없는 로봇까지 포함하는 최소 스키마, 다른 쪽은 `is_online` 등을 포함한 관리용 전체 스키마), 병합 시점에 이름 충돌·경로 충돌이 겹쳐 정적으로는 전혀 안 보이고 해당 엔드포인트를 실제로 호출해야만 드러나는 런타임 버그가 됐다.

**해결**

1. `schemas.py`: yuna의 최소 `RobotItem`(125행) 삭제, postwo의 전체 버전만 유지.
2. `pc_careguard/server/app/api/v1/robots.py` 파일 자체를 삭제 — 확인해보니 `app/robots.py::LIST_QUERY`가 이미 `WHERE r.deregistered_at IS NULL`만으로(맵 유무와 무관하게) 전체 로봇을 조회하고 있어서, yuna가 원했던 "맵 없는 로봇도 나와야 한다"는 요구사항을 이미 만족하고 있었다 — 완전히 중복이었다.
3. `main.py`에서 삭제된 라우터의 import/`include_router` 제거.
4. `app/robots.py::LIST_QUERY`에 "map을 JOIN/필터 조건으로 쓰면 안 된다"는 이유를 주석으로 남김 — 나중에 누가 이 쿼리를 "정리"하다 맵 없는 로봇을 다시 빠뜨리는 회귀를 예방하기 위함.
5. 검증: 실제 DB로 서버를 띄워 `GET /robots` 호출 → 맵 없는 로봇(R-05) 포함 5개 모두 정상 반환, `is_online`/`map_recipient` 포함 확인. `--reload`로 떠 있던 실제 개발 서버도 자동 반영되어 200 확인. 대시보드 C++ 쪽(`ApiClient::RobotInfo`/`parseRobot`)은 `id/code/name/model/ip`만 필드명으로 읽어서 그대로 호환 — 수정 불필요, 빌드도 정상.

**교훈**: 파이썬은 같은 파일 안에서 클래스/함수 이름을 다시 정의해도 문법 에러가 나지 않는다 — `py_compile`도, 파이썬 인터프리터 자체도 아무 경고 없이 조용히 덮어쓴다. FastAPI도 같은 경로에 라우터를 두 번 등록하는 걸 막지 않는다("Duplicate Operation ID" 경고만 낸다). 두 문제 모두 **서버를 실제로 띄워 문제의 엔드포인트를 호출해봐야만** 드러난다 — 코드 리뷰(정적으로 읽기)만으로는 놓치기 쉽다. 특히 이번처럼 "각자 브랜치에서는 멀쩡했는데 병합해서 문제가 생기는" 종류는 개별 PR의 diff만 봐서는 절대 안 보이고, 병합된 상태를 실제로 실행해봐야 한다.

---

## 2. 재발 방지 — flake8 pre-commit 훅 + GitHub Actions 추가

**배경**

repo에 `.pre-commit-config.yaml`, `pyproject.toml`, `.flake8`, `.github/workflows/` 등 정적 검사나 CI가 전혀 없었다(확인 완료). `.githooks/`에 이미 버전 관리되는 훅(`commit-msg`, `pre-commit` — `COMMIT_RULES.md` §5)이 있어 그 컨벤션을 따라 확장하기로 함.

**해결**

1. `pc_careguard/server/requirements-dev.txt`에 `flake8` 추가, 서버 venv에 설치.
2. `.githooks/pre-commit`에 검사 추가: `pc_careguard/server/*.py`가 스테이징돼 있으면 `flake8 --select=F`(pyflakes: 미정의 이름·미사용 import·이름 재정의 등)를 돌려 걸리면 커밋을 거부. flake8이 로컬에(또는 서버 venv에) 없으면 경고만 하고 통과시키는 best-effort 방식 — 커밋 자체가 flake8 미설치로 막히면 안 되기 때문.
3. `.github/workflows/server-lint.yml` 신설: `push`/`pull_request`(`develop`, `main` 대상) 시 flake8을 무조건 실행. `pull_request` 트리거는 기본적으로 PR 브랜치와 대상 브랜치를 **합친** 커밋을 체크아웃하므로, 이번 사고처럼 "각 브랜치는 멀쩡한데 합치면 충돌"하는 경우를 **병합 전에** 잡아낼 수 있다.
4. `COMMIT_RULES.md` §5에 새 검사 내용과 아래 3번 항목의 한계를 기록(§5·§6 절차상 요구되는 변경 기록).

**검증**

원래 버그와 동일하게(사용되기 전에 재정의) `RobotItem`을 다시 중복시켜 스테이징하고 훅을 직접 실행:
```
schemas.py:336:1: F811 redefinition of unused 'RobotItem' from line 331
pre-commit 훅: flake8(F계열: 미정의 이름·미사용 import·이름 재정의 등)에 걸렸습니다.
```
exit code 1로 커밋 차단 확인. 정상 상태로 되돌리면 exit 0으로 통과하는 것도 확인.

---

## 3. flake8(F811)의 맹점 — "미사용 상태에서 재정의"만 잡는다

**증상**

위 검증에 앞서, 처음엔 파일 **맨 끝**에 중복 `RobotItem`을 추가해 테스트했는데 flake8이 아무것도 잡아내지 못했다(exit 0).

**진단 과정**

1. 최소 재현(`class Foo: ...` 두 번, 아무것도 그 사이에 없음)으로는 정상적으로 `F811`이 남 — flake8 자체가 고장 난 건 아님을 확인.
2. 실제 `schemas.py`에서 `RobotItem`을 grep해보니 두 정의 **사이**에 `class RegisterRobotResponse(RobotItem):`(353행, 상속)이 있었음 — 처음 내가 넣은 테스트 위치(파일 맨 끝, 487행)는 이 상속 정의보다 **뒤**였다.
3. pyflakes의 `F811`은 이름 그대로 "**미사용(unused)** 상태에서 재정의"만 잡는 규칙이다. 첫 번째 `RobotItem`(331행)이 두 번째 정의(487행) **전에** `RegisterRobotResponse`의 베이스 클래스로 이미 한 번 "사용"됐기 때문에, pyflakes는 "쓰이지 않은 채로 덮어썼다"고 보지 않아 경고를 내지 않았다.
4. 재정의 위치를 331행 **앞**으로 옮겨(실제 버그와 같은 순서 — 사용되기 전에 재정의) 다시 테스트하니 정상적으로 `F811`이 발생했다(2절의 검증 결과).

**원인**

이번 실제 사고가 우연히 "첫 정의가 두 번째 정의 전에 한 번도 안 쓰인" 조건에 맞아서 잡혔던 것 — yuna의 정의(125행)가 postwo의 정의(342행)보다 앞에 있었고, 그 사이에 `RobotItem`을 참조하는 코드가 없었다. 만약 순서가 반대였거나, 두 정의 사이 어딘가에서 그 이름을 상속·타입힌트 등으로 한 번이라도 참조했다면 flake8이 조용히 통과시켰을 것이다.

**해결**

별도 코드 수정은 하지 않음 — 이 한계를 `COMMIT_RULES.md`와 이 문서에 명시해, "flake8 pre-commit/CI가 통과했다고 이런 종류의 이름 충돌이 항상 없다는 보장은 아니다"라는 걸 팀이 알고 있게 함. 더 확실한 방어가 필요해지면 mypy나 pylint의 `E0102`(재정의) 같은 별도 검사 도입을 검토할 것(이번 세션에서는 보류).

**교훈**: 안전망을 새로 추가했다고 그걸 곧바로 신뢰하지 말고, **의도적으로 그 버그를 재현해서 실제로 잡히는지 검증**해야 한다. 첫 시도에서 못 잡았을 때 "도구가 잘못됐다"고 넘어가지 않고 왜 못 잡았는지 파고든 덕분에, 이 검사가 정확히 어떤 조건에서만 유효한지(그리고 어떤 조건에서는 무력한지)까지 알게 됐다 — 이런 한계를 모른 채 "flake8 붙였으니 안전하다"고 믿는 게 오히려 더 위험하다.

---

## 4. (미해결) 심각도 분류 규칙이 두 파일에 독립적으로 하드코딩됨

RobotItem 사고 이후 "이런 이름/서비스 중복이 또 있는지" 전수 조사하다가 발견. **아직 고치지 않았고, 이 문서에 기록만 남기기로 함** — 아래는 증상이 아니라 잠재 위험(risk) 기록이다.

**조사 과정**

- `schemas.py` 및 서버 전체 `.py`에서 클래스/함수 이름 중복: 재확인 결과 없음.
- 모든 `APIRouter(prefix=...)` path가 서로 겹치는지 재확인: 없음.
- 사용자가 구체적으로 의심한 "P3 대상자의 관리담당자 선택"과 "P7 계정관리"가 같은 데이터를 중복 조회하는지 확인 — `ApiClient::fetchOperators()`(api_client.cpp:628-646)가 P7과 **같은** `GET /accounts`를 호출한 뒤 클라이언트에서 `role_code=="operator" && is_active`만 거르는 구조였고, 이 필터는 이 함수 한 곳에만 있음. 서버에 별도 "담당자 목록" 엔드포인트가 따로 있지 않음 — **중복 아님**, 오히려 재사용이 잘 된 사례.
- `robot_careguard_ws/.../uplink_node.py`와 `careguard_turtlebot_agent/uplink_node.py`처럼 이름이 겹치는 파일도 점검 — LIMO/터틀봇 각각을 위한 의도적으로 분리된 구현(위치 소스가 `/amcl_pose`+TF 대 `/odom`으로 다름)이었고, 주석에도 그 이유가 명시돼 있어 실수가 아님. `ws_config.py`의 `ROBOT_CODE` 기본값이 서로 다른 것도 마찬가지로 의도적(LIMO 기본값 `R-01`과 겹치지 않게 터틀봇 쪽은 기본값을 비워둠).
- 위 과정에서 진짜로 같은 규칙이 두 곳에 따로 표현된 사례를 하나 발견함(아래).

**발견한 것**

```python
# app/api/v1/media.py:39-42 — 영상 태그(P4)용
SEVERITY_TAGS = {
    "emergency": ("긴급", "emergency"),
    "attention": ("주의", "attention"),
}
# "info"는 여기 없어서 DEFAULT_TAG("순찰")로 빠짐

# app/api/v1/recipient.py:63 — 대상자 위험도 카드(P3)용
AND severity IN ('attention', 'emergency')  # "info"는 위험도로 안 침
```

`recipient.py` 63행 바로 위 주석이 스스로 "media.py의 SEVERITY_TAGS와 같은 분류"라고 밝히고 있다 — 즉 두 곳 모두 "`info`는 특별 취급에서 제외한다"는 **같은 규칙을 의도적으로 맞추려 한** 흔적이 있는데도, 실제 표현은 하나는 파이썬 `dict`, 하나는 SQL 문자열 리터럴로 따로 박혀 있다.

**왜 위험한가**

RobotItem처럼 지금 당장 크래시가 나지는 않는다 — 문법적으로 완전히 독립된 두 표현이라 한쪽만 바뀌어도 에러가 안 난다. 그래서 오히려 더 늦게 발견될 수 있다: 나중에 심각도 단계가 하나 추가되거나(예: `"critical"`) 이름이 바뀌면, 한쪽만 고치고 다른 쪽을 깜빡하기 쉽고, 그 결과는 예외가 아니라 **조용한 동작 불일치**로만 나타난다(예: 새 등급의 이벤트가 P4에서는 "순찰"로 태그되는데 P3 카드에서는 위험으로 뜸, 혹은 그 반대).

**현재 상태**: 사용자 요청에 따라 수정하지 않고 기록만 남김. 고칠 때는 두 파일이 같은 상수(예: `RISKY_SEVERITIES = {"attention", "emergency"}`를 공용 모듈에 두고 SQL 쪽은 `severity = ANY(%s)`로 파라미터화)를 참조하게 하는 방향을 검토할 것.
