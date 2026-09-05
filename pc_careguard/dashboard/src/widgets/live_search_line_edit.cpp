#include "live_search_line_edit.h"

#include <QInputMethodEvent>

LiveSearchLineEdit::LiveSearchLineEdit(QWidget* parent) : QLineEdit(parent) {
    // 영문 입력·삭제·붙여넣기처럼 IME 조합을 거치지 않는 변경은 여기서 잡는다.
    connect(this, &QLineEdit::textChanged, this, [this] { emit liveTextChanged(liveText()); });
}

QString LiveSearchLineEdit::liveText() const { return text() + preeditText_; }

void LiveSearchLineEdit::inputMethodEvent(QInputMethodEvent* event) {
    // 커밋되기 전에 preedit부터 갱신해야, base 처리 중 textChanged가 함께 튀어도
    // (커밋되는 경우) liveText()가 최신 preedit을 반영한다.
    preeditText_ = event->preeditString();
    QLineEdit::inputMethodEvent(event);
    emit liveTextChanged(liveText());
}
