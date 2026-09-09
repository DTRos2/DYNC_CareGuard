#pragma once
#include <QComboBox>

// QComboBox::down-arrow 서브컨트롤은 ::drop-down을 QSS로 커스터마이즈하는 순간(careguard.qss의
// `QComboBox::drop-down { border: none; width: 22px; }`) image 지정 없이는 빈 상자로 렌더링되는
// Qt 스타일시트 특성이 있다(CareGuardDateEdit과 같은 원인 — feedback_qt_dropdown_arrow_qss 참고).
// IconLineEdit·CareGuardDateEdit과 동일하게 paintEvent에서 아이콘을 직접 그려 우회한다.
//
// 드롭다운 팝업도 세 가지를 손본다:
//  1) (근본 원인) 비편집형 QComboBox는 SH_ComboBox_Popup 스타일 힌트가 참이라 "메뉴형"
//     팝업 경로를 탄다 — 이 경로에서 QComboBoxPrivateContainer::paintEvent()가 우리가 준
//     흰 라운드 배경 위에 PE_PanelMenu(각진 네이티브 메뉴 패널)를 한 겹 더 그리고, 트리거
//     위치에 팝업을 겹쳐 여는 배치까지 같이 딸려온다(로봇·맵 배정 콤보에서만 재현됐던
//     "박스 안에 각진 박스"·겹쳐 열리는 문제 — gui/issue/target9·10·13.png). 편집 가능한
//     복약 시각 콤보는 이 경로를 안 타서 애초에 문제가 없었다. .cpp의 NoMenuPopupStyle이
//     이 힌트를 항상 거짓으로 답해 모든 CareGuardComboBox가 편집형과 같은 일반 리스트
//     팝업 경로를 타게 만든다 — 근본 원인을 없앤 것.
//  2) radius가 보이려면 목록(QAbstractItemView)이 아니라 팝업 컨테이너
//     (QComboBoxPrivateContainer) 쪽에 배경·테두리·radius를 줘야 한다. 컨테이너의 실제
//     창 모양은 여전히 각진 사각형이라 라운드 바깥 네 모서리는 setMask()로 잘라낸다 —
//     WA_TranslucentBackground는 컴포지터 없는 환경에서 완전 불투명 영역은 멀쩡해도
//     테두리처럼 안티앨리어싱되는 픽셀이 검게 나와서(gui/issue/target10.png) 안 쓴다.
//  3) showPopup()에서 팝업 위치를 트리거 바로 아래로 한 번 더 명시적으로 맞춘다 — 1)로
//     메뉴형 배치 자체는 이미 꺼졌지만, 어떤 스타일 조합에서도 항상 같은 자리에 열리도록
//     보장하는 안전장치로 남겨둔다.
class CareGuardComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit CareGuardComboBox(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showPopup() override;
};
