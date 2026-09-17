#include <Servo.h>

// R-06 약통 펌웨어 — R-07(careguard_hw/firmware/pillbox_firmware)과 달리 서보 칸이
// 1개뿐이라 A/B 채널 구분 없이 단일 라인으로 정리했다. IR 5번 / 서보 4번.
const int IR_PIN = 5;
Servo servo_1;
int currentAngle = 0;

const int ANGLE_OPEN = 150;   // 열림 각도
const int ANGLE_CLOSED = 0;   // 닫힘 각도
const unsigned long CLOSE_DELAY = 5000;  // 감지 후 닫히기까지 대기 시간(ms)

// 기본값 = "닫힘"(isClosed=true). 전원 켜진 직후/평상시엔 약통이 잠겨 있고, PC가
// "OPEN"을 보낼 때만(=로봇이 순찰 중 사람을 인식했을 때) 열린다.
unsigned long detectedSince = 0;
bool waitingToClose = false;
bool isClosed = true;

// ---------- PC(pillbox_controller 상당) 시리얼 명령 ----------
// 사람을 인식하기 전까진 절대 안 열린다 — "OPEN\n" 명령이 유일한 개방 트리거다.
// 사람이 없는데 IR 노이즈만으로 저절로 열리는 경로는 없다.
//
// "CLOSE\n": 정상적으로는 IR 감지 CLOSE_DELAY 후 이 펌웨어가 스스로 닫지만(아래
// loop() 로직), 회수 대기 시간 안에 IR 감지를 못 받으면 "약통이 열린 채로 방치"되는
// 걸 막기 위해 PC가 강제로 닫으라고 보낸다. IR 대기 중이던 상태(waitingToClose)도
// 같이 취소한다 — 이미 강제로 닫혔는데 그 뒤 잠깐 IR이 감지돼도 또 "CLOSED" 알림을
// 중복으로 보내면 안 되기 때문이다.
//
// 응답: 명령을 처리하면 그 즉시 "OK\n"을 돌려준다(OPEN/CLOSE 공용). 잘못된 라인
// 문자면 무시하고 응답 없음 — PC 쪽이 타임아웃으로 실패 처리한다.
//
// "닫힘 완료" 알림: 열려있던 칸이 IR 감지 CLOSE_DELAY 후 실제로 닫히는 순간
// "CLOSED\n"을 알림으로 보낸다. IR로 정상 회수된 경우엔 "감지 후 닫힘"과 "회수
// 확인"이 사실상 같은 순간이라 이 알림 한 줄이 배출 완료 신호를 겸한다. CLOSE
// 명령으로 강제로 닫힌 경우(회수를 못 받은 jammed)엔 IR이 안 눌린 채로 닫히는
// 것이므로 이 "CLOSED" 알림을 안 보낸다. 한 번 닫힌 뒤에는 다음 "OPEN" 명령이
// 올 때까지 저절로 다시 열리지 않는다.
void handleSerialCommand() {
  if (!Serial.available()) {
    return;
  }
  String line = Serial.readStringUntil('\n');
  line.trim();

  if (line == "OPEN") {
    waitingToClose = false;
    isClosed = false;
    moveServoTo(servo_1, currentAngle, ANGLE_OPEN);
    Serial.println("OK");
  } else if (line == "CLOSE") {
    waitingToClose = false;  // IR 대기 중이었더라도 취소 — 아래서 이미 닫으므로
    isClosed = true;
    moveServoTo(servo_1, currentAngle, ANGLE_CLOSED);
    Serial.println("OK");
  }
  // 그 외 문자열은 조용히 무시한다 — 잘못된 줄바꿈/노이즈로 매번 에러를 찍으면
  // 정상 디버그 출력(IR 상태 줄)까지 알아보기 힘들어진다.
}

void setup() {
  Serial.begin(9600);

  pinMode(IR_PIN, INPUT);

  servo_1.attach(4);

  // 기본값 = 닫힘 — 전원이 켜지는 순간 약통은 잠겨 있어야 한다(사람을 인식하기
  // 전까진 절대 열리면 안 됨). isClosed 선언부의 기본값(true)과 짝을 맞춘다.
  currentAngle = ANGLE_CLOSED;
  servo_1.write(currentAngle);
}

void moveServoTo(Servo &servo, int &currentAngleRef, int targetAngle) {
  if (currentAngleRef < targetAngle) {
    for (int angle = currentAngleRef; angle <= targetAngle; angle++) {
      servo.write(angle);
      delay(30);
    }
  } else if (currentAngleRef > targetAngle) {
    for (int angle = currentAngleRef; angle >= targetAngle; angle--) {
      servo.write(angle);
      delay(30);
    }
  }
  currentAngleRef = targetAngle;
}

void loop() {
  handleSerialCommand();

  int sensor = digitalRead(IR_PIN);
  bool detected = (sensor == LOW);

  Serial.print("IR(5번): ");
  Serial.println(detected ? "감지" : "없음");

  if (detected && !isClosed && !waitingToClose) {
    waitingToClose = true;
    detectedSince = millis();
  }
  if (waitingToClose && (millis() - detectedSince >= CLOSE_DELAY)) {
    moveServoTo(servo_1, currentAngle, ANGLE_CLOSED);
    isClosed = true;
    waitingToClose = false;
    Serial.println("CLOSED");  // PC가 회수 확인(ir_triggered)으로 relay
  }
  // "IR이 없어지면 다시 연다"는 로직은 없다 — "열림"은 오직 OPEN 명령으로만
  // 시작돼야 하므로, 닫힌 뒤 IR이 사라졌다고 저절로 다시 열리면 안 된다. 다음
  // 사람 인식 때 PC가 다시 OPEN을 보낸다.

  delay(100);
}
