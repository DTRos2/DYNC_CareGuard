# 2026-09-06 트러블슈팅 — 대상자 관리 > 등록/상세 모달

> `feature/dashboard-p3-recipient-dialog` 브랜치 작업 중 발생한 문제의 증상·원인·해결을 정리한 기록.
> 대상 파일: `pc_careguard/dashboard/src/dialogs/{p3_recipient_create_dialog.*, p3_recipient_detail_dialog.*}`

## 요약

| # | 증상 | 원인 | 해결 |
|---|---|---|---|
| 1 | 한글 입력 중 다른 입력창을 클릭하면 방금 조합하던 글자가 새 입력창에도 찍힘 | 포커스 전환보다 늦게 도착하는 비동기 IBus 커밋 콜백이 그 시점에 포커스를 쥔(=새로 옮겨간) 위젯으로 잘못 전달됨 | `FocusIn` 후 80ms 동안, 조합 없이 커밋 문자열만 있는 `QInputMethodEvent`를 `eventFilter()`에서 위젯에 전달하지 않고 삼킴 |

---

## 1. 한글 입력 중 필드 전환하면 이전 글자가 새 필드에도 찍힘

**증상**
보호자·복약 스케줄 등의 입력창에 한글을 조합 중(예: "ㄴ" 상태)일 때 마우스로 다른 입력창을 클릭하면, 원래 칸에는 정상적으로 글자가 커밋되면서 **방금 포커스를 받은 새 칸에도 같은 글자가 또 나타남**(`gui/issue/target5.png`로 최초 제보됨).

**진단 과정**

1차·2차·3차 시도가 모두 잘못된 가설로 실패했다.

1. `QGuiApplication::inputMethod()->commit()`을 `focusChanged` 시그널에서 호출 → 효과 없음.
2. 같은 `commit()`을 `FocusOut` 이벤트에서 호출 → 효과 없음.
3. `FocusIn` 시점에 입력기 상태를 `reset()` → 효과 없음.

세 번 다 "내 개발 환경엔 실제 한글 입력기가 없어서 완전히 검증은 못 했다"고 넘어가며 추측으로 고치려 한 게 반복 실패의 근본 원인이었다. 뒤늦게 `ps aux | grep ibus`로 확인해보니 이 샌드박스에는 **실제 `ibus-daemon` + `ibus-engine-hangul`이 이미 떠 있었다** — `xdotool`로 실제 키 이벤트를 보내 진짜 조합 상태(`preedit "ㄴ"`)를 만들고 다른 필드를 클릭해 버그를 100% 그대로 재현할 수 있었다. `QTest::keyClicks()`(ASCII만 지원)나 `QInputMethodEvent`를 코드로 직접 만들어 보내는 방식(실제 플랫폼 입력 컨텍스트를 안 거침)으로는 애초에 이런 부류의 버그를 재현할 수 없다.

실제 재현 후 `qDebug()`로 이벤트 도착 순서를 로그로 찍어보니, 로컬 Qt 위젯의 `FocusIn`/`FocusOut`보다 IBus의 "조합 확정(commit)" 콜백이 **더 늦게** 도착했다. 그 응답이 도착하는 시점에는 이미 포커스가 새 위젯으로 넘어가 있어서, Qt 플랫폼 플러그인이 그 커밋 문자열을 (원래 의도한 위젯이 아니라) **그 순간 포커스를 쥔 위젯**에 `QInputMethodEvent`로 전달했다.

**원인**
포커스 전환은 로컬(동기)이지만 IBus 커밋 콜백은 비동기라, "포커스가 바뀌기 전에 커밋을 강제한다"는 접근 자체가 타이밍상 불가능했다. 커밋 응답 자체가 포커스 전환 이후에 도착하기 때문이다.

