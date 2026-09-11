# 2026-09-07 트러블슈팅 — careguard_agent/server > 영상 업로드 (robot_id 요구사항)

> 작성자: 김예은
> `feature/perception-video-recording` 브랜치에서 6단계(video_recorder.py)의
> 결과물을 서버로 올리는 7단계를 설계하던 중 발견한 문제의 증상·원인·해결을 정리한 기록.
> 대상 파일: `pc_careguard/server/app/media.py`,
> `robot_careguard_ws/src/careguard_perception/careguard_perception/config.py`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | ROS2(video_recorder.py)가 만든 영상을 `POST /media/upload`로 올리려는데, 그 로봇의 숫자 `robot_id`를 ROS2 쪽이 알 방법이 없어 업로드 자체가 불가능한 상태였음 | `POST /media/upload`가 대시보드(이미 robot_id를 아는 호출자)만 호출한다고 가정해 `robot_id`를 필수로 요구하도록 설계돼 있었음 | `/media/upload-map`이 이미 쓰던 패턴(robot_code로 받아 서버가 변환)을 `/media/upload`에도 동일하게 확장 |

---

## 1. video_recorder.py가 robot_id를 몰라 업로드 불가능

**증상**

`video_recorder.py`(6단계)가 mp4+json 사이드카를 다 만든 뒤, 이걸 `POST /media/upload`로 올리는 7단계를 설계하는 중이었다. 그런데 이 엔드포인트는 이렇게 정의돼 있었다:

```python
robot_id: int = Form(..., description="촬영한 로봇 id"),
```

`robot_id`는 DB `robot` 테이블의 숫자 PK인데, ROS2 쪽은 `CAREGUARD_ROBOT_CODE` 환경변수로 `robot_code`(예: `"R-01"`, 문자열)만 갖고 있다. 로봇 목록 조회 API(`robots.py`)가 이번 스코프 밖이라, ROS2 쪽엔 애초에 `robot_code -> robot_id`를 변환할 방법이 없었다.

**진단 과정**

사이드카 메타데이터에 뭘 채워야 업로드가 가능한지 검토하다가 발견했다. 처음 든 생각은 "그럼 업로드 직전에 robot_id를 조회하는 API 호출을 하나 더 넣어야 하나"였는데, 같은 문제를 이미 겪은 선례가 있는지 `media.py`를 훑어보니 `POST /media/upload-map`(맵 이미지 업로드)이 정확히 같은 상황이었다:

```python
# upload_map_image() 발췌
robot_code: str = Form(..., description="매핑을 수행한 로봇 code (예: R-04)"),
...
cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
```

맵 업로드도 대시보드가 `robot_id`를 모르는 채로 호출해야 해서 이미 `robot_code`를 받아 서버가 직접 변환하는 방식으로 풀어놨던 것이다.

**원인**

`POST /media/upload`가 처음 만들어질 때는 호출자가 대시보드(이미 `robot_id`를 아는 쪽) 하나뿐이라고 가정했다. 이후 ROS2(로봇)가 직접 이 엔드포인트를 호출하는 시나리오(6→7단계)가 새로 추가되면서, `robot_id`를 모르는 호출자가 생긴 게 원인이었다 — 엔드포인트 설계 당시엔 예상하지 못했던 사용처였다.

**해결**

`/media/upload-map`의 패턴을 그대로 `/media/upload`에도 적용했다: `robot_id`를 `Optional`로 바꾸고 `robot_code`를 추가로 받아, `robot_id`가 없으면 `robot_code`로 `robot` 테이블을 조회해 변환한다(둘 다 없으면 400, `robot_code`로 못 찾으면 404, 둘 다 오면 `robot_id` 우선).

```python
robot_id: Optional[int] = Form(None, ...),
robot_code: Optional[str] = Form(None, ...),
...
if robot_id is None:
    if not robot_code:
        raise HTTPException(status_code=400, detail="robot_id 또는 robot_code 중 하나는 필요합니다")
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        robot = cur.fetchone()
    if robot is None:
        raise HTTPException(status_code=404, detail="로봇을 찾을 수 없습니다")
    robot_id = robot["id"]
```

`video_recorder.py`의 사이드카는 `robot_code`만 채우면 되도록 통일했고(`config.py`의 `ROBOT_CODE`), 이미 `robot_id`를 아는 기존 호출자(대시보드)는 코드 변경 없이 그대로 동작한다.

**교훈**

- 같은 업로드 엔드포인트를 여러 종류의 호출자(대시보드/로봇)가 쓰게 될 걸 예상하지 못하면, 나중에 "이미 아는 값"만 필수로 요구해둔 게 새 호출자를 막는 걸림돌이 된다.
- 이번엔 `/media/upload-map`에서 이미 검증된 해결 패턴이 있어서 새로 고민할 필요 없이 그대로 재사용할 수 있었다 — 비슷한 문제를 다른 엔드포인트가 먼저 겪었는지 확인하는 게 시간을 아꼈다.
