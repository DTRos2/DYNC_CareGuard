# 2026-09-10 트러블슈팅 — 터틀봇·LIMO 원격 연결(Tailscale) 작업 중 겪은 문제들

> 작성자: 최치언
> 다른 공유기에 있는 터틀봇(R-06)·LIMO(R-07)를 Tailscale VPN으로 CareGuard 서버에 연결하는 작업(`feature/turtlebot-agent-autostart` 브랜치) 중 실제로 겪은 증상·원인·해결 기록. Tailscale 채택 이유 자체는 `docs/technical document/2026-09-10-tailscale-vpn-robot-connectivity.md` 참고, 이 문서는 그 과정에서 걸린 개별 문제들만 다룬다.
> 대상: `pc_careguard/dashboard`, `careguard_turtlebot_agent`, `robot_careguard_ws/src/careguard_agent`, `pc_careguard/server/app/ws/robot_ws.py`, `pc_careguard/server/tests`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 대시보드 실행 시 `Aborted (core dumped)` | Qt 6.5 xcb 플랫폼 플러그인이 요구하는 `libxcb-cursor0` 미설치 | `sudo apt install libxcb-cursor0` |
| 2 | 로봇 프로세스를 껐다 다시 띄웠는데 옛 설정(잘못된 서버 주소 등)으로 계속 연결 시도 | `ros2 run`의 부모 프로세스만 `kill`하면 실제 실행 파일인 자식 프로세스가 좀비로 남아 계속 돎 | 자식 PID까지 `kill -9`로 같이 종료 |
| 3 | LIMO(R-07) `uplink_node`가 "timed out during opening handshake"만 반복 | 서버 주소를 Tailscale IP로 줬는데, 그 LIMO엔 Tailscale이 설치돼 있지 않아 그 주소 자체에 도달 불가 (같은 공유기라 필요 없었음) | `CAREGUARD_SERVER_URL`을 서버의 로컬 IP(`192.168.230.7`)로 변경 |
| 4 | 서버를 재시작하면 대시보드에 로봇이 잠깐 "오프라인"으로 보임 | 서버 프로세스가 내려갔다 올라오는 동안 WebSocket 연결이 끊기고, 로봇은 정해진 재시도 주기(수십 초)를 기다렸다가만 재접속함 | 정상 동작 — 재연결 주기가 지나면 자동 복구, 급하면 로봇 쪽 uplink 프로세스를 직접 재시작해 즉시 재연결 유도 가능 |
| 5 (미해결) | 대시보드 UI에서 로봇 삭제 후 재등록 테스트 중 `R-06`이 실제로는 터틀봇인데 모델이 `limo`로 잘못 등록됨 | 재등록 시 모델 드롭다운을 잘못 선택(사람 실수, 코드 문제 아님) | 아직 안 고침 — 대시보드에서 모델을 `turtlebot`으로 수정 필요 |
| 6 (미해결) | 서버 `pytest`가 `conftest.py`도 못 읽고 즉시 실패 | 서버 venv의 `starlette`가 최신 버전(1.6.0)으로 깔려 있는데, 이 버전이 `httpx2`라는 패키지를 요구함(`fastapi`/`starlette` 의존성 드리프트로 보임, 이번 세션 변경과 무관) | 아직 안 고침 — 이번엔 테스트 코드를 pytest 없이 직접 스크립트로 실행해 우회 검증함 |

---

## 1. 대시보드 실행 시 `Aborted (core dumped)`

**증상**

```
$ ./build/careguard_dashboard
qt.qpa.plugin: From 6.5.0, xcb-cursor0 or libxcb-cursor0 is needed to load the Qt xcb platform plugin.
qt.qpa.plugin: Could not load the Qt platform plugin "xcb" in "" even though it was found.
This application failed to start because no Qt platform plugin could be initialized.
Aborted (core dumped)
```

**원인**

Qt 6.5부터 xcb 플랫폼 플러그인이 `libxcb-cursor0`(또는 `xcb-cursor0`)을 새로 요구하는데, 이 패키지가 시스템에 설치돼 있지 않았다. 바이너리 자체는 정상 — 재빌드해도 같은 에러가 남는다.

**해결**

```bash
sudo apt install libxcb-cursor0
```

설치 후 재실행하니 바로 정상 기동. 빌드를 다시 할 필요조차 없었다(`make`는 변경 사항이 없어 캐시된 결과 그대로 "Built target" 출력하고 끝).

---

## 2. `ros2 run` 프로세스를 껐는데도 옛 연결이 계속 남아있음

**증상**

R-05(aurix)·R-06(yeyu)·R-07(wego) 세 로봇 모두에서 반복적으로 겪음: `uplink_node`/`downlink_node`를 새 환경변수(수정된 서버 주소 등)로 다시 띄우려고 이전 프로세스를 종료했는데, 로그를 보면 여전히 **옛 설정**으로 연결 시도를 계속하고 있었다.

```bash
$ pgrep -af uplink_node
2153 /usr/bin/python3 /opt/ros/humble/bin/ros2 run careguard_agent uplink_node   # 이건 죽었는데
2155 /usr/bin/python3 /home/aurix/.../uplink_node                               # 이건 살아있음
```

**원인**

`ros2 run <pkg> <exe>`는 실제로 파이썬 실행 파일을 **자식 프로세스**로 fork해서 띄운다. `pkill`이나 `kill <PID>`로 `ros2 run` 명령 자체의 PID(부모)만 죽이면, 이미 fork된 자식(실제 `uplink_node`/`downlink_node` 프로세스)은 고아 프로세스로 남아 계속 돈다 — 겉보기엔 "종료했다"고 착각하기 쉽다.

