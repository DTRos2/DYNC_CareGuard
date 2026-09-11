# 2026-09-09 트러블슈팅 — 순찰 연속 녹화·음성 안내 연동

> 작성자: 김예은
> `feature/perception-video-recording` 브랜치에서 "순찰 시작~충전소 복귀"
> 구간을 통으로 녹화하고, 같은 시점에 한국어 음성 안내를 재생하는 기능을
> 붙이는 과정에서 겪은 문제들의 증상·원인·해결을 정리한 기록.
> 대상 파일: `robot_careguard_ws/src/careguard_agent/careguard_agent/downlink_node.py`,
> `robot_careguard_ws/src/careguard_perception/careguard_perception/video_recorder.py`,
> `robot_careguard_ws/src/careguard_hmi/careguard_hmi/audio_player.py`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | `patrol_task`는 잘 도는데 순찰 영상이 하나도 안 찍힘 | `video_recorder` 노드를 아무도 자동으로 안 띄움 — `patrol_task`만 `downlink_node`가 자동 기동해줬음 | `downlink_node._ensure_video_recorder_running()` 추가, `patrol_task`보다 먼저 뜨도록 순서 보장 |
| 2 | 대시보드엔 로봇이 잘 뜨는데 DB로 직접 조회하면 `robot`/`patrol_run`이 0행으로 보임 | 서버가 실제로 쓰는 DB(`server/.env`의 원격 호스트)와, 조회에 쓴 로컬 docker DB가 서로 다른 인스턴스였음 | 서버의 실제 `.env`(`POSTGRES_HOST`)를 먼저 확인하고 그 DB로 조회 |
| 3 | `espeak-ng -v ko "..."` 실행해도 무음, 에러도 없음 | PulseAudio 기본 출력(sink)이 실제 스피커가 안 물려 있는 장치로 잡혀 있었음(USB 오디오는 아날로그가 아니라 S/PDIF 프로파일로 잡혀 있었고, 그마저 물리적으로 스피커 미연결) | `pactl set-default-sink`로 실제 소리 나는 장치(모니터 HDMI)를 기본으로 지정, 재생을 `espeak-ng` 자체 재생이 아니라 `paplay`(PulseAudio 기본 장치를 항상 따라감)로 분리 |
| 4 | `audio_player`를 새로 만든 직후 `downlink_node`가 "10초 안에 구독 안 함" 경고 | 새 패키지(`careguard_hmi`)를 `colcon build`/재source 하기 전에 `downlink_node`를 이미 띄워둔 상태라, 그 프로세스가 물려받은 환경엔 `careguard_hmi`가 없었음 | `colcon build --packages-select careguard_hmi` 후 `downlink_node`가 도는 셸을 다시 source하고 재시작 |
| 5 | 한국어 음성이 "어색하게" 들림(기계음) | `espeak-ng`는 포먼트 합성이라 한국어 받침·연음·종결어미 억양 처리가 약함 | 오프라인 신경망 TTS인 Piper(`piper-tts`)로 교체 |
| 6 | Piper 모델 다운로드 후에도 `ValueError: Unable to find voice: ...` | `.onnx`/`.onnx.json` 다운로드가 실패했거나(리다이렉트 미처리 등) 실제로 그 경로에 파일이 없었음 | `curl -L`로 재다운로드하거나 `python3 -m piper.download_voices`로 공식 다운로더 사용, 파일 크기(수십MB)로 정상 여부 확인 |

---

## 1. video_recorder가 안 떠서 순찰 영상이 안 찍힘

**증상**

`patrol_task.py`가 순찰 시작/종료 시점에 `/careguard/perception/patrol_recording`으로 신호를 잘 보내는데도, 실제로 로봇에서 순찰을 돌려보면 `~/careguard_data/videos`에 아무 파일도 안 생겼다.

**진단 과정**

`ros2 node list`로 확인해보니 `patrol_task`는 떠 있는데 `video_recorder`는 안 떠 있었다. `downlink_node.py`를 훑어보니 `_ensure_patrol_task_running()`이 `patrol_task`는 순찰 시작 시 subprocess로 알아서 띄워주는데, 같은 역할을 하는 `video_recorder` 기동 로직이 아예 없었다.

**원인**

`video_recorder`(careguard_perception)를 띄우는 책임을 가진 코드가 어디에도 없었다 — 사람이 SSH로 직접 세 번째 터미널을 열어 수동으로 띄워야 하는 상태였는데, 이 단계가 안내되지 않아 빠뜨리기 쉬웠다.

**해결**

