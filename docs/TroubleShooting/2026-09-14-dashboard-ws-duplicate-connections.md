# 2026-09-14 트러블슈팅 — 대시보드 하나가 서버에 `/ws/dashboard` 소켓을 7개씩 물고 있던 문제

> 작성자: 도희정
> `sudo lsof -i :8000`으로 서버 포트에 붙은 커넥션을 점검하던 중, 대시보드 프로세스(`careguard_dashboard`) 단 하나가 같은 서버 IP:PORT로 ESTABLISHED 커넥션을 7개나 물고 있는 걸 발견. 처음엔 "재연결 로직이 끊긴 소켓을 안 닫고 계속 새로 여는 것 아니냐"고 의심했는데, 실제 원인은 재연결 로직이 아니라 페이지 수만큼 `WsClient`가 중복 생성되고 있던 것.
> 대상 파일: `pc_careguard/dashboard/src/net/ws_client.cpp`, `pc_careguard/dashboard/src/windows/mainwindow.{h,cpp}`, `pc_careguard/dashboard/src/pages/p{1,3,4,5,6,7,8}_*.{h,cpp}`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 대시보드 프로세스 1개가 서버 8000번 포트에 ESTABLISHED 커넥션 7개를 물고 있음 | P1/P3/P4/P5/P6/P7/P8 7개 페이지가 각자 자기 소유의 `WsClient`(=`QWebSocket` 1개)를 따로 만들어 `/ws/dashboard`에 독립적으로 연결 | `MainWindow`에서 `WsClient`를 하나만 만들어 7개 페이지에 공유 주입하도록 리팩터링 |

---

## 1. 대시보드 프로세스 하나가 소켓을 7개씩 물고 있음

**증상**

```
$ sudo lsof -i :8000
COMMAND      PID USER   FD   TYPE DEVICE SIZE/OFF NODE NAME
careguard 194575   hj   25u  IPv4 796213      0t0  TCP codelabHJ:43686->192.168.230.7:8000 (ESTABLISHED)
careguard 194575   hj   41u  IPv4 796214      0t0  TCP codelabHJ:43738->192.168.230.7:8000 (ESTABLISHED)
careguard 194575   hj   46u  IPv4 796215      0t0  TCP codelabHJ:43758->192.168.230.7:8000 (ESTABLISHED)
careguard 194575   hj   47u  IPv4 796216      0t0  TCP codelabHJ:43772->192.168.230.7:8000 (ESTABLISHED)
careguard 194575   hj   53u  IPv4 796217      0t0  TCP codelabHJ:43810->192.168.230.7:8000 (ESTABLISHED)
careguard 194575   hj   56u  IPv4 796218      0t0  TCP codelabHJ:43832->192.168.230.7:8000 (ESTABLISHED)
careguard 194575   hj   59u  IPv4 796219      0t0  TCP codelabHJ:43854->192.168.230.7:8000 (ESTABLISHED)
```

전부 같은 PID(같은 대시보드 프로세스), 같은 원격 주소. 먼저 고아 프로세스(부모가 죽어서 PID 1로 재부모화된 프로세스)인지 의심했으나 `ps -o pid,ppid,stat`로 확인한 결과 PPID가 살아있는 bash 셸이라 고아는 아니었다 — 진짜 의문은 "왜 한 프로세스가 같은 목적지로 소켓을 7개나 여는가"였다.

**진단 과정**

1. `reconnect`로 grep해서 재연결 관련 코드부터 확인. `ws_client.cpp`는 `socket_`(단일 `QWebSocket*` 멤버)을 재사용하고, `connectToServer()`도 이미 연결/연결 중이면 바로 리턴하는 가드가 있어 이 파일 자체에서 소켓이 누적될 구조가 아니었다. `p7_settings_page.cpp`의 "reconnect"는 로봇 재연결 재시도 주기 **설정값**(숫자 입력 필드)일 뿐 실제 소켓 코드가 아니었다.
2. `grep -rn "new WsClient"`로 실제 생성 지점을 전부 찾아보니, `P1OverviewPage`/`P3RecipientPage`/`P4VideoPage`/`P5MapPage`/`P6PatrolPage`/`P7SettingsPage`/`P8LogsPage` — **정확히 7개 페이지가 각자 생성자에서 `wsClient_ = new WsClient(this); ... wsClient_->connectToServer();`를 실행**하고 있었다. 페이지 수(7)와 lsof에서 본 커넥션 수(7)가 정확히 일치.
3. `mainwindow.cpp`를 보면 이 7개 페이지는 앱 시작 시 `pageStack_->addWidget(...)`로 전부 한 번에 생성돼 계속 살아있는 위젯들 — 화면엔 한 페이지만 보여도 나머지 6개도 백그라운드에서 각자 독립적인 `/ws/dashboard` 연결과 재연결 타이머(3초 간격)를 돌리고 있었다.

