/*
 * careguard_hw/careguard_hw/firmware/call_button/call_button.ino
 *
 * 침실 등 고정 위치의 응급 호출 버튼(ESP32). db/schema/careguard_schema_v1.sql의
 * call_button 테이블 코멘트가 이미 정해둔 계약을 그대로 구현한다:
 *   - 버튼이 눌리면 MQTT 토픽 cg/v1/button/{BUTTON_CODE}/press 로 발행한다
 *   - {BUTTON_CODE}는 서버 DB의 call_button.code와 정확히 일치해야 한다
 *     (예: "BTN_BEDROOM") — 등록 안 된 코드면 서버가 조용히 무시한다
 *     (pc_careguard/server/app/services/mqtt_listener.py::_handle_press 참고)
 *   - payload 내용은 서버가 안 본다(토픽의 코드만으로 판단) — 빈 문자열로 보낸다
 *
 * 배선: 버튼을 BUTTON_PIN과 GND 사이에 연결, INPUT_PULLUP 사용 — 평소 HIGH,
 * 누르면 LOW로 떨어진다. 별도 저항 없이 ESP32 내장 풀업만으로 동작한다.
 *
 * 디바운싱: 기계식 버튼은 눌리는/떼는 순간 수 ms 동안 신호가 여러 번 튄다
 * (채터링) — DEBOUNCE_MS 동안 상태가 안정적으로 유지된 경우에만 "눌림"으로
 * 확정하고, 눌려 있는 동안 반복 발행하지 않는다(뗐다가 다시 눌러야 재발행).
 *
 * 라이브러리: PubSubClient (Nick O'Leary) — Arduino 라이브러리 매니저에서 설치.
 *
 * 빌드/실행 미검증 — 실제 ESP32 보드로 아직 플래시해서 확인 못 함. 업로드 후
 * 시리얼 모니터(115200bps)로 WiFi/MQTT 연결 로그부터 확인할 것.
 */
#include <WiFi.h>
#include <PubSubClient.h>
#include <string.h>  // strlen() — WIFI_PASSWORD가 비어있는지(open network) 확인용

// ---- 배포 시 이 세 값만 기기별로 바꾸면 된다 ----
// ESP32는 2.4GHz만 지원한다(5GHz 불가, 하드웨어 한계) — 공유기가 대역별로 SSID를
// 분리해뒀다면(예: DYNC_5G/DYNC_2.4G) 반드시 2.4GHz 쪽 이름을 넣어야 한다.
const char* WIFI_SSID = "DYNC_2.4G";
// 암호화 없는(open) 네트워크라 비밀번호가 없다 — 빈 문자열로 두고, connectWifi()에서
// WiFi.begin(ssid) 한 인자짜리 오버로드로 연결한다(암호 있는 네트워크로 바뀌면 여기
// 채우고 connectWifi()도 WiFi.begin(WIFI_SSID, WIFI_PASSWORD)로 되돌린다).
const char* WIFI_PASSWORD = "";
// 서버 DB의 call_button.code와 정확히 일치해야 한다.
const char* BUTTON_CODE = "BTN_BEDROOM";

// MQTT 브로커 주소. 지금은 로컬 테스트용 PC(mosquitto, deploy/docker-compose.yml)의
// 주소를 기본값으로 넣어뒀다 — 이 PC와 ESP32가 같은 와이파이에 붙어있어야 한다.
// 실제 배포 시엔 운영 서버의 브로커 주소로 바꾼다.
const char* MQTT_HOST = "192.168.230.27";
const int MQTT_PORT = 1883;

const int BUTTON_PIN = 4;  // 배선에 맞춰 조정
const unsigned long DEBOUNCE_MS = 50;
const unsigned long RECONNECT_INTERVAL_MS = 3000;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

bool lastRawState = HIGH;       // 직전에 읽은 raw 값(디바운스 전)
bool stableState = HIGH;        // 디바운스를 통과한 확정 상태 — 이게 바뀌는 순간에만 처리하므로
                                 // 눌려 있는 동안 반복 발행되지 않는다(뗐다 다시 눌러야 재발행)
unsigned long lastChangeAt = 0;
unsigned long lastReconnectAttempt = 0;

void connectWifi() {
  // 이미 연결돼 있으면 즉시 반환하므로 평소엔 이 아래 블로킹 대기까지 안 간다.
  // 실제로 와이파이가 끊긴 순간엔 재연결될 때까지 loop() 전체가 멈춘다(버튼
  // 입력도 그동안 못 읽음) — 이 기기가 버튼 하나만 처리하는 단일 목적 장치라
  // 감내하기로 한 의도적 단순화다(비동기 재연결까지 만들 이유가 없음).
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("WiFi 연결 시도: %s\n", WIFI_SSID);
  if (strlen(WIFI_PASSWORD) == 0) {
    WiFi.begin(WIFI_SSID);  // open network — 비밀번호 인자를 아예 안 넘긴다
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi 연결됨, IP=%s\n", WiFi.localIP().toString().c_str());
}

bool connectMqtt() {
  // 재사용 가능한 client id가 필요해 BUTTON_CODE에 접두어만 붙인다 — 브로커가
  // 같은 client id의 중복 접속을 이전 접속 종료로 처리하므로, 기기 재부팅 시
  // "다른 접속이 이미 있음" 문제가 안 생긴다.
  String clientId = String("careguard-button-") + BUTTON_CODE;
  Serial.print("MQTT 브로커 연결 시도... ");
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("연결됨");
    return true;
  }
  Serial.printf("실패 (rc=%d)\n", mqttClient.state());
  return false;
}

void publishPress() {
  String topic = String("cg/v1/button/") + BUTTON_CODE + "/press";
  bool ok = mqttClient.publish(topic.c_str(), "");
  Serial.printf("발행 %s: %s\n", ok ? "성공" : "실패", topic.c_str());
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  connectWifi();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
}

void loop() {
  connectWifi();  // 와이파이가 끊겼으면(WL_CONNECTED 아니면) 다시 시도

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
      lastReconnectAttempt = now;
      connectMqtt();
    }
  } else {
    mqttClient.loop();
  }

  // ---- 버튼 읽기 + 디바운스 ----
  bool raw = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (raw != lastRawState) {
    lastChangeAt = now;  // 값이 바뀐 순간을 기록 — 채터링 구간의 시작점
    lastRawState = raw;
  }

  if ((now - lastChangeAt) >= DEBOUNCE_MS && stableState != raw) {
    // DEBOUNCE_MS 동안 값이 안 흔들렸으니 진짜로 상태가 바뀐 것으로 확정한다.
    stableState = raw;
    if (stableState == LOW) {
      // 눌림 확정. 연결돼 있을 때만 발행 — 끊긴 상태면 이번 눌림은 유실된다
      // (재시도 큐잉은 이번 스코프 밖. TODO: 필요하면 오프라인 시 로컬에 쌓아뒀다
      // 재연결 시 한꺼번에 보내는 방식 고려).
      if (mqttClient.connected()) {
        publishPress();
      } else {
        Serial.println("MQTT 미연결 상태라 이번 눌림은 발행하지 못했습니다");
      }
    }
    // stableState == HIGH(뗌 확정)는 별도 처리 없음 — 다음에 LOW로 바뀔 때
    // 위 분기가 다시 눌림으로 잡아준다.
  }
}
