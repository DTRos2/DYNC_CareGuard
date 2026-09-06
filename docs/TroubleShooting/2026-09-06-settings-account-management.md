# 2026-09-06 트러블슈팅 — 시스템 설정 > 계정 관리 페이지

> `feature/p7-settings-page` 브랜치 작업 중 발생한 문제들의 증상·원인·해결을 정리한 기록.
> 대상 파일: `pc_careguard/dashboard/src/{pages/p7_settings_page.*, dialogs/p7_account_dialog.*, windows/{main.cpp,mainwindow.*}}`, `pc_careguard/server/app/accounts.py`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 대시보드 실행해도 아무 반응 없음 | 이전 빌드가 중단돼 0바이트 실행파일이 남았고, make가 재링크를 건너뜀 | 빈 파일 삭제 후 강제 재링크 |
| 2 | 활성 체크박스 해제·저장해도 계속 "활성"으로 보임 | 계정 수정 저장 API 자체가 없어서 로컬 화면만 갱신되고 DB에는 반영 안 됨 | `PATCH /accounts/{id}` 등 실제 저장 API 추가 |
| 3 | 관리자 계정으로 로그인해도 삭제 체크박스가 안 보임 | 로그인 성공 시 받은 실제 로그인 ID를 어디에도 전달하지 않고 `"admin"` 하드코딩 값을 계속 씀 | 로그인 ID를 `MainWindow` → `P7SettingsPage`까지 실제로 전달 |
| 4 | 계정을 삭제했는데 DB에는 행이 남아있음 | 버그 아님 — 스키마 전체가 쓰는 소프트 삭제(`deleted_at`) 방식이라 물리적 행은 남고 조회에서만 제외됨 | 설계 의도 확인 후 정상 동작으로 결론 |
| 5 | 담당 대상자 검색창에서 "박"을 쳐도 입력창을 다시 클릭해야 걸러짐 | 한글 입력기(IME) 조합 중(preedit)에는 Qt `textChanged`가 발생하지 않음 — 조합 확정(커밋) 시점에만 발동 | `eventFilter`로 조합 중 문자열까지 가로채 실시간 반영 |

---

## 1. 대시보드 실행이 안 됨 (빈 바이너리)

**증상**
`./build/careguard_dashboard`를 여러 번 실행해도 아무 창도 뜨지 않고 바로 프롬프트로 돌아옴. 에러 메시지도 없음.

**진단 과정**
- `file build/careguard_dashboard` → `empty` — 실행파일이 **0바이트**였음.
- `cmake --build .`을 그냥 돌리면 "Built target"만 뜨고 실제로는 재링크를 안 함 — make가 파일 크기가 아니라 **타임스탬프**만 보고 "이미 최신"이라 판단했기 때문.

**원인**
이전 빌드(링크) 과정이 디스크 풀/강제종료 등으로 중간에 끊겼는데, 그 시점에 이미 0바이트짜리 실행파일이 생성되면서 타임스탬프가 갱신돼버림. 이후 `cmake --build`가 의존 파일들과 비교했을 때 "결과물이 더 최신"이라고 착각해 재링크를 건너뜀.

**해결**
```bash
rm -f careguard_dashboard
cmake --build . --target careguard_dashboard
```
빈 파일을 지우고 강제로 다시 링크 → 정상 22MB ELF 실행파일 생성, 정상 기동 확인.

**교훈**: 빌드가 중간에 끊긴 뒤 "Built target"만 보고 넘어가지 말 것. 산출물 크기/타임스탬프를 직접 확인하는 게 안전하다.

---

## 2. 계정 활성/비활성 저장이 DB에 반영 안 됨

**증상**
계정 수정 모달에서 "활성" 체크박스를 끄고 저장하면 화면에는 "비활성"으로 바뀌지만, 앱을 재시작하거나 목록을 다시 불러오면 다시 "활성"으로 돌아옴.

**진단 과정**
1. 처음엔 `AccountDialog::updateAdminBanners()`의 "본인 계정/마지막 관리자는 체크박스 비활성화" 로직을 의심했으나, 사용자가 **다른 계정**을 수정 중이라고 확인 → 이 가설 기각.
2. `p7_settings_page.cpp`의 `accountSaved` 핸들러 코드를 다시 확인하니 주석에 이렇게 쓰여 있었음: *"계정 자체 수정은 아직 서버 연동 범위 밖 — 이번 세션 내 로컬 표시만 갱신한다."*
3. 백엔드(`accounts.py`) 확인 → 계정 수정용 `PATCH`/`PUT` 엔드포인트가 **아예 없었음**. `GET /accounts`(목록), `POST /accounts`(등록), `PUT /{id}/recipients`(담당 배정)만 존재.

**원인**
프론트가 저장 시 화면(`accounts_` 벡터)만 갱신하고 서버에 반영하는 API 호출 자체가 없어서, `fetchAccounts()`가 다시 실행되는 순간(재시작 등) DB의 원래 값으로 되돌아감.

**해결**
- 백엔드에 `PATCH /accounts/{id}`(표시 이름·역할·활성 상태) 와 `PATCH /accounts/{id}/password`(비밀번호 변경) 추가. 활성→비활성 전환 시 `user_action_log`에 `account_activate`/`account_deactivate`/`account_update`로 감사 기록.
- 프론트 `P7SettingsPage::updateAccount()`/`changePassword()`를 추가해 실제 API를 호출하도록 변경, 성공 시 `fetchAccounts()`로 DB 기준 재조회.
- 원격 DB(테스트 계정)로 비활성화→활성화→비밀번호 변경→실제 로그인까지 end-to-end 검증 완료.

