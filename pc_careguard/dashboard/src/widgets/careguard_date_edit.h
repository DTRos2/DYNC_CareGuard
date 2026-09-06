#pragma once
#include <QDate>
#include <QDateEdit>

// QDateEdit::down-arrow 서브컨트롤은 ::drop-down을 QSS로 커스터마이즈하는 순간 image 지정 없이는
// 빈 상자로 렌더링되는 Qt 스타일시트 특성이 있다(캘린더 팝업 진입 아이콘이 안 보이던 원인).
// IconLineEdit과 동일하게 paintEvent에서 아이콘을 직접 그려 우회한다.
class CareGuardDateEdit : public QDateEdit {
    Q_OBJECT
public:
    explicit CareGuardDateEdit(const QDate& date, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};
