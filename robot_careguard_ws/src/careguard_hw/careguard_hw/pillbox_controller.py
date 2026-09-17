"""careguard_hw/pillbox_controller.py

약통 서보 2칸(A=좌측/B=우측)과 IR 센서 1개씩을 다루는 노드. careguard_mission/
medication_task.py가 유일한 클라이언트다. 실제 펌웨어(firmware/pillbox_firmware/
pillbox_firmware.ino)와 짝을 맞춘 프로토콜:

  PC -> 펌웨어: "OPEN A\n" / "OPEN B\n" — 그 즉시 서보를 여는 것 외에 펌웨어가
    할 일이 없다. "닫기" 명령은 없다 — 펌웨어가 IR 감지 1초 후 스스로 닫기
    때문(pillbox_firmware.ino의 CLOSE_DELAY). 그래서 PillboxSlot.srv도 "열기"
    하나만 있다.
  펌웨어 -> PC: "OK A\n" / "OK B\n" — 방금 받은 OPEN 명령 ack.
                "CLOSED A\n" / "CLOSED B\n" — 그 라인이 IR 감지 1초 후 스스로
                닫힘(=회수 확인 겸 배출 완료) — 이걸 IR_TRIGGERED_TOPIC으로 relay.
                그 외 줄(주기적인 "IR-A(9번): 감지 | IR-B(8번): 없음" 디버그
                출력)은 무시.

펌웨어가 100ms마다 디버그 줄을 계속 찍기 때문에, "명령 보내고 readline() 한 번
읽기" 방식은 그 사이 디버그 줄을 ack로 잘못 읽기 쉽다 — 그래서 시리얼은 전용
백그라운드 스레드 하나가 계속 읽으면서 줄 종류(OK/CLOSED/그 외)를 구분한다.

하드웨어 제약: 물리 서보 칸은 2개뿐이다. medication_schedule.slot_no는 1(아침)/
2(점심)/3(저녁) 세 값을 쓰지만(DB CHECK 제약, 스키마 코멘트는 "3칸 구성"이라
적혀 있음 — 코멘트는 고치지 않기로 확정, 실제 매핑은 여기서만 처리), 실제로는
slot_to_channel()로 순환 배치한다: 1번(아침)→A, 2번(점심)→B, 3번(저녁)→A(1번
칸을 재사용). 하루 세 번 중 두 번만 동시에 "채워진 채" 대기할 수 있다는 뜻이라,
운영 절차상 아침 약을 꺼낸 뒤에 저녁 약을 채워 넣어야 한다(이 노드는 그 절차를
강제하지 않는다 — UI/운영 가이드 영역).

빌드/실행 미검증 — 펌웨어 프로토콜은 사용자가 공유한 실제 .ino 기준으로 맞췄지만,
실 시리얼 포트로 오가는 걸 아직 한 번도 확인 못 했다.
"""
import threading

import rclpy
from rclpy.node import Node
from std_msgs.msg import String

from careguard_msgs.srv import PillboxSlot

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover - 개발 PC에 pyserial이 없을 수 있음
    serial = None

PILLBOX_SLOT_SERVICE = '/careguard/hw/pillbox_slot'
# medication_task.py의 같은 이름 상수와 같은 문자열이어야 한다(패키지 간 공유
# 없이 토픽명을 각자 하드코딩하는 이 코드베이스의 기존 방식). payload는
# std_msgs/String이고 값은 'A' 또는 'B' — 여러 슬롯을 동시에 다룰 일은 없지만
# (PillboxController가 goal 하나만 허용), "지금 닫힌 게 내가 기다리던 채널인지"
# 구분할 근거가 필요해 Bool이 아니라 채널 문자를 그대로 싣는다.
IR_TRIGGERED_TOPIC = '/careguard/hw/ir_triggered'

# OPEN 명령을 보내고 "OK <채널>" ack를 기다리는 최대 시간. 펌웨어의 moveServoTo()가
# ANGLE_CLOSED(0)->ANGLE_OPEN(150)을 1도당 delay(30)ms로 움직이고, "OK <채널>"은 그
# 이동이 다 끝난 뒤에야 찍힌다(handleSerialCommand()가 moveServoTo()를 블로킹으로
# 부름) — 즉 최악의 경우 150 * 30ms ≈ 4.5초가 걸린 "다음"에야 ack가 온다. 예전엔
# 3.0초였는데, 실 하드웨어로 확인해보니(2026-09-11) 서보는 실제로 열렸는데도 ack가
# 3초 안에 안 와서 hw_error로 잘못 끝나는 버그였다 — 4.5초보다 확실히 크게 잡는다.
ACK_TIMEOUT_SEC = 6.0

# slot_no(1~3) -> 서보 채널('A'/'B'). 3번(저녁)이 1번(아침) 칸을 재사용한다 —
# 클래스 docstring의 하드웨어 제약 참고.
_SLOT_TO_CHANNEL = {1: 'A', 2: 'B', 3: 'A'}


