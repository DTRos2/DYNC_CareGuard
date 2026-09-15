# 2026-09-15 트러블슈팅 — R-07 음성 안내(TTS)가 계속 안 나오던 문제

> 작성자: cuhote
> SOS 호출 버튼으로 로봇 출동·녹화까지는 되는데 음성 안내만 계속 안 나서, R-07(LIMO)에 직접 SSH로 들어가 파이프라인을 단계별로(피어 합성 → 재생) 분리해서 확인한 기록. 같은 날 겪은 관련 문제(record_trigger 발행 타이밍, PulseAudio 기본 출력 드리프트)도 함께 정리한다.
> 대상 파일: `robot_careguard_ws/systemd/careguard-downlink-r07.service`, `robot_careguard_ws/src/careguard_perception/careguard_perception/video_recorder.py`
> 대상 로봇: R-07(LIMO, wego@192.168.230.42)

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | `piper 합성 실패: ModuleNotFoundError: No module named 'onnxruntime'`가 계속 반복됨. `pip install --user onnxruntime`을 여러 번 실행해도 "already satisfied"만 뜸 | 셸의 `PYTHONPATH`에 `/home/wego/miniconda3/envs/dl_env/lib/python3.10/site-packages`가 이미 박혀있어서, `pip`가 conda 환경의 onnxruntime을 "이미 있다"고 착각하고 `piper`가 실제로 쓰는 시스템 파이썬(`~/.local`) 쪽엔 설치를 건너뜀 | `env -u PYTHONPATH /usr/bin/python3 -m pip install --user --force-reinstall --no-deps onnxruntime`로 `PYTHONPATH` 무시하고 강제 재설치 |
| 2 | onnxruntime 설치 후에도 소리가 안 남 — 로그에 `paplay 재생 실패(rc=1): Connection failure: Connection refused` | `careguard-downlink-r07.service`(systemd)가 `wego` 사용자의 PulseAudio 세션과 분리된 환경에서 실행돼, `paplay`가 세션 소켓(`XDG_RUNTIME_DIR`)을 못 찾음 — 합성(piper)까지는 성공하고 재생 단계에서만 조용히 실패 | 서비스 파일에 `Environment=XDG_RUNTIME_DIR=/run/user/1000` 추가 후 `daemon-reload` + 재시작 |
| 3 | 위 두 가지를 다 고쳐도, 재시작 직후 첫 SOS 출동에서만 가끔 녹화는 되는데 음성만 또 안 나옴 | `video_recorder`가 막 재시작된 직후라, 이 노드의 `/careguard/voice/announce` 발행자가 `audio_player`의 구독자한테 아직 발견(DDS discovery)되지 않은 채로 publish가 나가 유실됨 — `patrol_recording` 쪽에서 이미 겪었던 late-joiner 문제의 반대 방향 | `video_recorder.py::_announce_severity()`가 발행 전 최대 2초간 `count_subscribers(VOICE_TOPIC) > 0`을 기다리도록 수정(`501961c`) |
| 4 | `pactl set-default-sink`로 HDMI를 기본 출력으로 맞춰도 시간이 지나면(모니터 절전, 다른 장치 연결 등) 슬쩍 다른 장치로 바뀌어 다시 소리가 안 남 | 순정 PulseAudio의 `module-switch-on-port-available`이 장치 연결/해제가 있을 때마다 기본 출력을 자동으로 바꿈 — `pactl set-default-sink`는 그 순간만 유효하고 이 모듈의 자동 전환을 막지 못함 | `~/.config/pulse/default.pa`에 시스템 설정을 복사해 그 모듈을 주석 처리하고 `set-default-sink`를 고정값으로 추가, `pulseaudio -k`로 재시작 |

---

## 1. `onnxruntime` 미설치 — 사실은 `PYTHONPATH`가 conda로 새고 있었다

### 증상

```
piper 합성 실패(rc=1): Traceback (most recent call last):
  File "/home/wego/.local/bin/piper", line 3, in <module>
    from piper.__main__ import main
  File "/home/wego/.local/lib/python3.10/site-packages/piper/voice.py", line 15, in <module>
    import onnxruntime
ModuleNotFoundError: No module named 'onnxruntime'
```

`pip install --user onnxruntime`, `/usr/bin/python3 -m pip install --user onnxruntime` 둘 다 시도했는데 매번:

```
Requirement already satisfied: onnxruntime in ./miniconda3/envs/dl_env/lib/python3.10/site-packages (1.22.1)
```