**1차 해결 (한계 있었음)**
타이밍을 맞추는 대신 증상을 사후에 되돌리는 방식을 먼저 시도했다: 새 위젯이 `FocusIn`을 받는 순간 텍스트를 스냅샷 떠두고, 80ms 뒤 그 값이 달라져 있으면(=늦게 새어 들어온 커밋) 스냅샷으로 되돌림. 실제 ibus-hangul로 검증해 누수 자체는 막혔지만, **되돌리기 전까지 잘못된 글자가 화면에 잠깐 나타났다 지워지는 게 사용자 눈에 보이는 문제**가 남아 있었다.

**최종 해결**
되돌리는 대신 반영 자체를 막았다. `FocusIn` 시점부터 80ms 동안, 그 위젯에 도착하는 `QEvent::InputMethod` 중 `commitString()`은 있고 `preeditString()`은 비어 있는 이벤트(=새로 조합을 시작하는 게 아니라 이전 칸에서 새어 들어온 완성 글자와 정확히 같은 모양)만 `eventFilter()`에서 위젯에 전달하지 않고 그대로 삼킨다. 사용자가 새 칸에서 바로 한글 조합을 시작하는 정상 입력은 `preeditString`이 채워진 이벤트라 이 조건에 걸리지 않고 그대로 통과한다.

```cpp
bool P3RecipientCreateDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::FocusIn) {
        if (qobject_cast<QLineEdit*>(watched)) {
            imeGuardDeadline_[watched] = QDateTime::currentMSecsSinceEpoch() + 80;
        }
    } else if (event->type() == QEvent::InputMethod) {
        auto it = imeGuardDeadline_.find(watched);
        if (it != imeGuardDeadline_.end()) {
            if (QDateTime::currentMSecsSinceEpoch() < it.value()) {
                auto* ime = static_cast<QInputMethodEvent*>(event);
                if (!ime->commitString().isEmpty() && ime->preeditString().isEmpty()) {
                    imeGuardDeadline_.erase(it);
                    return true;  // 새어 들어온 커밋 — 위젯에 전달하지 않고 삼킴
                }
            } else {
                imeGuardDeadline_.erase(it);
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}
```

검증은 `QLineEdit` 2개만 있는 최소 스탠드얼론 하니스를 만들어, 실제 ibus-hangul + `xdotool`로 같은 시나리오를 재현하고 `textChanged` 로그로 확인했다: 누수된 커밋이 도착하면 삼켜지는 로그만 찍히고 그 칸의 `textChanged`는 **단 한 번도 발생하지 않았다**(=화면에 전혀 안 나타남). 그 직후 같은 칸에서 바로 이어서 정상적으로 한글 조합을 시작해도 문제없이 입력됐다.

`P3RecipientCreateDialog`·`P3RecipientDetailDialog` 양쪽 `eventFilter()`에 동일 패턴을 적용했다.

**교훈**
- "한글 입력할 때만 재현되는 버그"를 만나면 "내 환경엔 IME가 없어서 재현 못 한다"고 포기하지 말 것 — 실제 데몬(`ibus-daemon`/`ibus-engine-hangul`)이 떠 있는지 `ps aux`로 먼저 확인하고, 있으면 `xdotool`로 진짜 키 이벤트를 보내 재현해야 추측이 아니라 검증된 수정을 할 수 있다.
- CJK IME 조합 커밋은 플랫폼(IBus 등) 쪽 비동기 콜백이라 로컬 위젯 이벤트 순서만으로는 타이밍을 맞출 수 없는 경우가 있다. 이럴 때는 타이밍을 맞추려 하지 말고 증상의 "모양"(이 경우 `preeditString`이 빈 순수 커밋)으로 걸러내는 편이 더 견고하다.
- 사후 되돌리기(revert-after-the-fact) 방식은 기능적으로는 버그를 고쳐도 화면에 잘못된 상태가 잠깐 보이는 시각적 결함을 남길 수 있다 — 가능하면 반영 자체를 막는 방식을 우선 검토할 것.
