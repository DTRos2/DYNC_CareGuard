# 2026-09-14 트러블슈팅 — SOS 호출 버튼 눌림 1회에 care_event 2건씩 중복 생성

> 작성자: 도희정
> `origin/develop`을 pull하면서(`mqtt_listener.py`에 브로커 연결 실패를 조용히 삼키는 방어 코드가 새로 들어온 시점) ESP32 SOS 버튼을 눌러 동작을 확인하던 중 발견. 처음엔 "mqtt_listener 쪽에서 못 받는다"는 반대 증상으로 보고했는데, 실제로는 못 받는 게 아니라 서버가 2개 떠서 각자 받고 있었다.
> 대상 파일: `pc_careguard/server/app/services/mqtt_listener.py`, `pc_careguard/dashboard/src/main.cpp`, `robot_careguard_ws/systemd/careguard-uplink-r07.service`(및 downlink), `pc_careguard/server/.env`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | SOS 버튼 1회 눌림마다 `care_event`(SOS_BUTTON)가 항상 정확히 2건씩 생성, 같은 로봇에 출동 명령도 중복 전송 | 같은 공유 MQTT 브로커(`cg/v1/button/+/press`)에 서버 인스턴스가 동시에 2개 구독 — .7(팀원 PC, systemd, 로봇이 실제로 보는 canonical 서버) + .13(mosquitto/postgres를 호스팅하는 PC, 수동 `nohup uvicorn`으로 띄운 leftover 인스턴스) | .13의 leftover uvicorn 프로세스를 종료. 서버는 로봇 설정과 일치하는 .7 하나만 유지 |

---

## 1. SOS 버튼 1회 눌림 → care_event 2건

**증상**

`origin/develop`을 pull한 뒤 ESP32 SOS 버튼으로 동작 확인을 하던 중, DB에서 `care_event`(event_type='SOS_BUTTON')를 조회하면 매번 같은 초(때로는 20~30ms 차이) 안에 2행씩 INSERT되어 있었다:

```
id  | event_uid                | robot_id | occurred_at
140 | EV-R-07-260914-ff68c2b8  | 328      | 15:42:40.693315
139 | EV-R-07-260914-b9d53faf  | 328      | 15:42:40.658706
```

전부 같은 로봇(robot_id=328, R-07)이었다.

**진단 과정**

1. `mqtt_listener.py`(`_handle_press`) 코드 자체를 봐도 메시지 하나당 INSERT 한 번, `start_mqtt_listener()`도 앱 기동 시 한 번만 호출 — **이 서버 프로세스 하나만 놓고 보면 중복 로직이 없다.**
2. mosquitto(공유 브로커, .13에 docker로 호스팅)의 컨테이너 로그(`docker logs careguard_mosquitto`)를 시간순으로 훑어 실제 접속 클라이언트를 확인:
   - `auto-10CBF94C...` — .13 자신의 `nohup uvicorn`(수동 기동, systemd 아님)이 붙인 것. 서버 프로세스 시작 시각과 거의 일치.
   - `auto-346C8CEF...` — **192.168.230.7**에서 접속, 이후 disconnect 로그 없이 계속 연결 유지.
   - 그 외 192.168.230.27도 몇 차례 붙었다 끊겼다 했으나 이후 연결 기록 없음(테스트성 재시작으로 추정, 원인 아님).
