"""app/services/notifier.py

보호자 알림 발송. Solapi SMS 포인트가 소진되어(2026-09-13) 이메일로 대체한다 —
smtplib + STARTTLS로 Gmail SMTP에 직접 붙는다(외부 라이브러리 불필요).

NOTIFY_MODE=mock(기본값)이면 실제 SMTP 연결 없이 notification_log만 남긴다 —
테스트·CI·로컬 개발이 이 값에 기대어 실수로 실제 메일을 보내지 않는다. 실제
발송은 .env에서 NOTIFY_MODE=live로 바꿔야만 일어난다.

guardian.email이 없는 보호자는 발송을 "시도조차 안 한" 것이므로 status='failed'가
아니라 'skipped'로 기록한다 — notification_log.status CHECK가 이미 이 값을 허용한다.
"""

import smtplib
from email.mime.text import MIMEText

from ..core import config
from ..core import db


def send_email(to_email: str, subject: str, body: str) -> tuple[bool, str]:
    """실제 SMTP 발송. 성공 시 (True, ""), 실패 시 (False, 에러 메시지)."""
    if config.NOTIFY_MODE != "live":
        return True, ""  # mock — 실제 연결 없이 성공으로 간주(호출부가 로그를 남긴다)

    msg = MIMEText(body, _charset="utf-8")
    msg["Subject"] = subject
    msg["From"] = config.SMTP_FROM
    msg["To"] = to_email

    try:
        with smtplib.SMTP(config.SMTP_HOST, config.SMTP_PORT, timeout=10) as smtp:
            if not config.SMTP_SECURE:
                smtp.starttls()
            smtp.login(config.SMTP_USER, config.SMTP_PASS)
            smtp.sendmail(config.SMTP_FROM, [to_email], msg.as_string())
        return True, ""
    except Exception as exc:  # smtplib가 던지는 예외 종류가 다양해 광범위하게 잡는다
        return False, str(exc)


def notify_guardian_for_med_missed(
    recipient_id: int,
    care_event_id: int,
    recipient_name: str,
    drug_name: str,
    missed_count: int,
    reannounce_count: int,
) -> None:
    """복약 미수령(MED_MISSED) 발생 시 그 대상자의 최우선순위 보호자에게 메일을 보낸다.
    scheduler.py::_medication_tick()이 care_event INSERT를 커밋한 *뒤에* 호출한다 —
    SMTP 네트워크 I/O를 DB 트랜잭션 안에 넣지 않기 위해 별도 연결을 쓴다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT id, name, email FROM guardian
            WHERE recipient_id = %s AND deregistered_at IS NULL AND notify_enabled = TRUE
            ORDER BY priority ASC
            LIMIT 1
            """,
            (recipient_id,),
        )
        guardian = cur.fetchone()

        subject = f"[CareGuard] {recipient_name}님 복약 미수령 안내"
        body = (
            f"{recipient_name}님이 {drug_name} 복약 시각이 지나도록 약을 수령하지 "
            f"않았습니다 ({missed_count}/{reannounce_count}회 경보).\n"
            "대시보드에서 상세 내용을 확인해주세요."
        )

        if guardian is None or not guardian["email"]:
            cur.execute(
                """
                INSERT INTO notification_log
                    (care_event_id, recipient_id, guardian_id, channel, template_code, body, status)
                VALUES (%s, %s, %s, 'mail', 'MED_MISSED', %s, 'skipped')
                """,
                (care_event_id, recipient_id, guardian["id"] if guardian else None, body),
            )
            conn.commit()
            return

        ok, err = send_email(guardian["email"], subject, body)
        cur.execute(
            """
            INSERT INTO notification_log
                (care_event_id, recipient_id, guardian_id, channel, template_code, body, status, sent_at)
            VALUES (%s, %s, %s, 'mail', 'MED_MISSED', %s, %s, CASE WHEN %s THEN NOW() ELSE NULL END)
            """,
            (care_event_id, recipient_id, guardian["id"], body, "sent" if ok else "failed", ok),
        )
        conn.commit()