`downlink_node.py`에 `_ensure_video_recorder_running()`을 `_ensure_patrol_task_running()`과 완전히 같은 패턴(고아 프로세스 정리 → `ros2 run` → 준비 확인)으로 추가하고, `_prepare_navigation`/`_start_patrol` 양쪽에서 **`patrol_task`를 띄우기 전에** 먼저 호출한다. 순서가 중요한데, `patrol_task`가 뜨자마자 `patrol_recording` 토픽에 `start`를 publish하므로 `video_recorder`가 그 전에 구독을 걸어놓지 않으면(늦게 join한 구독자는 이전 메시지를 못 받음) 신호가 조용히 유실되기 때문이다. 준비 확인은 `count_subscribers(토픽) > 0`을 폴링해서 판단한다.

**교훈**

새 노드를 하나 추가할 때, "누가 이 노드를 실제로 기동시키는가"까지 같이 설계하지 않으면 코드는 다 맞는데 아무 일도 안 일어나는 상태가 된다 — `patrol_task`의 자동 기동 패턴이 이미 있었으니 그걸 그대로 재사용하면 되는 문제였다.

---

## 2. 로컬 DB와 서버가 실제로 쓰는 DB가 달라 헷갈림

**증상**

로봇이 실제로 순찰 중이라는데, `docker exec careguard_postgres psql ...`로 `robot`/`patrol_run` 테이블을 조회하면 전부 0행으로 나왔다.

**진단 과정**

`pc_careguard/server/.env`를 확인해보니 `POSTGRES_HOST`가 `localhost`가 아니라 `192.168.230.13`(원격 호스트)이었다 — 서버(`uvicorn`)와 대시보드는 이 원격 DB를 보고 있는데, 방금 조회한 건 이 PC의 로컬 docker 컨테이너(완전히 다른, 비어 있는 인스턴스)였다.

**원인**

`.env.example`의 기본값(`POSTGRES_HOST=localhost`)만 보고 실제 `.env`를 확인 안 한 채 로컬 docker DB로 바로 조회한 게 원인이었다.

**해결**

`server/.env`의 실제 `POSTGRES_HOST`/`POSTGRES_PORT`를 먼저 확인한 뒤 그 DB로 재조회해서 실제 로봇 5대와 `patrol_run` 이력을 확인했다.

**교훈**

여러 DB 인스턴스가 있을 수 있는 환경(로컬 개발용 docker + 실제 서버가 붙는 원격 DB)에서는, 서버 프로세스가 실제로 참조 중인 접속 정보를 먼저 확인하고 나서 DB를 조회해야 한다 — 기본값을 그대로 신뢰하면 안 된다.

---

## 3. espeak-ng 실행해도 무음

**증상**

`espeak-ng -v ko "테스트입니다"`가 에러 없이 끝나는데 아무 소리도 안 났다.

**진단 과정**

`aplay -l`로는 USB 오디오 장치(카드 0)가 잡혀 있는 게 확인됐지만, 이 로봇(Jetson)은 GUI 사운드 설정으로 출력 장치를 고르는 PulseAudio 환경이었다. `pactl list short sinks`로 확인해보니 USB 오디오는 **아날로그(`analog-stereo`)가 아니라 디지털(`iec958-stereo`, S/PDIF) 프로파일**로 잡혀 있었고, GUI에서 출력 테스트를 해보니 실제로 소리가 나는 건 모니터에 연결된 HDMI(`alsa_output.platform-....hda.hdmi-stereo`)뿐이었다 — USB 오디오 장치의 아날로그 출력 잭엔 애초에 스피커가 물리적으로 연결돼 있지 않았다.

**원인**

두 가지가 겹쳤다: (1) 실제로 소리가 나는 물리 스피커가 USB 오디오가 아니라 모니터(HDMI)에 연결돼 있었고, (2) `espeak-ng`가 PulseAudio 기본 출력 장치 설정을 그대로 따라가는지가 불확실했다(빌드에 따라 자체 ALSA 백엔드로 직접 나가버릴 수 있음).

**해결**

- 당장은 `pactl set-default-sink <HDMI sink 이름>`으로 기본 출력을 모니터로 지정
- 코드(`audio_player.py`)는 `espeak-ng`/Piper가 직접 재생하게 두지 않고, **wav 파일로 합성한 뒤 `paplay`로 재생**하도록 분리했다 — `paplay`는 PulseAudio 기본 sink를 항상 그대로 따라가므로, 나중에 실제 USB 스피커로 바꾸더라도(`pactl set-default-sink`만 다시 지정하면) 코드 변경 없이 그대로 동작한다.

**교훈**