**해결**

`pgrep -af`로 나온 PID를 **부모·자식 둘 다** 확인해서 같이 종료:

```bash
kill -9 <부모PID> <자식PID>
```

재시작 스크립트를 짤 때도 `pkill -f uplink_node`처럼 이름으로 매칭하는 편이 PID 하나만 죽이는 것보다 안전하다.

**교훈**: 백그라운드로 띄운 ROS2 노드를 재시작할 때는 `pgrep -af`로 정말 다 죽었는지 재확인하는 습관이 필요하다. 특히 이번처럼 서버 주소를 실수로 잘못 준 뒤 "고쳐서 다시 띄웠는데 왜 안 되지"라고 헷갈리는 상황이면, 옛 고아 프로세스가 여전히 같은 `robot_code`로 붙어 있어서 서버 쪽 연결(`_connections` 딕셔너리)을 잘못된 쪽이 계속 차지하고 있을 가능성부터 의심해야 한다.

---

## 3. LIMO(R-07) uplink_node가 계속 "timed out during opening handshake"

**증상**

R-06(yeyu)과 같은 방식으로 R-07(LIMO, `wego@192.168.230.42`)의 `CAREGUARD_SERVER_URL`을 서버의 Tailscale IP(`100.107.185.97`)로 주고 띄웠는데, 연결이 계속 실패:

```
[WARN] [uplink_node]: 업링크 연결 실패, 3.0s 후 재시도: timed out during opening handshake
```

**진단 과정**

1. `ping 100.107.185.97`을 LIMO에서 실행 → 100% 패킷 손실.
2. `which tailscale` → 아무것도 안 나옴. **이 LIMO엔 Tailscale이 아예 설치돼 있지 않았다.**
3. LIMO의 IP(`192.168.230.42`)를 보니 서버 PC의 와이파이 IP(`192.168.230.7`)와 **같은 대역** — 애초에 Tailscale이 필요 없는 상황이었다. `curl http://192.168.230.7:8000/health`로 로컬 IP 도달 확인.

**원인**

R-06(yeyu)은 실제로 다른 공유기에 있어서 Tailscale이 필요했지만, R-07(LIMO)은 이 서버 PC와 **같은 공유기**를 쓰고 있었다 — 습관적으로 "지금까지 쓰던 Tailscale IP"를 그대로 재사용하려다 생긴 실수였다. 로컬 IP로 충분한 상황에 굳이 Tailscale(그것도 설치 안 된)을 거치려 한 것.

**해결**

`CAREGUARD_SERVER_URL`을 서버의 로컬 IP로 교체 후 재시작 → 즉시 연결 성공.

```
[INFO] [uplink_node]: 서버 업링크 연결됨: ws://192.168.230.7:8000/ws/robot?robot_code=R-07&role=uplink
```

**교훈**: Tailscale IP는 "다른 네트워크에 있을 수도 있는 로봇"을 위한 범용 주소지, 모든 로봇에 무조건 써야 하는 게 아니다. 로봇을 새로 연결할 때마다 "이 로봇이 서버랑 같은 공유기인지"부터 먼저 확인하고 주소를 고르는 게 맞다 — 같은 공유기인데 Tailscale을 거치면 그 로봇에 Tailscale 자체를 새로 설치해야 하는 불필요한 작업이 추가된다.

---

## 4. 서버 재시작 시 대시보드에서 로봇이 잠깐 "오프라인"으로 보임

**증상**

서버(대시보드 내장 uvicorn)를 재시작할 때마다, 이미 연결돼 있던 로봇들이 대시보드에서 일시적으로 오프라인으로 표시됨. 사용자가 "왜 지금 오프라인이야?"라고 여러 번 확인함.

**원인**

서버 프로세스가 내려가면 로봇 쪽 WebSocket 연결이 강제로 끊긴다. 로봇의 `uplink_node`/`downlink_node`는 즉시 재연결하지 않고, 설정된 재연결 주기(시스템 설정 > 임계값·알림의 "재연결 재시도 주기", 기본 3~30초 사이)만큼 기다렸다가 재시도한다 — 서버가 다시 뜨는 타이밍과 로봇의 재시도 타이밍이 안 맞으면 최대 그 주기만큼 오프라인으로 보인다.

**해결**

별도 조치 불필요 — 재연결 주기가 지나면 자동으로 복구된다(실제로 반복 확인함, 최대 30초 정도 소요). 화면 확인이 급하면 로봇 쪽에서 `uplink_node`를 직접 재시작해 즉시 재연결을 유도할 수 있다.

**교훈**: 서버를 재시작할 계획이면, 그 직후 몇십 초간은 로봇이 오프라인으로 보이는 게 정상이라는 걸 미리 알고 있어야 "또 뭔가 고장났나" 하고 불필요하게 재조사하지 않는다.

---

## 5. (해결) `R-06` 재등록 테스트 중 모델이 `turtlebot`→`limo`로 잘못 등록됨

로봇 삭제 후 재등록이 되는지 대시보드 UI로 직접 테스트하는 과정에서, `R-06`(실제로는 `yeyu` 터틀봇, IP `100.110.210.121`)이 모델 `limo`로 재등록된 채 남아있는 게 발견됨. 연결(WebSocket) 자체는 `robot_code` 문자열만으로 식별되므로 온라인 상태나 데이터 수신에는 지장이 없지만, 대시보드에 표시되는 로봇 종류가 실제와 다르다.

**현재 상태**: db 수정

---


