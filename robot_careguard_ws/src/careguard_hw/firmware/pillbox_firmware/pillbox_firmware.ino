#include <Servo.h>

// ---------- 1번 라인: IR 9번 - 서보 3번 ----------
const int IR_PIN_A = 9;
Servo servo_A;
int currentAngle_A = 0;

// ---------- 2번 라인: IR 8번 - 서보 4번 ----------
const int IR_PIN_B = 8;
Servo servo_B;
int currentAngle_B = 0;

const int ANGLE_OPEN = 150;   // 열림 각도
const int ANGLE_CLOSED = 0;   // 닫힘 각도
const unsigned long CLOSE_DELAY = 1000;  // 감지 후 닫히기까지 대기 시간(ms)

// 라인 A 상태 — 기본값 = "닫힘"(isClosed_A=true). 전원 켜진 직후/평상시엔 약통이
// 잠겨 있고, PC가 "OPEN A"를 보낼 때만(=로봇이 순찰 중 사람을 인식했을 때) 열린다.
unsigned long detectedSince_A = 0;
bool waitingToClose_A = false;
bool isClosed_A = true;

// 라인 B 상태 — 위와 동일.
unsigned long detectedSince_B = 0;
bool waitingToClose_B = false;
bool isClosed_B = true;

// ---------- PC(pillbox_controller.py) 시리얼 명령 ----------
// 사람을 인식하기 전까진 절대 안 열린다 — "OPEN A\n" / "OPEN B\n" 명령이 유일한
// 개방 트리거다(로봇이 순찰 중 사람을 인식했을 때만 PC가 이 명령을 보낸다).
// "CLOSE" 명령은 없다 — 일단 열리면 그 뒤론 이 펌웨어가 IR로 알아서 닫는다
// (감지 1초 후, 아래 loop() 로직). 사람이 없는데 IR 노이즈만으로 저절로 열리는
// 경로는 없다 — "열림"은 오직 이 명령으로만 시작된다.
//
// 응답: 명령을 처리하면 그 즉시 "OK A\n" / "OK B\n"을 돌려준다(잘못된 라인
// 문자면 무시하고 응답 없음 — pillbox_controller.py의 HW_SERVICE_TIMEOUT_SEC
// 만큼 기다리다 실패로 처리된다).
//
// "닫힘 완료" 알림: 열려있던 칸이 IR 감지 CLOSE_DELAY(1초) 후 실제로 닫히는
// 순간 "CLOSED A\n" / "CLOSED B\n"을 알림으로 보낸다 — pillbox_controller.py가
// 이걸 그대로 /careguard/hw/ir_triggered로 relay한다. 이 펌웨어에선 "IR 감지 후
// 닫힘"과 "회수 확인"이 사실상 같은 순간이라(감지 즉시 1초 뒤 자동으로 닫히는
// 구조), medication_task.py 쪽에도 별도 "닫기" 명령이 필요 없다 — 이 알림 한
// 줄이 배출 완료 신호를 겸한다. 한 번 닫힌 뒤에는(위 기본값과 같은 이유로)
// 다음 "OPEN" 명령이 올 때까지 저절로 다시 열리지 않는다.
void handleSerialCommand() {
  if (!Serial.available()) {
    return;
  }
  String line = Serial.readStringUntil('\n');
  line.trim();

  if (line == "OPEN A") {
    waitingToClose_A = false;
    isClosed_A = false;
    moveServoTo(servo_A, currentAngle_A, ANGLE_OPEN);
    Serial.println("OK A");
  } else if (line == "OPEN B") {
    waitingToClose_B = false;
    isClosed_B = false;
    moveServoTo(servo_B, currentAngle_B, ANGLE_OPEN);
    Serial.println("OK B");
  }
  // 그 외 문자열은 조용히 무시한다 — 잘못된 줄바꿈/노이즈로 매번 에러를 찍으면
  // 정상 디버그 출력(IR 상태 줄)까지 알아보기 힘들어진다.
}

void setup() {
  Serial.begin(9600);

  pinMode(IR_PIN_A, INPUT);
  pinMode(IR_PIN_B, INPUT);

  servo_A.attach(3);
  servo_B.attach(4);

  // 기본값 = 닫힘 — 전원이 켜지는 순간 약통은 잠겨 있어야 한다(사람을 인식하기
  // 전까진 절대 열리면 안 됨). isClosed_A/B 선언부의 기본값(true)과 짝을 맞춘다.
  currentAngle_A = ANGLE_CLOSED;
  currentAngle_B = ANGLE_CLOSED;
  servo_A.write(currentAngle_A);
  servo_B.write(currentAngle_B);
}

void moveServoTo(Servo &servo, int &currentAngle, int targetAngle) {
  if (currentAngle < targetAngle) {
    for (int angle = currentAngle; angle <= targetAngle; angle++) {
      servo.write(angle);
      delay(30);
    }
  } else if (currentAngle > targetAngle) {
    for (int angle = currentAngle; angle >= targetAngle; angle--) {
      servo.write(angle);
      delay(30);
    }
  }
  currentAngle = targetAngle;
}

void loop() {
  handleSerialCommand();

  int sensorA = digitalRead(IR_PIN_A);
  int sensorB = digitalRead(IR_PIN_B);

  bool detectedA = (sensorA == LOW);
  bool detectedB = (sensorB == LOW);

  Serial.print("IR-A(9번): ");
  Serial.print(detectedA ? "감지" : "없음");
  Serial.print("  |  IR-B(8번): ");
  Serial.println(detectedB ? "감지" : "없음");

  // ---------- 라인 A (IR 9번 / 서보 3번) ----------
  if (detectedA && !isClosed_A && !waitingToClose_A) {
    waitingToClose_A = true;
    detectedSince_A = millis();
  }
  if (waitingToClose_A && (millis() - detectedSince_A >= CLOSE_DELAY)) {
    moveServoTo(servo_A, currentAngle_A, ANGLE_CLOSED);
    isClosed_A = true;
    waitingToClose_A = false;
    Serial.println("CLOSED A");  // pillbox_controller.py가 ir_triggered로 relay
  }
  // (예전엔 여기서 "IR이 없어지면 다시 연다"는 로직이 있었지만 뺐다 — 이제
  // "열림"은 오직 OPEN 명령으로만 시작돼야 하므로, 닫힌 뒤 IR이 사라졌다고
  // 저절로 다시 열리면 안 된다. 다음 사람 인식 때 PC가 다시 OPEN을 보낸다.)

  // ---------- 라인 B (IR 8번 / 서보 4번) ----------
  if (detectedB && !isClosed_B && !waitingToClose_B) {
    waitingToClose_B = true;
    detectedSince_B = millis();
  }
  if (waitingToClose_B && (millis() - detectedSince_B >= CLOSE_DELAY)) {
    moveServoTo(servo_B, currentAngle_B, ANGLE_CLOSED);
    isClosed_B = true;
    waitingToClose_B = false;
    Serial.println("CLOSED B");  // pillbox_controller.py가 ir_triggered로 relay
  }
  // 라인 A와 같은 이유로 "IR 사라지면 자동 재개방" 로직은 없다.

  delay(100);
}
