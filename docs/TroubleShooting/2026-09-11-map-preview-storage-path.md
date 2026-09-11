# 2026-09-11 트러블슈팅 — 맵·로봇 매핑 페이지에서 DB에 저장된 맵을 못 불러오는 문제

> 작성자: 최치언
> "매핑 현황 → 맵 미리보기"를 열면 DB엔 분명 맵이 있는데 이미지가 안 뜨는 증상을 조사하다가, 서버 코드 버그(경로 계산)와 데이터 문제(다른 컴퓨터에서 올라온 파일, 라이브 데이터와의 충돌)가 겹쳐 있던 걸 정리한 기록. 조사 도중 DB를 직접 여러 번 고치면서 실수(다른 사람이 실시간으로 만든 데이터와 충돌, 동시 편집 경합)도 있었고, 그 되돌리기 과정까지 포함한다.
> 대상 파일: `pc_careguard/server/app/core/config.py`, `pc_careguard/server/app/api/v1/media.py`, DB `files`/`map`/`waypoint` 테이블
> 관련 PR: `fix/media-storage-relative-path` → `develop`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | R-01~04 전부 "맵 미리보기"가 빈 상태(아직 매핑된 지도가 없습니다) | `config.py`의 `STORAGE_DIR` 계산이 2026-09-09 리팩터(`app/config.py`→`app/core/config.py`) 이후 한 단계 부족해져 실제 파일이 있는 `server/storage` 대신 빈 `server/app/storage`를 가리킴. 게다가 `files.storage_path`에 업로드한 사람 계정의 절대경로(`/home/ktel/...`)가 그대로 박혀 있어 다른 계정으로 clone한 컴퓨터에선 애초에 못 찾음 | `STORAGE_DIR` 계산 수정 + `storage_path`를 상대경로로 저장·해석하는 로직 추가(`resolve_storage_path`/`_relative_storage_path`/`_heal_legacy_path`), DB의 기존 절대경로도 실제 파일이 있는 것만 상대경로로 일괄 정리 |
| 2 | R-04의 최신 활성 맵 이미지(`files.id=31`, 이후 `41`)는 코드를 아무리 고쳐도 계속 404 | 팀원(yeeun)이 자기 컴퓨터에서 로컬로 띄운 서버로 업로드한 파일이라, 파일 바이트 자체가 이 컴퓨터의 `storage/`에 없음 (경로 문제가 아니라 실체 부재) | 코드로 해결 불가. `maps/`는 git에 커밋되는 폴더라 팀원이 실제로 커밋·푸시해야 해결됨 — 이 세션에서는 임시로 이전 로컬 저장본으로 되돌렸다가, 이후 "안전하게 되돌릴 수 있는 것만" 롤백하면서 다시 그 상태로 되돌아감(미해결로 남김) |
| 3 | 맵을 robot_id=291(R-07)로 옮긴 뒤 "팀원이 만든 마커(웨이포인트)가 R-04에서 안 보인다" | 원인 1을 조사하며 깨진 맵(`map.id=5`)을 임시로 R-07 소유로 옮겼는데, 그 사이 팀원이 R-04로 실제 SLAM 재매핑을 했다. `_save_slam_result`의 `ON CONFLICT(robot_id, name)`이 robot_id=4엔 더 이상 그 맵이 없어서(=충돌 없음) 기존 행을 덮어쓰는 대신 완전히 새 맵 행(`id=27`)을 만들었고, 예전 웨이포인트 5개는 여전히 옛 `map_id=5`(R-07 소유)에 남아 R-04 화면엔 안 보이게 됨 | 활성 웨이포인트 5개(`WP_27/28/29/30/32`)를 `map_id=5` → `map_id=27`로 이관(두 맵의 resolution/origin/크기가 동일함을 먼저 확인) |
| 4 | 경로 일괄 정리 스크립트를 두 번째 돌렸을 때 `id=4`(R-01)는 이미 상대경로였고 `id=5`(R-02)는 `./pc_careguard/server/storage/maps/R-02/...`라는, 스크립트가 만든 적 없는 깨진 값으로 되어 있었음 | 같은 시각 다른 사람이 DBeaver로 `files`/`map` 테이블을 열어두고 있었던 것(스크린샷으로 확인) + 재시작 전 옛 코드가 여전히 메모리에 떠 있던 서버 프로세스의 self-healing 로직이 겹쳐 발생한 동시 편집 경합 | 깨진 값을 올바른 상대경로(`maps/R-02/mapd_mod2.pgm`)로 재수정 |

