from collections import defaultdict

import bcrypt
import psycopg2
from fastapi import APIRouter, HTTPException
from typing import List

from . import db
from ..schemas import (
    AccountItem,
    AssignRecipientsRequest,
    ChangePasswordRequest,
    DeleteAccountsRequest,
    RecipientOption,
    RegisterAccountRequest,
    RegisterAccountResponse,
    UpdateAccountRequest,
)

router = APIRouter(prefix="/accounts", tags=["accounts"])

PASSWORD_POLICY_ERROR = "비밀번호는 8자 이상이며 영문과 숫자를 모두 포함해야 합니다"


def _check_password_policy(password: str) -> None:
    """8자 이상 · 영문/숫자 조합 — 대시보드(AccountDialog) 쪽 검증과 동일한 정책.

    클라이언트 검증만 믿지 않고 여기서도 막아, API를 직접 호출해도 우회할 수 없게 한다.
    """
    has_letter = any(c.isalpha() for c in password)
    has_digit = any(c.isdigit() for c in password)
    if len(password) < 8 or not has_letter or not has_digit:
        raise HTTPException(status_code=400, detail=PASSWORD_POLICY_ERROR)

LIST_QUERY = """
    SELECT u.id, u.login_id, u.display_name, u.is_active, u.last_login_at,
           r.code AS role_code, r.name AS role_name
    FROM careguard_user u
    JOIN role r ON r.id = u.role_id
    WHERE u.deleted_at IS NULL
    ORDER BY u.id
"""

# 담당 대상자·로봇 요약(charge_summary)의 원본. recipient.operator_id로 역조회한다.
CHARGE_QUERY = """
    SELECT rc.operator_id, rc.name AS recipient_name, rb.code AS robot_code
    FROM recipient rc
    LEFT JOIN robot rb ON rb.id = rc.robot_id
    WHERE rc.deregistered_at IS NULL AND rc.operator_id IS NOT NULL
"""


def _row_to_item(row, charge_summary: str = None) -> AccountItem:
    return AccountItem(
        id=row["id"],
        login_id=row["login_id"],
        display_name=row["display_name"],
        role_code=row["role_code"],
        role_name=row["role_name"],
        is_active=row["is_active"],
        last_login_at=row["last_login_at"],
        charge_summary=charge_summary,
    )


@router.get("", response_model=List[AccountItem])
def list_accounts():
    """계정 관리 탭 목록. P7SettingsPage가 화면 진입 시 이걸로 채운다."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(LIST_QUERY)
        rows = cur.fetchall()

        cur.execute(CHARGE_QUERY)
        charge_rows = cur.fetchall()

    charges_by_operator = defaultdict(list)
    for row in charge_rows:
        label = row["recipient_name"]
        if row["robot_code"]:
            label = f"{label} {row['robot_code']}"
        charges_by_operator[row["operator_id"]].append(label)

    return [
        _row_to_item(r, " · ".join(charges_by_operator.get(r["id"], [])) or None)
        for r in rows
    ]


@router.get("/recipients", response_model=List[RecipientOption])
def list_assignable_recipients():
    """계정 수정 모달의 "담당 대상자" 체크리스트를 채우는 목록."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT rc.id, rc.code, rc.name, rb.code AS robot_code, rc.operator_id, rc.guardian_user_id
            FROM recipient rc
            LEFT JOIN robot rb ON rb.id = rc.robot_id
            WHERE rc.deregistered_at IS NULL
            ORDER BY rc.code
            """
        )
        rows = cur.fetchall()
    return [RecipientOption(**row) for row in rows]


@router.put("/{user_id}/recipients", status_code=204)
def assign_recipients(user_id: int, payload: AssignRecipientsRequest):
    """이 계정이 담당할 대상자 목록을 통째로 교체한다.

    기존에 이 계정 담당이었지만 새 목록에서 빠진 대상자는 미배정(NULL)으로 되돌리고,
    새 목록에 있는 대상자는 이 계정 담당으로 설정한다. 다른 계정이 담당 중이던
    대상자를 새 목록에 넣으면 이 계정 쪽으로 넘어온다(교체).
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM careguard_user WHERE id = %s AND deleted_at IS NULL", (user_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=404, detail="계정을 찾을 수 없습니다")

        cur.execute(
            "UPDATE recipient SET operator_id = NULL WHERE operator_id = %s AND deregistered_at IS NULL",
            (user_id,),
        )
        if payload.recipient_ids:
            cur.execute(
                "UPDATE recipient SET operator_id = %s WHERE id = ANY(%s) AND deregistered_at IS NULL",
                (user_id, payload.recipient_ids),
            )
        conn.commit()