def slot_to_channel(slot_no: int) -> str:
    if slot_no not in _SLOT_TO_CHANNEL:
        raise ValueError(f'유효하지 않은 slot_no: {slot_no} (1~3만 허용)')
    return _SLOT_TO_CHANNEL[slot_no]


class SerialPillboxBackend:
    """아두이노(pillbox_firmware.ino)와 시리얼로 통신하는 실제 백엔드.

    on_line 콜백은 백그라운드 읽기 스레드에서(= ROS 콜백 스레드가 아님) 매 줄마다
    불린다 — PillboxController가 여기서 "CLOSED A/B"만 걸러 토픽으로 publish한다.
    """

    def __init__(self, port: str, baudrate: int = 9600, on_line=None):
        if serial is None:
            raise RuntimeError('pyserial이 설치돼 있지 않습니다 (python3-serial)')
        self._serial = serial.Serial(port, baudrate, timeout=1.0)
        self._write_lock = threading.Lock()
        self._on_line = on_line
        self._ack_events = {'A': threading.Event(), 'B': threading.Event()}
        self._running = True
        self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self._reader_thread.start()

    def _read_loop(self):
        while self._running:
            try:
                raw = self._serial.readline()
            except Exception:
                break
            if not raw:
                continue  # readline 타임아웃(1초) — 그냥 다시 시도
            line = raw.decode('ascii', errors='ignore').strip()
            if not line:
                continue
            if line in ('OK A', 'OK B'):
                self._ack_events[line[-1]].set()
                continue
            # "CLOSED A"/"CLOSED B" 등 그 외 줄(디버그 IR 상태 포함)은 그대로
            # on_line으로 넘긴다 — 여기서 더 안 걸러도 PillboxController가
            # "CLOSED "로 시작하는지만 보고 스스로 거른다.
            if self._on_line:
                self._on_line(line)

    def open_slot(self, channel: str) -> bool:
        self._ack_events[channel].clear()
        with self._write_lock:
            self._serial.write(f'OPEN {channel}\n'.encode('ascii'))
        return self._ack_events[channel].wait(timeout=ACK_TIMEOUT_SEC)

    def close(self):
        self._running = False


class FakePillboxBackend:
    """시리얼 포트가 없는 개발 PC에서 서비스 인터페이스만 확인할 때 쓰는 스텁 —
    항상 성공을 반환하고 IR은 절대 안 켜진다(수동 테스트 때 IR_TRIGGERED_TOPIC에
    직접 pub해서 대신 확인)."""

    def open_slot(self, channel: str) -> bool:
        return True

    def close(self):
        pass


class PillboxController(Node):
    def __init__(self):
        super().__init__('pillbox_controller')
        self.declare_parameter('serial_port', '')
        self.declare_parameter('baudrate', 9600)
        serial_port = self.get_parameter('serial_port').value
        baudrate = self.get_parameter('baudrate').value

        self._ir_pub = self.create_publisher(String, IR_TRIGGERED_TOPIC, 10)

        if serial_port:
            try:
                self._backend = SerialPillboxBackend(serial_port, baudrate, on_line=self._on_serial_line)
                self.get_logger().info(f'시리얼 백엔드 연결: {serial_port}@{baudrate}')
            except Exception as exc:  # noqa: BLE001 - 포트 연결 실패해도 노드는 뜨게
                self.get_logger().error(f'시리얼 연결 실패({exc}) — Fake 백엔드로 대체합니다')
                self._backend = FakePillboxBackend()
        else:
            self.get_logger().warn(
                'serial_port 파라미터가 없어 FakePillboxBackend로 시작합니다 (실 하드웨어 미연결)')
            self._backend = FakePillboxBackend()

        self.create_service(PillboxSlot, PILLBOX_SLOT_SERVICE, self._on_pillbox_slot)

    def _on_serial_line(self, line: str):
        # 백그라운드 읽기 스레드에서 호출된다 — rclpy 퍼블리셔는 스레드 안전하므로
        # 그대로 publish한다. "CLOSED A"/"CLOSED B" 외의 줄(주기적 IR 디버그 출력
        # 등)은 여기서 조용히 버린다.
        if not line.startswith('CLOSED '):
            return
        channel = line.split(' ', 1)[1]
        if channel not in ('A', 'B'):
            return
        msg = String()
        msg.data = channel
        self._ir_pub.publish(msg)

    def _on_pillbox_slot(self, request, response):
        try:
            channel = slot_to_channel(request.slot_no)
        except ValueError as exc:
            self.get_logger().error(str(exc))
            response.success = False
            return response
        try:
            response.success = self._backend.open_slot(channel)
        except Exception as exc:  # noqa: BLE001 - 통신 예외도 실패 응답으로 수렴
            self.get_logger().error(f'서보 제어 실패: {exc}')
            response.success = False
        return response


def main(args=None):
    rclpy.init(args=args)
    node = PillboxController()
    try:
        rclpy.spin(node)
    finally:
        node._backend.close()
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
