#pragma once
#include <QIcon>
#include <QLineEdit>

// QLineEdit::addAction()의 좌/우 아이콘 슬롯 자동 계산이 필드 하나에 아이콘 2개(좌측 잠금 +
// 우측 눈 토글)를 동시에 넣었을 때 잘려 보이는 문제가 있어(로그인 화면 비밀번호 필드), 직접
// paintEvent에서 그리는 방식으로 대체한 QLineEdit. 위치를 전적으로 통제하므로 잘림이 없다.
class IconLineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit IconLineEdit(QWidget* parent = nullptr);

    void setLeadingIcon(const QIcon& icon);
    void setTrailingIcon(const QIcon& icon);  // 클릭 가능 (trailingIconClicked 발생)

signals:
    void trailingIconClicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void updateTextMargins();
    QRect leadingRect() const;
    QRect trailingRect() const;

    static constexpr int kIconSize = 14;
    static constexpr int kInset = 8;

    QIcon leadingIcon_;
    QIcon trailingIcon_;
};