---

## 1. R-01~04 맵 미리보기가 전부 안 뜨던 문제

### 증상

대시보드 "맵·로봇 매핑" 페이지의 "매핑 현황" 패널에서 등록된 맵(R-01/02/03/04 전부)의 미리보기를 열면 이미지가 안 뜨고 빈 상태로 나옴. `GET /maps/active`는 정상적으로 맵 목록(이름/좌표/`image_url`)을 반환하는데, 그 `image_url`(`/media/files/{id}`)을 호출하면 전부 `404 Not Found`.

### 진단 과정

1. `pc_careguard/dashboard/src/widgets/slam_mapping_card.cpp`, `p5_map_preview_dialog.cpp`, `p5_map_page.cpp`를 먼저 훑었지만 대시보드 쪽 로직(`registeredMaps_` 캐시, `fetchActiveMaps`/`fetchBytes` 흐름)은 정상이었음 — 이미지가 안 오는 건 서버 쪽 문제로 좁혀짐.
2. 서버 API(`pc_careguard/server/app/api/v1/media.py::get_media_file`)를 직접 curl로 호출:
   ```
   $ curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8000/media/files/4
   404
   ```
3. DB(`files` 테이블, `entity_type='map_image'`)를 직접 조회하니 `storage_path`가 전부 절대경로였고, 그 중 상당수가 `/home/ktel/DYNC_CareGuard/...`(이 컴퓨터 계정은 `coc`), 일부(R-04 최신 것)는 `/home/yeeun/DYNC_ws/...`(완전히 다른 사람의 클론 경로)로 되어 있었음.
4. `config.py`를 열어 `STORAGE_DIR` 계산을 직접 실행해보니:
   ```python
   >>> from app.core import config
   >>> config.STORAGE_DIR
   PosixPath('/home/coc/DYNC_CareGuard/pc_careguard/server/app/storage')  # 실제 파일은 server/storage에 있음
   ```
   `git log`로 추적하니 `430a7b6 refactor(server): app 모듈 재구성 + R-04 맵 폴더명을 robot_code 기준으로 통일`(2026-09-09) 커밋에서 `config.py`가 `app/config.py`→`app/core/config.py`로 한 단계 더 들어갔는데, `Path(__file__).resolve().parent.parent`는 그대로 둬서 `server/app/storage`(빈 폴더)를 가리키게 됐다. 실제 `server/storage/maps/`엔 파일이 멀쩡히 있었는데 서버가 엉뚱한 곳을 보고 있었던 것.
5. `/home/ktel/...` 계열은 "이 리포를 예전에 `ktel` 계정으로 clone해서 서버를 띄웠을 때 만들어진 값"으로 추정 — 실제로 `git log`상 이 리포는 여러 사람이 각자 로컬로 서버를 띄워 같은 원격 Postgres(`192.168.230.13`)에 붙는 구조였음(공유 DB + 로컬 파일 저장소 조합).

### 원인

두 가지가 겹쳐 있었다.
1. **`STORAGE_DIR` 계산 버그**: 위 4번 항목. 이 컴퓨터에서 서버를 새로 띄운 순간부터 업로드/조회 전부가 잘못된 폴더를 봤다.
2. **`storage_path`에 업로드 당시 컴퓨터의 절대경로를 그대로 저장**: `media.py::upload_video`/`upload_map_image`가 `str(storage_path)`(절대경로)를 DB에 그대로 넣고 있었다. DB(Postgres)는 공유인데 파일은 각자 로컬 디스크에만 있는 구조라, 절대경로를 그대로 공유 DB에 넣으면 "그 경로를 만든 사람의 컴퓨터"에서만 유효했다.

### 해결