GUI로 출력 장치를 고르는 PulseAudio/PipeWire 환경에서는, 커맨드라인 도구가 그 설정을 그대로 따라간다고 가정하면 안 된다 — 재생 자체를 PulseAudio 클라이언트(`paplay`)로 분리해두면 시스템 기본 출력 설정과 항상 일치한다.

---

## 4. 새 패키지(careguard_hmi) 추가 직후 구독 타임아웃

**증상**

`careguard_hmi`/`audio_player.py`를 새로 만들고 나서 순찰을 돌리니 `downlink_node`가 `10초 안에 audio_player가 안내 토픽을 구독하지 않았습니다` 경고를 냈다.

**원인**

새 패키지를 만든 직후 `colcon build`를 안 했거나(또는 했어도), `downlink_node`를 이미 띄워놓은 터미널의 환경(`AMENT_PREFIX_PATH` 등)은 빌드 전 상태를 그대로 물려받고 있어서 `ros2 run careguard_hmi audio_player`가 실패했다 — `subprocess.Popen`으로 띄우는 자식 프로세스는 부모(`downlink_node`)의 환경을 그대로 상속하기 때문이다.

**해결**

`colcon build --packages-select careguard_hmi` 실행 후, `downlink_node`(그리고 `uplink_node`)를 새로 source한 터미널에서 재시작.

**교훈**

ROS2 워크스페이스에 새 패키지를 추가했을 때, 그 패키지를 자동으로 띄워주는 다른 노드가 이미 실행 중이라면 그 노드도 재시작해야 새 패키지를 찾을 수 있다 — 빌드만 하고 기존 프로세스를 안 건드리면 "빌드는 성공했는데 왜 안 되지"가 된다.

---

## 5. espeak-ng 한국어 음질이 어색함 → Piper TTS로 교체

**증상**

`espeak-ng`로 재생되는 한국어 안내가 실제로 들어보니 "어색하다"는 피드백을 받았다 — 음절 단위로 뚝뚝 끊기고 종결어미("-습니다") 억양이 부자연스러움.

**원인**

`espeak-ng`는 포먼트 합성(규칙 기반 파형 생성) 엔진이라 사람 목소리에 가까운 운율을 만들지 못한다. 한국어는 받침·연음 처리가 복잡한데 `espeak-ng`의 한국어 음소 규칙이 단순해서 다른 언어보다 더 두드러지게 어색하게 들린다.

**해결**

오프라인 신경망 TTS인 **Piper**(`piper-tts`, ONNX 기반, 경량이라 Jetson에서도 빠르게 동작)로 교체했다. `audio_player.py`의 합성 부분(`_speak()`)만 `espeak-ng` 호출에서 `piper --model ... --output_file ...` 호출로 바꿨고, 그 뒤 `paplay` 재생 경로는 그대로 재사용했다 — 토픽 인터페이스(`/careguard/voice/announce`)가 그대로라 `patrol_task.py` 등 발행 쪽은 전혀 안 건드렸다.

**교훈**

합성 엔진 선택이 사용자 체감 품질에 큰 영향을 준다 — 처음부터 "안내가 나온다"는 걸 검증하는 데는 설치가 가장 쉬운 `espeak-ng`로 시작했지만, 실제 사용 품질까지 고려하면 오프라인이면서도 신경망 기반인 Piper 같은 대안으로 넘어가는 게 맞았다. `_speak()` 하나로 엔진을 캡슐화해둔 덕분에 교체 비용이 작았다.

---

## 6. Piper 음성 모델 "Unable to find voice"

**증상**

Piper 설치 후 `echo "..." | piper --model ~/piper-voices/ko_KR-kss-medium.onnx --output_file /tmp/test.wav`를 실행하니 `ValueError: Unable to find voice: ... (use piper.download_voices)` 에러가 났다.

**원인**

지정한 경로에 실제로 `.onnx` 파일이 없었다 — `wget`/`curl`로 받는 과정에서 리다이렉트를 못 따라가 실패했거나(작은 HTML 에러 페이지만 저장됨), 애초에 다운로드가 실행되지 않았다.

**해결**

`curl -L`(리다이렉트 추적)로 재다운로드하거나, Piper가 에러 메시지에서 직접 안내한 `python3 -m piper.download_voices ko_KR-kss-medium` 공식 다운로더를 사용했다. 다운로드 후 `ls -la`로 파일 크기가 실제 모델 크기(수십MB)인지 확인하는 과정을 거쳤다.

**교훈**

큰 바이너리 파일을 `wget`/`curl`로 받을 때는 항상 파일 크기로 다운로드 성공 여부를 확인해야 한다 — HTTP 200이 와도 리다이렉트 페이지나 에러 페이지를 그대로 저장했을 수 있다.
