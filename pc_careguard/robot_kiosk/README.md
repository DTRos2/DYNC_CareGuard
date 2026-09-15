# 로봇 후면 상태 화면 (kiosk)

로봇 뒷면 7인치 모니터에 띄우는 최소 상태 화면. 빌드 없이 `index.html`을 브라우저로
열면 끝 — 새 백엔드 없이 기존 `/ws/dashboard`, `GET /robots/{id}/thresholds`,
`GET /patrol-runs`만 사용한다.

## 실제로 나오는 화면

- 평소: 배터리 %(초록, 크게) + "대기 중" / "순찰 중" 배지 + 온라인 표시 + 시각
- 배터리가 임계값(기본 30%, `battery_attention_pct`) 이하: 전부 빨간 계열로 바뀌고
  배지가 "충전 필요 · 복귀 중"으로 전환
- 화면이 서버와 끊기면 배지 대신 "서버 연결 끊김 · 재연결 중"이 뜨고, 3초 간격으로
  자동 재접속을 시도한다(끊긴 동안 마지막 온라인/오프라인 상태를 그대로 보여주지
  않도록, 재연결 전까지는 "온라인"이라고 단정하지 않는다).

## 요구사항 스펙과 실제 백엔드가 다른 부분 (백엔드 새로 안 만들기로 한 결과)

애초 요청한 `patrol_start` / `patrol_stop` / `dock_result` WS 메시지 타입은
`dashboard_ws.py`가 실제로 broadcast하는 목록에 없다(전수 검색 확인, 2026-09-15
기준). 새 엔드포인트를 만들지 않기로 한 범위 안에서 아래처럼 대체했다:

| 요청했던 것 | 실제로 쓴 것 | 비고 |
|---|---|---|
| `patrol_start` | `patrol_progress` 수신 시 순찰 중으로 간주 | `patrol_progress`는 순찰 중에만 온다 |
| `patrol_stop` | `patrol_done` 수신 시 정지로 간주, `patrol_run_updated` 수신 시 재조회 | `patrol_run_updated`는 취소뿐 아니라 **순찰 시작 시점**과 SOS가 기존 순찰을 paused로 미룰 때도 broadcast된다(2026-09-15 코드 확인, `_start_patrol_run`/`cancel_patrol_run`/`drive_to_point` 세 곳 전부) — 방향을 이 메시지만으로 알 수 없어서, 값을 추측하는 대신 `GET /patrol-runs?status=running`을 다시 호출해 실제 상태를 가져온다(다른 대시보드 페이지들과 같은 패턴) |
| (첫 진입 시 순찰 중 여부) | `GET /patrol-runs?robot_id=&status=running` | WS 쪽엔 순찰 시작/종료에 대한 hydration이 없어서, 화면을 새로 켰을 때 "이미 순찰 중"인지 알 방법이 이 기존 엔드포인트(P6가 새로고침 시 쓰는 것과 동일)뿐이었다. 이 쿼리는 `paused`(SOS가 잠시 밀어낸 순찰)도 `running`으로 같이 잡아준다(`api/v1/patrol.py::list_patrol_runs` 문서 참고) |
| `dock_result` | (사용 안 함) | 이 타입 자체가 백엔드에 없다. 저배터리 자동 복귀는 서버가 로봇에게 직접 명령만 보내고(`robot_ws.py::_handle_battery_state`) 대시보드로는 알리지 않는다. 다행히 요구사항 4번 문구("배터리 부족 시... 복귀 중으로 전환")가 이미 배터리 %만으로 판단하는 로직이라, 이 화면은 `dock_result` 없이도 스펙대로 동작한다 |

## 인증 없음

`GET /robots/{id}/thresholds`, `GET /patrol-runs` 둘 다 이 브랜치(develop 기반)엔
인증 자체가 없다 — 세션 토큰 방식은 채택하지 않기로 결정됐고(역할별 구분은 로그인
정보 기준으로 대시보드 화면만 다르게 구성하는 것으로 정리됨), `core/auth.py`가 이
브랜치엔 아예 없다(`api/v1/robots.py`/`api/v1/patrol.py`에 `Depends`/`require_roles`
없음, 직접 확인). 그래서 이 페이지는 토큰을 전혀 다루지 않는다 — 헤더 없이 그냥
호출한다.

## 설정

전부 URL 쿼리 파라미터로 넘긴다 (파일 상단 `CONFIG` 기본값을 직접 고쳐도 됨):

```
index.html?robot=R-01&robot_id=1&server=http://192.168.230.13:8000
```

- `robot` — `dashboard_ws` 메시지의 `robot_code`와 매칭할 값 (기본 `R-01`)
- `robot_id` — thresholds/patrol-runs 조회에 쓸 숫자 DB id (기본 `1`)
- `server` — 서버 주소. `dashboard/.env`의 `CAREGUARD_SERVER_URL`과 같은 값

## 로봇에서 풀스크린(kiosk)으로 띄우기

로봇 온보드 PC에 Chrome/Chromium이 있다는 전제. 터미널에서:

```bash
chromium-browser --kiosk --noerrdialogs --disable-infobars \
  --incognito \
  "file:///home/<user>/DYNC_CareGuard/pc_careguard/robot_kiosk/index.html?robot=R-01&robot_id=1&server=http://192.168.230.13:8000"
```

- `--kiosk`: 주소창/탭 없이 전체화면
- `--incognito`: 매번 깨끗한 상태로 시작(캐시된 옛날 화면이 잠깐 보이는 것 방지)
- 부팅 시 자동 실행하려면 이 명령을 `~/.config/autostart/careguard-kiosk.desktop`
  같은 autostart 항목이나 systemd user service로 등록
- `file://`로 직접 열어도 되고, 원한다면 로봇 PC에서 `python3 -m http.server`로
  이 폴더를 서빙해 `http://localhost:8080/index.html?...`로 열어도 된다(둘 다
  동일하게 동작 — `fetch`/`WebSocket` 대상은 `server` 파라미터가 결정하므로 이
  폴더를 어디서 서빙하는지와 무관함)
- 종료: `Alt+F4` 또는 `xdotool key alt+F4` (원격 SSH에서 kiosk 끄고 싶을 때)

## 참고: 목업 이미지에 대해

요청에 첨부됐다는 목업 두 장은 이 세션에서 실제로 전달받지 못했다(첨부 파일 없음).
그래서 레이아웃은 요구사항 4번 텍스트 설명(배터리 %는 크게, 상태 배지, 온라인 표시,
시각, 평소 초록/경고 빨강)만으로 구성했다 — 실물 목업과 다르면 알려주면 맞춰
수정한다.