---

## 3. 관리자로 로그인해도 삭제 체크박스가 안 보임

**증상**
DB상 역할이 "관리자"인 계정으로 로그인했는데도, 계정 관리 탭에 삭제용 체크박스/삭제 버튼이 뜨지 않음.

**진단 과정**
- 체크박스 노출 조건은 `P7SettingsPage::isCurrentUserAdmin()`이 판단하는데, 이 함수는 `currentUserLoginId_`와 로그인 ID가 같은 계정의 role을 봄.
- `p7_settings_page.h`를 보니 `currentUserLoginId_`가 `QStringLiteral("admin")`으로 **하드코딩**돼 있었음.
- 로그인 흐름(`LoginWindow::loginSucceeded(username)` → `main.cpp`)을 추적해보니, 실제 로그인 ID는 `MainWindow::setUserName()`(상단바 표시용)에만 전달되고 `P7SettingsPage`로는 전달된 적이 없었음.

**원인**
실제 로그인 세션과 무관하게 `currentUserLoginId_`가 항상 `"admin"`이라는 고정 문자열이었기 때문에, DB의 실제 관리자 로그인 ID(예: `admin_test`)와 문자열이 달라 `isCurrentUserAdmin()`이 항상 `false`를 반환.

**해결**
- `P7SettingsPage::setCurrentUserLoginId(loginId)` 추가 (호출 시 `refreshAccountTable()`로 즉시 재판정).
- `MainWindow`가 `P7SettingsPage` 포인터를 들고 있다가, 이미 로그인 ID를 받고 있던 `setUserName()` 안에서 그대로 `settingsPage_->setCurrentUserLoginId(name)`도 호출하도록 연결 (`videoPage_->setUserName()`과 같은 패턴).

---

## 4. 계정을 삭제했는데 DB에 행이 남아있음 (버그 아님)

**증상**
계정 삭제 후 UI 목록에서는 사라졌는데, DB에서 직접 `SELECT`로 확인하면 해당 행이 그대로 존재.

**진단 과정**
- 실제 DB 조회 결과: 해당 행의 `deleted_at`에 삭제 시각이 찍혀 있고 `is_active = false`로 바뀜.
- `user_action_log`에도 `account_delete` 기록이 남아 있음.
- `list_accounts()` 쿼리가 `WHERE u.deleted_at IS NULL` 조건으로 걸러서 가져오기 때문에 UI 목록에는 정상적으로 안 보임.

**원인**
버그가 아니라 **의도된 소프트 삭제**. `robot`, `recipient`, `camera_recording` 등 스키마 전반에서 이미 쓰고 있는 `deleted_at` 패턴과 동일하게, `user_action_log` 등 기존 참조가 깨지지 않도록 행을 물리적으로 지우지 않음.

**결론**: 정상 동작. 필요하면 하드 삭제로 바꿀 수 있으나, 감사 로그 참조 무결성 때문에 권장하지 않음.

---

## 5. 담당 대상자 검색이 실시간으로 안 됨 (한글 입력)

**증상**
계정 수정 모달의 "담당 대상자" 검색창에 "박"을 입력해도 목록이 바로 안 걸러지고, 입력창을 다시 클릭해야 필터링됨. 글자를 입력하면 그 글자에 검은 밑줄(마크)이 생기는 게 눈에 띔.

**진단 과정**
- 코드상 `recipientSearchEdit_`는 이미 `QLineEdit::textChanged`에 연결돼 있어 "Enter를 쳐야 검색되는" 구조는 아니었음.
- 사용자가 설명한 "글자에 검은 마크가 생긴다"는 특징이 한글 입력기(IME)의 **조합 중(preedit) 표시**(밑줄)와 정확히 일치.
- Qt에서는 IME가 글자를 조합하는 동안에는 `QLineEdit::textChanged`가 발생하지 않고, 조합이 **확정(커밋)되는 시점**(다음 글자 입력, 포커스 이동 등)에만 발생한다는 것을 확인.

**원인**
검색 로직 자체는 정상이었지만, 애초에 "조합 중" 상태에서는 Qt가 텍스트 변경 신호를 주지 않아서 필터링이 트리거될 방법이 없었음.

**해결**
`AccountDialog`에 `eventFilter()`를 구현해 `recipientSearchEdit_`의 `QEvent::InputMethod` 이벤트를 가로채고, 아직 커밋 안 된 조합 문자열(`QInputMethodEvent::preeditString()`)까지 합쳐서 매 순간 필터링하도록 변경. 이제 글자가 조합 중(밑줄 표시)인 상태에서도 바로 검색 결과가 갱신됨.

```cpp
bool AccountDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == recipientSearchEdit_ && event->type() == QEvent::InputMethod) {
        auto* imEvent = static_cast<QInputMethodEvent*>(event);
        applyRecipientFilter(recipientSearchEdit_->text() + imEvent->preeditString());
    }
    return QDialog::eventFilter(watched, event);
}
```

**교훈**: 한글(CJK) 입력이 관련된 실시간 검색/유효성 검사 UI는 `textChanged`만으로는 조합 중 상태를 놓친다. `QEvent::InputMethod`/`preeditString()`을 함께 고려해야 한다.