### 진단 과정

- `piper` 스크립트 자체는 `/home/wego/.local/bin/piper`, 참조하는 라이브러리도 `/home/wego/.local/lib/python3.10/site-packages/piper/` — 시스템 파이썬(`/usr/bin/python3`) 기준 설치임을 확인.
- `/usr/bin/python3 -c "import sys; print(sys.executable)"` → `/usr/bin/python3`로 정확히 나옴에도 여전히 conda 경로를 "이미 있다"고 봄 — 실행 파일이 아니라 **`sys.path` 자체가 오염**된 것으로 의심.
- `echo $PYTHONPATH` 확인 결과, ROS 워크스페이스 소싱 과정에서 딸려 들어온 긴 경로 목록 중간에 `/home/wego/miniconda3/envs/dl_env/lib/python3.10/site-packages`가 명시적으로 포함돼 있었음.

### 원인

`pip install`은 대상 인터프리터가 무엇이든 상관없이, **현재 프로세스의 `sys.path`에서 이미 import 가능하면 설치를 건너뛴다.** `PYTHONPATH`가 `sys.path`에 합쳐지므로, `/usr/bin/python3`로 pip를 실행해도 `PYTHONPATH`에 껴 있는 conda 경로의 onnxruntime이 "이미 만족됨"으로 잡혀 `~/.local`에는 끝까지 설치되지 않았다.

### 해결

```bash
env -u PYTHONPATH /usr/bin/python3 -m pip install --user --force-reinstall --no-deps onnxruntime
```

- `env -u PYTHONPATH`: 이 명령 실행 중에만 환경변수를 제거(전역 `PYTHONPATH`는 ROS 워크스페이스에 필요하므로 건드리지 않음)
- `--force-reinstall --no-deps`: "이미 있다"고 오판해 건너뛰지 않도록 강제

### 검증

```bash
env -u PYTHONPATH /usr/bin/python3 -c "import onnxruntime; print(onnxruntime.__file__)"
# /home/wego/.local/lib/python3.10/site-packages/onnxruntime/__init__.py
```

GPU 탐지 실패 경고(`device_discovery.cc: Failed to open ".../vendor"`)는 CPU로 폴백하는 정상 동작이라 무시해도 된다.

---

## 2. 합성은 되는데 재생만 실패 — systemd 서비스가 PulseAudio 세션을 못 찾음

### 증상

onnxruntime 설치 후에도 소리가 안 남. 로그:

```
[audio_player]: 음성 안내: 음성 테스트입니다
[audio_player]: paplay 재생 실패(rc=1): Connection failure: Connection refused
```

같은 `paplay <file>`을 로그인 셸에서 수동으로 실행하면 정상적으로 소리가 남 — "왜 수동으로는 되는데 서비스로는 안 되냐"는 지점에서 원인이 좁혀짐.

### 원인

`careguard-downlink-r07.service`가 `audio_player`(careguard_hmi)를 자식 프로세스로 띄우는데, systemd 서비스는 로그인 세션과 별도 환경에서 실행돼 PulseAudio 세션 소켓 위치(`$XDG_RUNTIME_DIR/pulse/native`)를 알려주는 `XDG_RUNTIME_DIR`이 비어 있다. `piper`(합성)는 이 값과 무관해 성공하지만, `paplay`(재생)는 이 소켓에 접속해야 해서 `Connection refused`로 실패한다.

### 해결

`robot_careguard_ws/systemd/careguard-downlink-r07.service`에 한 줄 추가(`id -u wego` = 1000):

```ini
Environment=XDG_RUNTIME_DIR=/run/user/1000
```

```bash
sudo systemctl daemon-reload
sudo systemctl restart careguard-downlink-r07.service
```

### 검증

```bash
ros2 topic pub --once /careguard/voice/announce std_msgs/msg/String "data: '{\"text\": \"음성 테스트입니다\"}'"
```
실행 시 로그에 `paplay 재생 실패` 없이 실제 스피커로 음성 재생 확인.

---

## 3. 재시작 직후 첫 SOS 출동에서만 가끔 음성 유실 — DDS late-joiner (반대 방향)

### 증상

1, 2번을 다 고친 뒤에도, `careguard-downlink-r07.service`를 재시작한 직후 곧바로 SOS 버튼을 누르면 녹화는 되는데 음성만 안 나오는 경우가 있었음(재현이 매번 되진 않음 — 몇 초 뒤에 다시 누르면 정상).

### 원인

