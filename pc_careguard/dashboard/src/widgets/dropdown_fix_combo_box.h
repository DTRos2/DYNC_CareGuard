#pragma once
#include <QComboBox>
#include <QPoint>

// 이 앱에서는 QComboBox가 팝업 위치를 잘못 계산해서(프레임리스 다이얼로그뿐 아니라
// MainWindow에 바로 얹히는 일반 페이지에서도 재현됨 — 원인 특정 전이라 조건 없이 항상
// 적용한다) 콤보 박스 바로 아래가 아니라 창 중간·위쪽에 뜬다. 팝업을 띄운 직후 콤보
// 박스 바로 아래로 직접 옮겨서 고정한다.
class DropDownFixComboBox : public QComboBox {
public:
    using QComboBox::QComboBox;
    void showPopup() override {
        QComboBox::showPopup();
        view()->window()->move(mapToGlobal(QPoint(0, height())));
    }
};