1. `config.py`: `STORAGE_DIR`이 `server/storage`를 정확히 가리키도록 `.parent`를 한 번 더 추가.
2. `media.py`에 세 함수 추가:
   - `_relative_storage_path(absolute_path)`: 새로 업로드되는 파일은 `STORAGE_DIR` 기준 **상대경로** 문자열로 DB에 저장.
   - `resolve_storage_path(stored_value)`: 저장된 값이 상대경로면 그냥 `STORAGE_DIR`과 합치고, 절대경로(레거시)면 `config.STORAGE_DIR.name`(하드코딩된 `"storage"` 문자열이 아니라 실제 설정값 기준)이 나오는 지점 뒤쪽만 잘라 이 컴퓨터의 `STORAGE_DIR`과 재조합해본다. 폴더 구조(`STORAGE_DIR` 아래)는 어느 컴퓨터든 동일하므로, 계정/클론 경로만 다른 레거시 값도 코드 수정만으로 다시 읽힌다.
   - `_heal_legacy_path(file_id, stored_value, resolved_path)`: 레거시 절대경로를 성공적으로 찾아 서빙할 때마다 그 자리에서 DB 값을 상대경로로 조용히 정리 — 팀원 각자 컴퓨터에서 서버를 띄워도 수동 마이그레이션 없이 접근될 때마다 저절로 정리됨.
3. `get_media_file`/`write_map_yaml_sidecar`가 `Path(row["storage_path"])` 대신 `resolve_storage_path(...)`를 쓰도록 교체.
4. DB의 기존 `files.storage_path`(실제로 이 컴퓨터에 파일이 존재하는 것만, 20건) 절대경로 → 상대경로 일괄 정리. 파일 실체가 없는 값(아래 2절)은 잘못 덮어써 원본 위치 단서를 잃지 않도록 손대지 않음.
5. 브랜치 `fix/media-storage-relative-path`(base: `origin/develop`)를 새로 만들어 `config.py`/`media.py` 수정만 커밋 후 push. PR은 사용자가 직접 생성.

**검증**: 재시작 후 `curl /media/files/{4,5,6,7}`(R-01~04의 기존 맵 이미지) 전부 200 확인. `GET /maps/active` 응답과 대조해 정상 매칭 확인.

**교훈**: 여러 사람이 각자 로컬로 서버를 띄우면서 하나의 공유 DB를 보는 구조에서는, "로컬 디스크 경로"를 DB(공유 자원)에 절대경로로 저장하면 반드시 이런 문제가 생긴다. 파일 저장소를 코드 차원에서 "이식 가능한 값(상대경로)"으로 강제하지 않으면, 각자의 클론 위치·계정명이 바뀔 때마다 조용히 깨진다 — 에러 로그 하나 없이 그냥 404만 날 뿐이라 원인 파악이 오래 걸렸다.

---

## 2. R-04의 최신 맵 이미지가 실체 없이 DB에만 남아있던 문제

### 증상

1절을 고친 뒤에도 R-04의 **가장 최근** 활성 맵 이미지(처음엔 `files.id=31`, 이후 팀원이 재매핑하면서 `id=41`)만 계속 404.

### 진단 과정

`files.storage_path`를 확인하니:
```
id=31: /home/yeeun/DYNC_ws/pc_careguard/server/storage/maps/4/8eef39aae4884e4db726eff0e3d2a5ca.pgm
id=41: /home/yeeun/DYNC_ws/pc_careguard/server/app/storage/maps/R-04/d4b8ef40b03f4ce397fde3152dd8c158.pgm
```
`find`로 이 컴퓨터의 `server/storage/` 전체를 뒤져봐도 해당 파일명이 전혀 없음 — 경로 계산 문제가 아니라 **파일 바이트 자체가 이 컴퓨터에 온 적이 없음**. `.gitignore`를 확인하니:
```
pc_careguard/server/storage/*
!pc_careguard/server/storage/maps/
```
`storage/maps/`만 예외적으로 git 추적 대상이라, R-01~04의 기존 파일들은 git으로 커밋되어 있어 누가 clone하든 실체가 있었던 것(1절이 해결됐던 이유). 하지만 이 두 파일은 팀원이 로컬 서버로 업로드만 하고 아직 git에 커밋·푸시하지 않은 상태였다.

### 원인

- `storage/maps/`를 git으로 공유하는 방식 자체가 "누군가 커밋·푸시해야만" 다른 사람 컴퓨터에도 실체가 생기는 구조라, 실시간 SLAM 저장 직후에는 필연적으로 이런 공백이 생긴다.
- `id=41`은 추가로 `.../app/storage/...`라는 경로 조각이 남아있어, 업로드 당시 팀원 컴퓨터에서도 1절의 `STORAGE_DIR` 버그가 아직 안 고쳐진 상태였음을 알 수 있다(같은 버그가 여러 컴퓨터에 동시에 존재).

