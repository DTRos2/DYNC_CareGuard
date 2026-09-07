#pragma once
#include <QListWidget>

// 체크 가능한 항목을 담는 목록 — 체크박스 아이콘을 정확히 클릭하지 않아도 행 어디를
// 클릭하든 체크 상태가 토글된다(체크박스를 정확히 클릭한 경우는 Qt 기본 토글에 맡겨
// 이중 토글을 피한다). AccountDialog·RobotDialog의 "담당 대상자" 목록에서 공통으로 쓴다.
class CheckableListWidget : public QListWidget {
    Q_OBJECT
public:
    explicit CheckableListWidget(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};