@router.put("/{user_id}/guardian-recipients", status_code=204)
def assign_guardian_recipients(user_id: int, payload: AssignRecipientsRequest):
    """이 보호자 계정이 볼 대상자 목록을 통째로 교체한다. assign_recipients(operator)와 동일 패턴."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM careguard_user WHERE id = %s AND deleted_at IS NULL", (user_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=404, detail="계정을 찾을 수 없습니다")

        cur.execute(
            "UPDATE recipient SET guardian_user_id = NULL WHERE guardian_user_id = %s AND deregistered_at IS NULL",
            (user_id,),
        )
        if payload.recipient_ids:
            cur.execute(
                "UPDATE recipient SET guardian_user_id = %s WHERE id = ANY(%s) AND deregistered_at IS NULL",
                (user_id, payload.recipient_ids),
            )
        conn.commit()


@router.post("", response_model=RegisterAccountResponse, status_code=201)
def register_account(payload: RegisterAccountRequest):
    """계정 등록 모달의 "등록" 버튼이 호출하는 엔드포인트. 비밀번호는 여기서 해시한다."""
    _check_password_policy(payload.password)

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id, code, name FROM role WHERE code = %s", (payload.role_code,))
        role = cur.fetchone()
        if role is None:
            raise HTTPException(status_code=400, detail="알 수 없는 역할입니다")

        password_hash = bcrypt.hashpw(payload.password.encode(), bcrypt.gensalt()).decode()

        try:
            cur.execute(
                """
                INSERT INTO careguard_user (login_id, password_hash, role_id, is_active, display_name)
                VALUES (%s, %s, %s, %s, %s)
                RETURNING id
                """,
                (payload.login_id, password_hash, role["id"], payload.is_active, payload.display_name),
            )
        except psycopg2.errors.UniqueViolation:
            conn.rollback()
            raise HTTPException(status_code=409, detail="이미 사용 중인 아이디입니다")

        user_id = cur.fetchone()["id"]
        conn.commit()

    return RegisterAccountResponse(
        id=user_id,
        login_id=payload.login_id,
        display_name=payload.display_name,
        role_code=role["code"],
        role_name=role["name"],
        is_active=payload.is_active,
        last_login_at=None,
    )


@router.patch("/{user_id}", response_model=AccountItem)
def update_account(user_id: int, payload: UpdateAccountRequest):
    """계정 수정 모달의 "저장" 버튼이 호출하는 엔드포인트. 표시 이름·역할·활성 상태를 갱신한다.

    로그인 ID는 수정 모달에서 잠겨 있어 여기서 바꾸지 않는다.
    """
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT is_active FROM careguard_user WHERE id = %s AND deleted_at IS NULL",
            (user_id,),
        )
        current = cur.fetchone()
        if current is None:
            raise HTTPException(status_code=404, detail="계정을 찾을 수 없습니다")

        cur.execute("SELECT id, code, name FROM role WHERE code = %s", (payload.role_code,))
        role = cur.fetchone()
        if role is None:
            raise HTTPException(status_code=400, detail="알 수 없는 역할입니다")

        cur.execute(
            """
            UPDATE careguard_user
            SET display_name = %s, role_id = %s, is_active = %s
            WHERE id = %s
            """,
            (payload.display_name, role["id"], payload.is_active, user_id),
        )

        # 활성 계정을 비활성화하면 담당 중이던 대상자를 미배정으로 되돌린다(delete_accounts와 동일한
        # 방침) — 비활성 계정이 대상자를 계속 담당한 것처럼 남아 있으면 안 된다.
        if current["is_active"] and not payload.is_active:
            cur.execute(
                "UPDATE recipient SET operator_id = NULL WHERE operator_id = %s AND deregistered_at IS NULL",
                (user_id,),
            )

        # 활성 상태가 실제로 바뀐 경우에만 activate/deactivate로, 그 외 수정은 account_update로 남긴다.
        # 실제 로그인 세션(조작 주체) 연동은 범위 밖이라 user_id는 대상 계정 자신으로 기록한다.
        if current["is_active"] != payload.is_active:
            action_type = "account_activate" if payload.is_active else "account_deactivate"
        else:
            action_type = "account_update"
        cur.execute(
            "INSERT INTO user_action_log (user_id, action_type, target_table, target_id) "
            "VALUES (%s, %s, 'careguard_user', %s)",
            (user_id, action_type, user_id),
        )

        conn.commit()

        cur.execute(
            """
            SELECT u.id, u.login_id, u.display_name, u.is_active, u.last_login_at,
                   r.code AS role_code, r.name AS role_name
            FROM careguard_user u
            JOIN role r ON r.id = u.role_id
            WHERE u.id = %s
            """,
            (user_id,),
        )
        row = cur.fetchone()

    return _row_to_item(row)


@router.patch("/{user_id}/password", status_code=204)
def change_password(user_id: int, payload: ChangePasswordRequest):
    """계정 수정 모달의 "변경" 버튼이 호출하는 엔드포인트. 새 비밀번호를 해시해 저장한다."""
    _check_password_policy(payload.new_password)
    password_hash = bcrypt.hashpw(payload.new_password.encode(), bcrypt.gensalt()).decode()
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "UPDATE careguard_user SET password_hash = %s WHERE id = %s AND deleted_at IS NULL",
            (password_hash, user_id),
        )
        if cur.rowcount == 0:
            conn.rollback()
            raise HTTPException(status_code=404, detail="계정을 찾을 수 없습니다")

        cur.execute(
            "INSERT INTO user_action_log (user_id, action_type, target_table, target_id) "
            "VALUES (%s, 'password_change', 'careguard_user', %s)",
            (user_id, user_id),
        )
        conn.commit()


@router.post("/delete", status_code=204)
def delete_accounts(payload: DeleteAccountsRequest):
    """계정 관리 탭의 체크박스로 선택한 계정들을 삭제한다. 관리자 권한 여부는 클라이언트에서 판단한다.

    실제 행은 지우지 않고 deleted_at을 채운다 — list_accounts()의 WHERE u.deleted_at IS NULL
    조건과 동일하게, 기존 참조(user_action_log 등)를 보존하면서 목록에서만 제외한다.
    담당 중이던 대상자는 미배정으로 되돌린다.
    """
    if not payload.user_ids:
        return

    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(
            "UPDATE recipient SET operator_id = NULL "
            "WHERE operator_id = ANY(%s) AND deregistered_at IS NULL",
            (payload.user_ids,),
        )
        cur.execute(
            """
            UPDATE careguard_user
            SET deleted_at = NOW(), is_active = FALSE
            WHERE id = ANY(%s) AND deleted_at IS NULL
            RETURNING id
            """,
            (payload.user_ids,),
        )
        deleted_ids = [row["id"] for row in cur.fetchall()]
        for uid in deleted_ids:
            cur.execute(
                "INSERT INTO user_action_log (user_id, action_type, target_table, target_id) "
                "VALUES (%s, 'account_delete', 'careguard_user', %s)",
                (uid, uid),
            )
        conn.commit()