3. `care_event` 중복 발생 시각 구간과 위 두 클라이언트가 동시에 연결돼 있던 구간이 정확히 겹침 — **MQTT pub/sub 특성상 같은 토픽을 구독하는 인스턴스가 여러 개면 각자 독립적으로 메시지를 받아 각자 INSERT**하기 때문에 정확히 2배로 나타난 것.
4. 어느 쪽을 꺼야 하는지 확인하기 위해 로봇(R-07)의 systemd 유닛(`robot_careguard_ws/systemd/careguard-uplink-r07.service`)을 읽어보니 `CAREGUARD_SERVER_URL=http://192.168.230.7:8000`으로 하드코딩 — **로봇 자체가 물리적으로 .7만 보도록 되어 있어 .7은 끄면 안 됨**. 따라서 꺼야 할 쪽은 .13의 leftover.
5. 혹시 이 PC(.13)의 Qt 대시보드(`careguard_dashboard`)가 로컬 서버를 자동으로 하나 더 띄운 게 아닌지도 의심했으나, `main.cpp::startBackendServer()`(커밋 `dcc108a`에서 `CAREGUARD_SERVER_URL` 기준으로 remote reachable이면 자동 기동을 건너뛰도록 이미 고쳐져 있음) 로직과 `ps -ef --forest`로 확인한 프로세스 트리 모두 "대시보드가 uvicorn을 자식 프로세스로 띄운 적 없음"을 확인 — 원인 아님.
6. 192.168.230.27도 `ps aux | grep uvicorn` 결과 대시보드 GUI 프로세스만 있고 서버 프로세스는 없음을 확인 — 원인 아님. 다만 `pc_careguard/dashboard/.env`는 `CAREGUARD_SERVER_URL=http://192.168.230.7:8000`으로 정확히 잡혀 있었음(로봇과 일치).

**원인**

DB(postgres)·MQTT 브로커(mosquitto)가 물리적으로 .13 PC에 떠 있다 보니, 개발 중 .13에서도 손쉽게 `MQTT_HOST=localhost`로 서버를 수동 기동해볼 수 있었다. 이 leftover 프로세스가 정리되지 않은 채 로봇의 canonical 서버(.7, systemd)와 동시에 같은 브로커를 구독하면서, 버튼 눌림 하나가 두 서버 인스턴스 수만큼 `care_event`로 중복 삽입됐다. `main.cpp`의 `dcc108a` 픽스(대시보드가 remote reachable이면 로컬 서버 자동 기동을 건너뛰는 것)는 "대시보드가 실수로 로컬 서버를 띄우는" 경로만 막을 뿐, 사람이 터미널에서 직접 `uvicorn`을 띄우는 경로는 막지 못한다.

**해결**

- .13에서 수동으로 떠 있던 uvicorn 프로세스를 종료(`kill`). 서버는 로봇 설정과 일치하는 .7의 systemd 서비스(`careguard-server.service`) 하나만 유지.
- 코드 변경 없음 — dedup 로직이나 MQTT shared subscription 같은 방어 코드는 "여러 서버가 상시 동시에 떠 있어야 하는" 아키텍처를 전제로 한 것인데, 지금은 canonical 서버가 하나여야 하는 상황이라 그런 복잡도는 추가하지 않기로 함.

**검증**

.13의 leftover 프로세스 종료 후 mosquitto 로그에서 해당 클라이언트(`auto-D1E52362...`)의 정상 disconnect 확인. 이후 실제로 ESP32 SOS 버튼을 눌러 `care_event`가 정확히 1건만 생성되는 것을 확인 — **해결 완료**.

---

**교훈**: DB/브로커 같은 공유 인프라를 특정 팀원 PC가 호스팅하는 구조에서는, 그 PC에서 "로컬 테스트 삼아" 서버를 띄우기가 너무 쉬워서 leftover 프로세스가 생기기 쉽다. 대시보드의 자동 기동 로직만 고쳐서는(즉 "실수로 자동으로 뜨는 것"만 막아서는) 사람이 수동으로 띄운 프로세스는 못 막는다는 걸 이번에 확인했다. 또한 증상을 처음 보고할 때 "mqtt_listener가 못 받는다"고 표현했지만 실제로는 정반대(여러 곳에서 받고 있었음)였다 — MQTT pub/sub 구조에서는 "안 받는다"는 증상을 보고받아도 "받는 곳이 여러 개인 건 아닌지"를 먼저 의심해볼 가치가 있다.
