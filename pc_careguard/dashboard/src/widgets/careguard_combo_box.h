#pragma once
#include <QComboBox>

// QComboBox::down-arrow 서브컨트롤은 ::drop-down을 QSS로 커스터마이즈하는 순간(careguard.qss의
// `QComboBox::drop-down { border: none; width: 22px; }`) image 지정 없이는 빈 상자로 렌더링되는
// Qt 스타일시트 특성이 있다(CareGuardDateEdit과 같은 원인 — feedback_qt_dropdown_arrow_qss 참고).
// IconLineEdit·CareGuardDateEdit과 동일하게 paintEvent에서 아이콘을 직접 그려 우회한다.
//
// 드롭다운 팝업도 두 가지를 손본다(showPopup() 참고):
//  1) radius가 보이려면 목록(QAbstractItemView)이 아니라 팝업 컨테이너
//     (QComboBoxPrivateContainer) 쪽에 배경·테두리·radius를 줘야 한다. 컨테이너의 실제
//     창 모양은 여전히 각진 사각형이라 라운드 바깥 네 모서리는 setMask()로 잘라낸다 —
//     WA_TranslucentBackground는 컴포지터 없는 환경에서 완전 불투명 영역은 멀쩡해도
//     테두리처럼 안티앨리어싱되는 픽셀이 검게 나와서(gui/issue/target10.png) 안 쓴다.
//  2) 일반(비편집형) QComboBox는 Qt/Fusion 스타일이 "현재 선택된 항목을 트리거 박스
//     위치에 맞추는" 팝업 배치를 쓰기 때문에 팝업이 트리거 박스 안쪽에 겹쳐서 열린다
//     (gui/issue/target9.png). 복약 시각 콤보(편집 가능)처럼 항상 트리거 바로 아래에
//     열리도록 팝업 위치를 직접 재계산한다.
class CareGuardComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit CareGuardComboBox(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showPopup() override;
};