**원인**

`WsClient`를 페이지별 멤버로 두고 각 페이지 생성자에서 직접 `new`하는 패턴이 P1 이후 P3/P4/P5/P6/P7/P8에 그대로 복사됐다(`p3_recipient_page.cpp`, `p4_video_page.h` 등의 주석에도 "P5/P6와 같은 패턴 — 페이지마다 자기 WsClient를 따로 둔다"라고 명시돼 있었음 — 의도된 설계였다). 페이지 하나짜리 앱이면 문제가 없지만, 8개 페이지가 전부 한 프로세스 안에서 상시 살아있는 이 대시보드 구조에서는 동일 엔드포인트로 소켓과 재연결 루프가 페이지 수만큼 중복 생성되는 결과로 이어졌다.

**해결**

- `MainWindow`에 `WsClient* wsClient_` 멤버를 추가하고 생성자에서 단 한 번만 생성.
- 각 페이지의 `WsClient` 생성 로직(`new WsClient(this)` + `connectToServer()`)을 제거하고, 대신 `setWsClient(WsClient* client)`라는 공개 메서드를 추가해서 전달받은 공유 인스턴스에 `messageReceived`(+ P5/P6/P7은 `connectionStateChanged`까지)만 구독하도록 변경.
- `MainWindow`에서 페이지 8개를 전부 생성한 뒤, `WsClient`를 만들어 7개 페이지에 `setWsClient()`로 내려주고, **모든 페이지가 구독을 마친 다음에** `wsClient_->connectToServer()`를 한 번만 호출. (순서를 반대로 하면 최초 연결이 이미 끝난 뒤에 뒤늦게 구독하는 페이지가 `connectionStateChanged(true)`를 못 받아 P5/P6의 "연결 끊김" 배너가 계속 떠 있는 상태로 남을 수 있어 순서를 명시적으로 맞췄다.)

**검증**

리팩터링 전 바이너리로 떠 있던 프로세스(PID 194575, 커넥션 7개)를 종료하고 재빌드한 바이너리로 재시작(PID 243386) 후 다시 `lsof -i :8000`으로 확인:

```
careguard 243386   hj   47u  IPv4 ... ESTABLISHED
careguard 243386   hj   56u  IPv4 ... ESTABLISHED
careguard 243386   hj   62u  IPv4 ... ESTABLISHED
careguard 243386   hj   63u  IPv4 ... ESTABLISHED
careguard 243386   hj   64u  IPv4 ... ESTABLISHED
```

7개 → 5개로 감소. `/ws/dashboard`는 의도대로 1개로 줄었고, 남은 4개는 WS가 아니라 각 페이지가 개별적으로 갖고 있는 `QNetworkAccessManager`(REST 폴링용)의 HTTP keep-alive 커넥션 — 이건 정상 동작이라 별도 조치는 하지 않았다.

---

**교훈**: "재연결할 때 소켓을 안 닫는 버그"를 의심하고 들어갔지만 실제 원인은 재연결 로직이 아니라 **연결을 맺는 지점 자체가 페이지 수만큼 중복**돼 있던 것이었다. lsof에서 본 커넥션 개수가 코드베이스의 어떤 자연수(페이지 수, 매니저 인스턴스 수 등)와 정확히 일치하면, "왜 하나가 안 닫히나"보다 "누가 몇 번 만드나"부터 세어보는 쪽이 더 빨리 원인에 닿는다. 또한 실시간 push 연결(WS)처럼 앱 전체에 공유돼야 하는 리소스는 페이지/위젯 단위가 아니라 최상위 컨테이너(`MainWindow`)가 소유하고 주입하는 편이 안전하다 — 페이지별 소유 패턴은 한 페이지만 보고 있을 때도 다른 페이지가 백그라운드에서 계속 리소스를 잡고 있다는 걸 놓치기 쉽다.
