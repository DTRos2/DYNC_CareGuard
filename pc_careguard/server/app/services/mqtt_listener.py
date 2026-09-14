"""app/services/mqtt_listener.py

ESP32 응급 호출 버튼 -> MQTT -> care_event(SOS_BUTTON) 생성 + 로봇 자동 출동.

db/schema/careguard_schema_v1.sql의 call_button 테이블 코멘트에 이미 계약이
정해져 있다: "유선 전원, ESP32 + MQTT. 누르면 care_event(SOS_BUTTON)가 생성되고
로봇이 해당 위치로 출동한다." 이 파일은 그 계약의 서버 쪽 절반이다 —
- 토픽: cg/v1/button/{code}/press (call_button.code가 그 {code})
- 눌림 메시지를 받으면: call_button으로 로봇·위치를 역조회 -> care_event INSERT
  -> patrol.py::dispatch_drive_to_point()로 그 좌표에 로봇을 보낸다.

paho-mqtt는 자체 네트워크 스레드(loop_start())로 돌아서, 콜백 안에서 DB 쿼리·
WS 전송 같은 비동기 작업을 하려면 메인 이벤트 루프로 다시 넘겨야 한다 — 로봇
쪽 careguard_agent/downlink_node.py가 쓰는 것과 같은 스레드-이벤트루프 브리지
패턴(asyncio.run_coroutine_threadsafe)이다.

paho-mqtt 2.x부터 콜백 시그니처가 바뀌어서(reason_code/properties 추가)
CallbackAPIVersion.VERSION2를 명시한다 — 안 그러면 1.x 시그니처로 간주돼
DeprecationWarning이 뜨고, 언젠가 2.x에서 완전히 제거될 예정이라 신규 코드는
처음부터 VERSION2로 맞춘다.

브로커: deploy/docker-compose.yml의 mosquitto 서비스(로컬 개발 기본값
localhost:1883, core/config.py::MQTT_HOST/MQTT_PORT).

빌드/실행 미검증 — 실제 ESP32 보드로 아직 검증 못 함. mosquitto_pub으로 수동
발행해 서버 쪽 동작만 먼저 확인하는 걸 권장한다:

    mosquitto_pub -h localhost -p 1883 -t cg/v1/button/BTN_BEDROOM/press -m ""
"""
import json
import uuid
from datetime import datetime
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt

from ..api.v1.patrol import dispatch_drive_to_point
from ..core import config, db

KST = ZoneInfo("Asia/Seoul")

# call_button.code의 MQTT 토픽 패턴. paho는 와일드카드(+) 매치 그룹을 따로 안 주므로,
# 실제 code는 콜백에서 msg.topic 문자열을 "/"로 쪼개 꺼낸다(_on_message 참고).
TOPIC_FILTER = "cg/v1/button/+/press"


def start_mqtt_listener(loop) -> mqtt.Client | None:
    """앱 시작 시(main.py) 한 번 호출. asyncio 이벤트 루프(loop)는 paho의 콜백
    스레드에서 코루틴을 예약하는 데 쓴다.

    브로커가 이 컴퓨터에 없는 경우(로컬 개발용 대시보드 등, SOS 버튼 기능을
    안 쓰는 인스턴스)에도 서버 자체는 정상적으로 떠야 한다 — 여기서 실패해도
    나머지 startup 태스크(스케줄러, dashboard_ws 등)에는 영향이 없어야 하므로,
    연결 실패를 경고만 남기고 삼킨다. 이 경우 이 서버는 SOS 버튼 이벤트를
    받지 못하지만, 다른 기능들은 그대로 동작한다.
    """
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

    def _on_connect(c, userdata, connect_flags, reason_code, properties=None):
        c.subscribe(TOPIC_FILTER)

    def _on_message(c, userdata, msg):
        parts = msg.topic.split("/")
        # cg / v1 / button / {code} / press — 정확히 5토막이 아니면 우리 계약이 아니다.
        if len(parts) != 5 or parts[0:3] != ["cg", "v1", "button"] or parts[4] != "press":
            return
        code = parts[3]
        import asyncio  # 지역 임포트: 이 함수는 paho의 콜백 스레드에서 실행되므로

        asyncio.run_coroutine_threadsafe(_handle_press(code), loop)

    client.on_connect = _on_connect
    client.on_message = _on_message
    try:
        client.connect(config.MQTT_HOST, config.MQTT_PORT, keepalive=30)
    except OSError as e:
        print(
            f"[mqtt_listener] {config.MQTT_HOST}:{config.MQTT_PORT} 브로커에 연결하지 "
            f"못했습니다 ({e}). 응급 호출 버튼 기능은 비활성화된 채로 서버를 계속 "
            "시작합니다 — 로컬 개발 환경이면 mosquitto 컨테이너를 띄우세요."
        )
        return None
    client.loop_start()
    return client


