"""careguard_hw/pillbox_controller.py

약통 서보 2칸(A=좌측/B=우측)과 IR 센서 1개씩을 다루는 노드. careguard_mission/
medication_task.py가 유일한 클라이언트다. 실제 펌웨어(firmware/pillbox_firmware/
pillbox_firmware.ino)와 짝을 맞춘 프로토콜:

  PC -> 펌웨어: "OPEN A\n" / "OPEN B\n" — 그 즉시 서보를 연다. 평상시엔 펌웨어가
    IR 감지 1초 후 스스로 닫으므로(pillbox_firmware.ino의 CLOSE_DELAY) 닫기
    명령이 필요 없었지만, 2026-09-16부터 "CLOSE A\n" / "CLOSE B\n"도 추가됐다 —
    medication_task.py가 회수 대기 시간(RETRIEVAL_TIMEOUT_SEC) 안에 IR 감지를
    못 받으면(jammed) 열린 채로 방치하지 않고 이 명령으로 강제로 닫는다.
  펌웨어 -> PC: "OK A\n" / "OK B\n" — 방금 받은 OPEN/CLOSE 명령 ack(같은 응답을
                공유한다).
                "CLOSED A\n" / "CLOSED B\n" — IR 감지 1초 후 스스로 닫힐 때만
                오는 알림(=회수 확인 겸 배출 완료) — 이걸 IR_TRIGGERED_TOPIC으로
                relay한다. CLOSE 명령으로 강제로 닫힌 경우엔 이 알림이 안 온다
                (IR이 안 눌렸으니 회수된 게 아니라서 — pillbox_firmware.ino 참고).
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
import time

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
# 3초 안에 안 와서 hw_error로 잘못 끝나는 버그였다 — 4.5초보다 크게 6.0초로 올렸었다.
# 2026-09-16: 그런데도 R-07 실기에서 hw_error가 계속 재현돼(연결 94초 뒤, 갓 시작된
# 직후도 아닌 상황) 6초조차 마진이 부족한 것으로 보여 10.0초로 더 올린다 — 4.5초
# 최악 소요시간 대비 두 배 이상 여유.
ACK_TIMEOUT_SEC = 10.0

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

    def __init__(self, port: str, baudrate: int = 9600, on_line=None, logger=None):
        if serial is None:
            raise RuntimeError('pyserial이 설치돼 있지 않습니다 (python3-serial)')
        self._logger = logger
        self._serial = serial.Serial(port, baudrate, timeout=0.2)
        self._serial.reset_input_buffer()
        # 대부분의 아두이노 호환 보드는 시리얼 포트가 열리는 순간(DTR 신호) 자동으로
        # 리셋된다 — 그 직후 명령을 바로 보내면 보드가 아직 setup()도 못 끝낸 상태라
        # "OPEN A"가 씹히고 ACK_TIMEOUT_SEC 뒤 hw_error로 실패한다(2026-09-16 R-07
        # 실기에서 재부팅 직후 실제로 겪음 — 시리얼 모니터로 보드 자체는 정상 동작
        # 확인했는데도 첫 명령만 실패). 부트로더+스케치 초기화가 끝날 시간을 넉넉히
        # 확보한 뒤에야 명령을 받기 시작한다.
        time.sleep(2.0)
        self._write_lock = threading.Lock()
        self._on_line = on_line
        self._ack_events = {'A': threading.Event(), 'B': threading.Event()}
        self._running = True
        self._buf = b''
        self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self._reader_thread.start()

    def _log(self, msg: str):
        if self._logger:
            self._logger.info(f'[serial] {msg}')

    def _read_loop(self):
        # pyserial의 readline()에 기대지 않고 바이트를 직접 모아 '\n' 단위로 자른다.
        # 아두이노가 UTF-8 한글 디버그 줄("IR-A(9번): ...")과 순수 ASCII ack 줄("OK A")을
        # 같은 스트림에 섞어 보내는데, 이전엔 ascii로만 decode하면서 readline()의
        # 내부 타임아웃 처리에 전적으로 의존했다 — 여기서는 직접 버퍼를 들고 있다가
        # 완전한 줄이 모였을 때만 잘라내므로 줄 경계가 깨질 여지가 없다.
        while self._running:
            try:
                n = self._serial.in_waiting or 1
                chunk = self._serial.read(n)
            except Exception as exc:
                self._log(f'읽기 오류로 리더 스레드 종료: {exc!r}')
                break
            if not chunk:
                continue  # read 타임아웃(0.2초) — 그냥 다시 시도
            self._buf += chunk
            while b'\n' in self._buf:
                raw_line, self._buf = self._buf.split(b'\n', 1)
                line = raw_line.decode('utf-8', errors='ignore').strip()
                if not line:
                    continue
                if line in ('OK A', 'OK B'):
                    self._log(f'ack 수신: "{line}"')
                    self._ack_events[line[-1]].set()
                    continue
                # "CLOSED A"/"CLOSED B" 등 그 외 줄(디버그 IR 상태 포함)은 그대로
                # on_line으로 넘긴다 — 여기서 더 안 걸러도 PillboxController가
                # "CLOSED "로 시작하는지만 보고 스스로 거른다.
                if self._on_line:
                    self._on_line(line)

    def _send(self, command: str, channel: str) -> bool:
        self._ack_events[channel].clear()
        payload = f'{command} {channel}\n'.encode('ascii')
        with self._write_lock:
            self._serial.write(payload)
            self._serial.flush()
        self._log(f'명령 전송: "{command} {channel}"')
        acked = self._ack_events[channel].wait(timeout=ACK_TIMEOUT_SEC)
        if not acked:
            self._log(
                f'ack 타임아웃: "{command} {channel}" — {ACK_TIMEOUT_SEC:.0f}초 안에 '
                f'"OK {channel}"을 못 받음(버퍼에 남은 데이터: {self._buf!r})')
        return acked

    def open_slot(self, channel: str) -> bool:
        return self._send('OPEN', channel)

    def close_slot(self, channel: str) -> bool:
        # 2026-09-16 추가 — medication_task.py가 RETRIEVAL_TIMEOUT_SEC 안에 IR
        # 감지를 못 받았을 때(jammed) 열린 채로 방치하지 않고 강제로 닫는 용도.
        # pillbox_firmware.ino의 새 "CLOSE A/B" 명령과 짝을 맞춘다 — ack는 OPEN과
        # 같은 "OK A/B" 줄을 그대로 재사용한다.
        return self._send('CLOSE', channel)

    def close(self):
        self._running = False


class FakePillboxBackend:
    """시리얼 포트가 없는 개발 PC에서 서비스 인터페이스만 확인할 때 쓰는 스텁 —
    항상 성공을 반환하고 IR은 절대 안 켜진다(수동 테스트 때 IR_TRIGGERED_TOPIC에
    직접 pub해서 대신 확인)."""

    def open_slot(self, channel: str) -> bool:
        return True

    def close_slot(self, channel: str) -> bool:
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
                self._backend = SerialPillboxBackend(
                    serial_port, baudrate, on_line=self._on_serial_line, logger=self.get_logger())
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
        action = 'CLOSE' if request.close else 'OPEN'
        try:
            # request.close(2026-09-16 추가) — medication_task.py가 회수 대기
            # 시간이 지나도록 IR 감지를 못 받으면 여기로 강제 닫기를 요청한다.
            if request.close:
                response.success = self._backend.close_slot(channel)
            else:
                response.success = self._backend.open_slot(channel)
        except Exception as exc:  # noqa: BLE001 - 통신 예외도 실패 응답으로 수렴
            self.get_logger().error(f'서보 제어 실패({action} {channel}): {exc}')
            response.success = False
            return response

        # 2026-09-16 추가 — 예전엔 ack 타임아웃으로 실패해도(response.success=False,
        # 예외 없음) 아무 로그가 안 남아서 medication_task.py의 "hw_error" 로그만
        # 보고는 원인(ack 타임아웃인지, 다른 이유인지)을 알 수 없었다. 최소한
        # 여기서라도 "무엇을, 왜" 실패했는지 남긴다.
        if not response.success:
            self.get_logger().error(
                f'{action} {channel} 실패 — {ACK_TIMEOUT_SEC:.0f}초 안에 아두이노 "OK {channel}" '
                'ack를 못 받았습니다(시리얼 연결/전원/펌웨어 상태를 확인하세요)')
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
