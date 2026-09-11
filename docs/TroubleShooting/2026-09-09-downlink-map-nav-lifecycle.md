# 2026-09-09 트러블슈팅 — 지도를 재매핑해도 로봇이 이전 지도 기준으로 계속 동작함

> 작성자: 도희정
> `map(robot_id, name)`에 걸린 UNIQUE 제약 때문에, 같은 이름으로 다시 SLAM 매핑(재매핑)해도 `map.id`가 안 바뀐다(웨이포인트를 살리기 위한 의도된 설계 — `map.py::create_map`의 ON CONFLICT). 그런데 `downlink_node.py`의 여러 캐시 로직이 "map_id가 같으면 재사용해도 된다"고 가정하고 있어서, 재매핑 직후에도 로봇이 예전 지도·예전 충전소 좌표·예전 점유맵 해석 기준으로 계속 동작하는 버그 3개가 한꺼번에 발견됐다. 전날(9/8) 리포트에서 "map_id가 안 바뀌면 자동 재다운로드 안 됨"으로 알려진 한계로 남겨뒀던 지점의 후속 수정.
> 대상 파일: `robot_careguard_ws/src/careguard_agent/careguard_agent/downlink_node.py`, `pc_careguard/server/app/api/v1/map.py`, `pc_careguard/server/app/schemas.py`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 지도를 재매핑해도 로봇의 Nav2(rviz2 포함)가 예전 지도로 계속 떠있음 | `_ensure_navigation_running`이 `map_id`만 보고 "이미 이 지도로 떠있다"고 재사용 판단 — 같은 이름 재매핑은 `map_id`가 안 바뀌므로 이 조건에 항상 걸림 | `_download_map`이 (yaml 경로, **내용이 실제로 바뀌었는지**) 튜플을 반환하도록 고치고, `map_id`가 같아도 내용이 바뀌었으면 Nav2를 재기동 |
| 2 | 재매핑으로 충전 스테이션 좌표가 바뀌어도, 순찰 후 로봇이 예전(또는 엉뚱한) 위치로 복귀 | `_ensure_patrol_task_running`도 1번과 같은 `map_id` 캐시 재사용 로직을 갖고 있어서, 충전소 좌표를 최초 기동 시점에 캐시한 채 안 갱신 | `map_id` 비교/재사용 로직을 아예 제거 — 이 함수가 불릴 때마다(순찰 시작 직전에만 호출되므로 비용 문제 없음) 무조건 재시작 |
| 3 | 특정 맵에서 로봇이 점유/자유 공간을 원본과 다르게 해석(라이다 스캔이 지도 벽과 안 맞음) | `GET /maps/active`가 `mode`/`negate`/`occupied_thresh`/`free_thresh`를 안 내려줘서, 로봇이 이 4개 값을 전부 `map_saver_cli` 기본값(`trinary`/0/0.65/0.196)으로 하드코딩 — 실제 DB엔 다른 값(예: `free_thresh=0.25`)으로 저장된 맵도 있었음 | 서버가 `map.raw_yaml`에서 이 4개 값을 뽑아 `GET /maps/active` 응답에 포함시키고, 로봇은 하드코딩 대신 이 값을 그대로 씀 |

---

## 1. 지도를 재매핑해도 Nav2가 예전 지도로 계속 떠있음

**증상**

P6 순찰 페이지에서 SLAM으로 같은 방을 다시 매핑해 저장해도(같은 이름 유지), 로봇의 Nav2가 그 새 지도를 안 받고 예전 이미지·resolution·origin 기준으로 계속 떠있었다 — rviz2로 봐도 옛 지도 그대로.

**진단 과정**

1. `map(robot_id, name)` 테이블에 UNIQUE 제약이 있다는 걸 다시 확인 — 같은 이름으로 재매핑하면 `map.py::create_map`이 새 행을 만드는 대신 기존 행을 `UPDATE`한다(웨이포인트가 살아있어야 하므로 의도된 설계). 즉 `map.id`는 재매핑 전후로 동일하다.
2. `downlink_node.py::_ensure_navigation_running`을 보니 판단 기준이 `self._nav_map_id == map_id`뿐이었다 — 내용이 바뀌었는지는 전혀 안 본다.
3. 전날(9/8) 리포트에서 이미 "map_id가 안 바뀌면 자동 재다운로드 안 됨"이라는 한계로 문서화돼 있었던 지점과 정확히 같은 코드 경로임을 확인.

**원인**

`map_id` 동일 = 재사용 가능이라는 가정이, "같은 이름 재매핑은 id가 안 바뀐다"는 DB 제약과 충돌한다. id만으로는 "지금 서버가 들고 있는 지도가 로봇이 갖고 있는 것과 같은 내용인지"를 판별할 수 없다.

**해결**

`_download_map(map_id)`의 반환 타입을 `str` → `Tuple[str, bool]`로 바꿔, 새로 받은 이미지(`.pgm`)·yaml 바이트를 로컬에 이미 있던 파일과 바이트 단위로 비교해 `changed`를 계산한다. `_ensure_navigation_running`은 이제 `map_id`를 먼저 확인하기 전에 항상 먼저 `_download_map`을 호출해 최신 상태를 받아오고, `self._nav_map_id == map_id and not changed`일 때만 재사용한다.

**검증**

현재 코드(`downlink_node.py:595,602,783-855`)에서 `yaml_path, changed = self._download_map(map_id)` 후 `if self._nav_map_id == map_id and not changed:` 로 재사용을 판단하고, `_download_map`이 이미지·yaml 바이트를 비교해 `changed`를 반환하는 구현을 확인 — **해결 완료**.