async def _handle_press(code: str) -> None:
    """호출 버튼 눌림 1건 처리. call_button.code로 못 찾으면(미등록 버튼, 오배선 등)
    조용히 무시한다 — 존재하지 않는 이벤트를 굳이 로그로 남겨 소음을 만들 이유가 없다.
    """
    now = datetime.now(KST)
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT cb.code, cb.room_name, cb.waypoint_id, cb.pos_x, cb.pos_y, cb.yaw,
                   m.id AS map_id, r.id AS robot_id, r.code AS robot_code,
                   w.code AS wp_code, w.pos_x AS wp_x, w.pos_y AS wp_y, w.yaw AS wp_yaw
            FROM call_button cb
            JOIN map m ON m.id = cb.map_id
            JOIN robot r ON r.id = m.robot_id AND r.deregistered_at IS NULL
            LEFT JOIN waypoint w ON w.id = cb.waypoint_id
            WHERE cb.code = %s AND cb.deregistered_at IS NULL
            """,
            (code,),
        )
        button = cur.fetchone()
        if button is None:
            return

        cur.execute(
            "SELECT id FROM recipient WHERE robot_id = %s AND deregistered_at IS NULL LIMIT 1",
            (button["robot_id"],),
        )
        recipient = cur.fetchone()

        # waypoint_id가 있으면(등록된 지점 재사용) 그 좌표를, 없으면 call_button에
        # 직접 박아둔 좌표를 쓴다 — call_button.waypoint_id 코멘트와 같은 우선순위.
        if button["waypoint_id"] is not None:
            x, y, yaw, point_code = button["wp_x"], button["wp_y"], button["wp_yaw"], button["wp_code"]
        else:
            x, y, yaw, point_code = button["pos_x"], button["pos_y"], button["yaw"], code

        event_uid = f"EV-{button['robot_code']}-{now:%y%m%d}-{uuid.uuid4().hex[:8]}"
        summary = f"{button['room_name']} 호출 버튼 눌림"
        cur.execute(
            """
            INSERT INTO care_event (event_uid, recipient_id, robot_id, trigger_path, event_type,
                severity, summary, evidence, location_name, pos_x, pos_y, occurred_at)
            VALUES (%s, %s, %s, 'A', 'SOS_BUTTON', 'emergency', %s, %s, %s, %s, %s, NOW())
            RETURNING id
            """,
            (
                event_uid,
                recipient["id"] if recipient else None,
                button["robot_id"],
                summary,
                json.dumps({"button_code": code}),
                button["room_name"],
                x,
                y,
            ),
        )
        care_event_id = cur.fetchone()["id"]
        conn.commit()

    try:
        # care_event_id/severity를 실어 보내면 downlink_node.py가 record_trigger를
        # 발행해 video_recorder.py의 이벤트 클립 녹화를 켠다(P6 "이동"은 이 인자들을
        # 안 넘기므로 녹화가 안 켜진다 — dispatch_drive_to_point 공유 로직 참고).
        await dispatch_drive_to_point(
            button["robot_code"], point_code, x, y, yaw, button["map_id"],
            care_event_id=care_event_id, severity="emergency", reason=summary,
        )
    except Exception:
        # 로봇 미등록/SLAM 중/연결 끊김 등으로 출동 명령이 실패해도, 위에서 이미
        # care_event는 남았다 — 담당자가 대시보드로 보고 수동 대응할 수 있으니
        # 버튼 눌림 처리 자체를 실패로 취급하지 않는다(HTTP 요청 컨텍스트가 아니라
        # 이 예외를 돌려줄 곳도 없다).
        pass