### 해결

코드로는 해결 불가능한 문제 — 실제 파일을 이 컴퓨터의 `server/storage/maps/R-04/`에 갖다 놓거나(팀원에게 직접 전달), 팀원이 git에 커밋·푸시해야 한다. 이 세션에서는:
1. 임시로 `map.id=5`(같은 맵의 이전 버전)를 이 컴퓨터에 실체가 있는 이전 로컬 저장본(`files.id=30`)으로 되돌려 미리보기가 뜨게 함.
2. 이후 "DB에서 안전하게 되돌릴 수 있는 것만" 롤백하는 과정에서 이 되돌림도 함께 원복되어, 현재는 다시 파일 실체가 없는 상태로 남아있음(**미해결**, 팀원의 git push 필요).

**교훈**: 맵 이미지처럼 크기가 작고 드물게 바뀌는 파일은 git으로 "공유 저장소" 대용을 쓸 수 있지만, 그 방식은 "커밋·푸시를 잊지 않는다"는 사람의 습관에 의존한다. 자동화(예: 저장 성공 시 서버가 자동으로 git add/commit/push, 또는 애초에 공유 네트워크 스토리지 사용)가 없으면 이런 공백이 반복될 것이다.

---

## 3. 라이브 데이터와 충돌 — map.robot_id 이관 후 마커(웨이포인트) 실종

### 증상

2절의 임시 복구로 `map.id=5`를 `robot_id=4`(R-04)에서 `robot_id=291`(R-07, 실제 LIMO 로봇)로 옮겼는데, 몇 분 뒤 사용자가 "팀원이 만든 마커들이 안 보인다"고 보고.

### 진단 과정

1. `map` 테이블을 다시 조회하니, 내가 옮긴 직후엔 없던 새 행이 생겨 있었음:
   ```
   id=27, robot_id=4, name='동산주택 2층', is_active=true, created_at=2026-09-11 14:52:12  (내가 map.id=5를 옮긴 지 9분 뒤)
   ```
2. `ws/robot_ws.py::_save_slam_result`를 보면 `INSERT ... ON CONFLICT (robot_id, name) DO UPDATE`로, 같은 로봇이 같은 이름으로 재저장하면 **기존 행을 덮어써서 map.id를 그대로 유지**하도록 설계돼 있다(그래야 기존 웨이포인트가 유효). 그런데 내가 `map.id=5`를 `robot_id=291`로 옮겨버린 바람에, robot_id=4로 들어온 이번 저장은 `(robot_id=4, name='동산주택 2층')` 조합의 기존 행을 못 찾아 **충돌 없이 새 행(id=27)을 INSERT**했다.
3. `waypoint` 테이블을 보니, 활성 웨이포인트 5개(`WP_27/28/29/30/32`)는 여전히 `map_id=5`(이제 R-07 소유)에 남아있고, 새로 생긴 `map_id=27`(R-04)에는 팀원이 재매핑 후 새로 찍은 2개(`WP_02/03`)뿐이었음.
4. `map.id=5`와 `map.id=27`의 `resolution`/`origin_x`/`origin_y`/`width_px`/`height_px`가 완전히 동일함을 확인 — 같은 물리 공간을 재매핑한 것이라 좌표계가 호환됨.

### 원인

내가 "깨진 파일을 되돌린다"는 목적으로 `map.robot_id`를 바꾼 행위가, 그 사이 진행 중이던 팀원의 **실시간 SLAM 재저장** 흐름이 의존하던 "robot_id+name으로 기존 행을 찾아 덮어쓴다"는 불변조건을 깨버렸다. 정적으로는 전혀 안 보이는, 라이브 시스템과의 시간차 경합(race)이었다.

### 해결

1. `map.id=5`를 다시 `robot_id=4`로 되돌리려 시도했으나, 이번엔 `map(robot_id, name)` UNIQUE 제약이 새로 생긴 `id=27`과 충돌해 거부됨 — 팀원의 새 데이터를 지우지 않는 한 완전한 원복은 불가능한 상태가 되어버림.
2. 대신 활성 웨이포인트 5개만 `map_id=5` → `map_id=27`로 이관(`UPDATE waypoint SET map_id=27 WHERE map_id=5 AND deregistered_at IS NULL`) — 좌표계가 동일함을 미리 확인했고, `waypoint_active_code_idx`(코드 중복)·`waypoint_one_charge_per_map_idx`(맵당 충전소 1개) 제약과도 충돌 없음을 사전 확인 후 실행.
3. `map.id=5`의 `robot_id`는 결국 R-07 소유로 남겨둠(사용자와 상의 후 결정) — R-04는 `map.id=27`을 계속 자기 맵으로 씀.