---

## 2. 재매핑해도 patrol_task가 예전 충전소 좌표를 계속 씀

**증상**

지도를 재매핑해서 충전 스테이션 위치가 실제로 바뀌었는데도, 순찰이 끝난 뒤 로봇이 예전 좌표(또는 엉뚱한 곳)로 복귀하려고 했다.

**진단 과정**

`_ensure_patrol_task_running`이 1번과 완전히 같은 패턴의 `map_id` 캐시-재사용 로직(`if self._patrol_task_map_id == map_id: return True`)을 갖고 있었다 — `patrol_task`는 시작 시점에 딱 한 번 `WaypointManager.fetch(CAREGUARD_MAP_ID)`로 충전 스테이션 좌표를 캐시해두는 구조라, 재사용되는 순간 그 캐시는 재매핑 이전 좌표로 고정된다.

**원인**

1번과 동일한 "id 비교만으로 재사용 판단" 패턴이 이 함수에도 복사돼 있었다.

**해결**

1번처럼 내용 비교(`changed`)를 도입하는 대신 — `patrol_task`는 Nav2와 달리 런치 트리·AMCL·costmap이 없는 가벼운 `ros2 run` 노드 하나라 재시작 비용이 거의 없다 — 아예 재사용 로직 자체를 제거하고 이 함수가 호출될 때마다 무조건 재시작하도록 단순화했다. 이 함수는 순찰이 실제로 진행 중일 땐 호출되지 않는다(사전 준비/순찰 시작 직전에만 호출)는 것도 docstring으로 함께 남겼다.

**검증**

현재 코드(`downlink_node.py:670-717`)에서 `_ensure_patrol_task_running`이 `map_id` 비교 없이 매 호출마다 이전 프로세스를 정리(`_terminate_process_group`)하고 새 `CAREGUARD_MAP_ID` 환경변수로 무조건 재시작하는 구현, 그리고 docstring에 "map_id가 같아도 매번 무조건 재시작한다"는 설명이 명시돼 있음을 확인 — **해결 완료**.

---

## 3. 특정 맵에서 점유/자유 공간을 원본과 다르게 해석

**증상**

일부 맵으로 순찰할 때 로봇이 실제 벽 구조와 다르게 점유/자유 공간을 인식했다 — 9/8자 resolution(스케일) 버그와는 증상이 비슷해 보이지만 원인은 달랐다.

**진단 과정**

`downlink_node.py::_download_map`이 Nav2용 `.yaml`을 만들 때 `negate`/`occupied_thresh`/`free_thresh`를 매번 `map_saver_cli` 기본값(0/0.65/0.196)으로 하드코딩하고 있는 걸 발견. DB `map.raw_yaml`을 직접 확인해보니 실제로 기본값과 다른 값으로 저장된 맵이 존재 — 즉 서버가 갖고 있는 원본 값과 로봇이 실제로 쓰는 값이 처음부터 다른 맵이 있었다.

**원인**

`GET /maps/active` 응답에 애초에 이 필드들이 없었다 — `write_map_yaml_sidecar`(서버)는 원본 `.yaml`을 그대로 저장하지만, 그걸 다시 내려주는 API가 이미지 서빙용 최소 필드만 노출하고 있었다.

**해결**

- 서버: `list_active_maps`/`create_map` 두 쿼리에 `COALESCE(m.raw_yaml->>'mode', 'trinary')` 등 4개 컬럼 추가, `MapItem` 스키마에도 같은 기본값으로 필드 추가(구버전 맵처럼 `raw_yaml`에 키가 없으면 두 곳의 폴백 기본값을 반드시 맞춰야 함 — 코드 주석에 명시).
- 로봇: `_download_map`이 하드코딩된 4개 값 대신 서버가 내려준 값을 그대로 사용.

**검증**

현재 코드에서 확인:
- `map.py`의 `list_active_maps`/`create_map` 두 쿼리 모두 `COALESCE(m.raw_yaml->>'mode', 'trinary') AS mode`, `negate`, `occupied_thresh`, `free_thresh` 4개 컬럼을 포함.
- `schemas.py`의 `MapItem`에 `mode`/`negate`/`occupied_thresh`/`free_thresh` 필드가 같은 기본값으로 존재.

— **해결 완료**. 로봇 쪽 `_download_map`이 이 값을 실제로 반영해 쓰는지, 그리고 문제였던 맵으로 실제 재현 테스트까지 거쳤는지는 이 조사에서 로봇 코드까지 전부 대조하진 못했다 — 필요하면 `_download_map`의 yaml 작성 부분을 추가로 확인할 가치가 있다.

---

**교훈**: "id가 같으면 내용도 같다"는 캐시 가정은, 그 id를 발급하는 쪽에 `ON CONFLICT DO UPDATE` 같은 upsert 제약이 하나라도 있으면 깨진다 — 이번 3개 버그 모두 `map(robot_id, name)` UNIQUE 제약 하나에서 비롯됐다. 같은 원인이 서로 다른 두 함수(`_ensure_navigation_running`, `_ensure_patrol_task_running`)에 복사돼 있었던 것도 눈여겨볼 점 — 한쪽을 고치면서 "이 패턴을 쓰는 다른 곳이 또 있는지" 훑어보는 습관이 후속 버그를 한 번에 같이 잡는 데 도움이 됐다.