`downlink_node.py`가 `video_recorder`를 새로 띄운 직후(`_ensure_video_recorder_running`) 곧바로 `record_trigger`를 보내면, `video_recorder` 안의 `_voice_pub`(→ `/careguard/voice/announce`)가 아직 `audio_player`의 구독자와 DDS 매칭이 끝나기 전에 `publish()`가 실행돼 메시지가 조용히 유실된다. `patrol_recording` 토픽에서 이미 겪었던(그래서 `_ensure_video_recorder_running`이 구독자 수를 확인하며 대기하는) 것과 같은 종류의 문제가, 이번엔 `video_recorder`가 "늦게 join하는 발행자" 쪽이 되어 반대 방향으로 발생한 것.

### 해결

`careguard_perception/video_recorder.py::_announce_severity()`가 발행 전에 최대 2초(`VOICE_SUBSCRIBER_WAIT_TIMEOUT_SEC`) 동안 `count_subscribers(config.VOICE_TOPIC) > 0`을 폴링하도록 수정(커밋 `501961c`). `audio_player` 자체가 안 떠 있는 경우까지 순찰/녹화를 막을 이유는 없어, 타임아웃 후에는 그냥 발행하고 넘어간다.

---

## 4. HDMI를 기본 출력으로 맞춰도 시간 지나면 다시 바뀜

### 증상

`pactl set-default-sink alsa_output.platform-3510000.hda.hdmi-stereo`로 맞춰도, 얼마 뒤 다시 조회하면 다른 장치(analog-stereo, USB 오디오 등)로 기본값이 바뀌어 있어 음성이 다시 안 남.

### 원인

`pactl info | grep "Server Name"` → `pulseaudio`(PipeWire 아닌 순정 PulseAudio) 확인. 순정 PulseAudio는 기본적으로 `module-switch-on-port-available` 모듈을 로드하는데, 이 모듈이 "장치가 연결되거나 우선순위가 바뀌면 기본 출력을 자동으로 전환"한다. `pactl set-default-sink`는 그 순간의 값만 바꿀 뿐, 이 자동 전환 자체를 막지 못해 다음 이벤트(장치 연결/해제, 모니터 절전 등)가 오면 도로 바뀐다.

### 해결

시스템 기본 설정을 사용자 설정으로 복사한 뒤, 그 모듈만 비활성화하고 기본 싱크를 고정값으로 추가한다(처음부터 새로 작성하면 다른 필수 설정이 빠져 오디오 전체가 깨지므로 반드시 복사 후 수정):

```bash
mkdir -p ~/.config/pulse
cp /etc/pulse/default.pa ~/.config/pulse/default.pa
sed -i 's/^load-module module-switch-on-port-available/#&/' ~/.config/pulse/default.pa
echo "set-default-sink alsa_output.platform-3510000.hda.hdmi-stereo" >> ~/.config/pulse/default.pa
pulseaudio -k
```

### 검증

```bash
pactl get-default-sink
# alsa_output.platform-3510000.hda.hdmi-stereo
```
다른 USB 오디오 장치를 꽂았다 빼도 기본값이 안 바뀌는 것 확인.

---

## 교훈

- `pip install`은 "설치 대상 인터프리터가 뭔지"가 아니라 "지금 프로세스의 `sys.path`에 이미 있는지"로 건너뛸지를 판단한다 — `PYTHONPATH`가 길게 누적되는 ROS 워크스페이스 환경에서는 `which pip`/`sys.executable`만 확인하고 안심하면 안 되고, `echo $PYTHONPATH`까지 봐야 한다.
- systemd 서비스가 GUI 세션의 자원(PulseAudio, X11 등)을 쓰려면 `XDG_RUNTIME_DIR` 같은 세션 환경변수를 명시적으로 넘겨줘야 한다 — 합성/렌더링 같은 "순수 계산" 단계는 성공하고 "출력" 단계에서만 조용히 실패하는 패턴이 이 문제의 전형적인 증상이다.
- DDS(ROS2) late-joiner 문제는 "구독자가 늦게 뜨는 경우"만 있는 게 아니라, "발행자가 늦게 뜨는 경우"도 똑같이 유실을 일으킨다 — 이 프로젝트는 앞의 경우(`patrol_recording`)를 이미 방어하고 있었는데, 뒤의 경우(`voice/announce`)는 이번에 처음 발견됐다. 새 토픽을 추가할 때마다 "누가 늦게 뜰 수 있는지" 양쪽 다 점검할 것.
