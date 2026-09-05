#pragma once
#include <QLineEdit>

// 한글처럼 조합형 입력(IME)을 쓰는 언어는 완성되지 않고 조합 중인 글자(preedit)가
// QLineEdit::text()에는 반영되지 않는다 — 예를 들어 "김순자"를 입력하는 중 "자"의
// 마지막 자모가 아직 조합 중이면 text()는 "김순"까지만 돌려준다. 그래서
// QLineEdit::textChanged만 보고 즉시 검색하면, 조합이 끝나서 커밋되는 시점(다음 글자를
// 치거나 Enter/포커스 이동으로 강제 커밋될 때)까지 마지막 글자가 검색에서 빠진다.
//
// 이 위젯은 inputMethodEvent를 가로채 preedit 문자열까지 합친 "현재 화면에 보이는
// 대로의" 텍스트가 바뀔 때마다 liveTextChanged를 낸다.
// (aurix_main_project_01/medirun_gui/widgets/livesearchlineedit 참고 이식)
class LiveSearchLineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit LiveSearchLineEdit(QWidget* parent = nullptr);

    QString liveText() const;

signals:
    void liveTextChanged(const QString& text);

protected:
    void inputMethodEvent(QInputMethodEvent* event) override;

private:
    QString preeditText_;
};