**검증**: `GET /waypoints?map_id=27`에 5개(이관분) + 2개(신규) = 7개 전부 조회됨.

**교훈**: DB를 직접 고칠 때는 "지금 이 행을 다른 프로세스(여기서는 로봇의 SLAM 저장 플로우)가 참조 중인지"를 반드시 확인해야 한다. 특히 `ON CONFLICT (a, b) DO UPDATE`처럼 "같은 키로 들어오면 기존 행을 재사용한다"는 코드가 있다면, 그 키(`robot_id`, `name`)를 임의로 바꾸는 순간 그 코드의 전제가 깨져 예상 못 한 새 행이 생길 수 있다.

---

## 4. 동시 편집 경합 — DBeaver로 손댄 값과 코드 재적용 순서 문제

### 증상

1절의 코드+DB 수정을 롤백했다가 사용자 요청으로 다시 적용하는 과정에서, 두 번째로 돌린 "절대경로 20건 일괄 정리" 스크립트가 21건이 아니라 19건만 처리됨. 확인해보니:
```
id=4: maps/R-01/mapd_origin.pgm                                   -- 이미 정상 상대경로
id=5: ./pc_careguard/server/storage/maps/R-02/mapd_mod2.pgm       -- 스크립트가 만든 적 없는 깨진 값
```

### 진단 과정

1. 마침 화면 스크린샷에 DBeaver로 `map`/`files` 테이블을 열어둔 창이 함께 떠 있는 게 보였음 — 같은 시각 사람이 직접 SQL 클라이언트로 이 테이블을 편집 중이었을 가능성.
2. `id=4`는 정상 상대경로였는데, 이건 **롤백으로 파일을 되돌리고 코드를 되돌리는 사이**에도 예전 코드가 여전히 메모리에 떠 있던 서버 프로세스가 계속 요청을 처리하며 `_heal_legacy_path`로 자동 정리했을 가능성이 큼(디스크 파일을 `git checkout`으로 되돌려도 이미 떠 있는 프로세스는 재시작 전까진 옛 코드로 계속 동작한다).
3. `id=5`의 값(`./pc_careguard/server/storage/maps/R-02/...`)은 `resolve_storage_path`/`_heal_legacy_path`가 절대 만들어내지 않는 형태(맨 앞 `./`, `storage`가 중간에 다시 등장)라 프로그램이 아니라 **사람이 직접 입력한 값**으로 보임 — 대화 중 사용자가 "절대경로 앞부분만 지우면 되는지" 질문했던 것과 같은 맥락의 수동 실험으로 추정.

### 원인

- 서버 코드 파일을 되돌리거나 다시 적용해도, **이미 실행 중인 프로세스는 재시작 전까지 메모리에 로드된 옛 코드로 계속 동작**한다는 걸 매번 챙기지 않으면 "분명 코드를 되돌렸는데 DB가 계속 자동으로 바뀐다"처럼 보이는 혼란이 생긴다.
- 같은 테이블을 사람(DBeaver)과 스크립트가 동시에 만지면, 스크립트의 가정("절대경로 아니면 이미 정리된 값")과 실제 값(사람이 반쯤 고치다 만 값)이 어긋나 조용히 스킵되고, 그 결과가 깨진 채로 방치될 수 있다.

### 해결

`id=5`를 올바른 상대경로(`maps/R-02/mapd_mod2.pgm`)로 재수정. `resolve_storage_path`로 실제 파일이 열리는지 직접 검증 후 반영.

**교훈**: 공유 DB를 여러 주체(서버 프로세스, 수동 스크립트, DBeaver로 접속한 사람)가 동시에 만지는 상황에서는 "내가 마지막으로 확인한 상태"를 그대로 믿지 말고, **쓰기 직전에 항상 다시 읽어 검증**해야 한다. 이번엔 다행히 드라이런/검증 습관 덕분에 깨진 값을 커밋 전에 잡았지만, 검증 없이 바로 밀어붙였다면 R-02도 다시 못 여는 상태로 방치될 뻔했다.
