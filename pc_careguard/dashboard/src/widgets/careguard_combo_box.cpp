#include "careguard_combo_box.h"

#include <QAbstractItemView>
#include <QFrame>
#include <QLayout>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>

#include "theme/calendar_style.h"
#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {
constexpr int kPopupRadius = 8;  // careguard.qss의 다른 팝업류(QCalendarWidget 등)와 맞춘 값
}

CareGuardComboBox::CareGuardComboBox(QWidget* parent) : QComboBox(parent) {}

void CareGuardComboBox::showPopup() {
    QComboBox::showPopup();
    // 팝업 컨테이너는 showPopup() 안에서(첫 호출 시) 지연 생성되므로, 그 실체가 만들어진
    // 지금에야 view()->window()로 잡을 수 있다 — 생성자에서 미리 해두는 건 불가능하다.
    QWidget* popupWindow = view() ? view()->window() : nullptr;
    if (!popupWindow) return;

    // 일반(비편집형) 콤보박스는 Qt/Fusion 스타일이 "현재 선택 항목을 트리거 박스 위치에
    // 맞추는" 팝업 배치를 쓴다 — 그래서 로봇·맵 배정 콤보의 팝업이 트리거 박스 안쪽에
    // 겹쳐서 열렸다(gui/issue/target9.png). 편집 가능한 복약 시각 콤보는 이 배치를 안 써서
    // 항상 트리거 바로 아래에 열린다(target8.png) — 그 모양으로 강제로 맞춘다.
    popupWindow->move(mapToGlobal(QPoint(0, height())));

    // radius가 실제로 보이려면 목록(QAbstractItemView)이 아니라 팝업 실체인 컨테이너
    // (QComboBoxPrivateContainer) 쪽에 배경·테두리·radius를 줘야 한다 — 목록에 불투명
    // 배경을 칠하면 그 각진 사각형이 컨테이너가 그린 둥근 모서리를 그대로 덮어버리기
    // 때문이다(careguard.qss의 QComboBox QAbstractItemView 규칙이 그래서 배경을 투명으로
    // 비워둠). 비공개 클래스 이름에 기대는 QSS 타입 선택자 대신, 여기서 인라인
    // setStyleSheet()로 직접 걸어 Qt 버전 변화에 덜 취약하게 한다.
    if (!popupWindow->property("cgPopupStyled").toBool()) {
        popupWindow->setStyleSheet(QStringLiteral("background-color: #FFFFFF; "
                                                    "border: 1px solid #CDD5DD; "
                                                    "border-radius: %1px;")
                                        .arg(kPopupRadius));
        // 컨테이너 자신의 프레임 폭(PM_MenuPanelWidth)·레이아웃 여백을 0으로 없애 컨테이너
        // 크기 == 목록 크기로 맞춘다 — 남는 띠가 없어야 라운드 배경이 창을 정확히 덮는다.
        if (auto* frame = qobject_cast<QFrame*>(popupWindow)) {
            frame->setFrameShape(QFrame::NoFrame);
            frame->setLineWidth(0);
            frame->setMidLineWidth(0);
        }
        if (QLayout* layout = popupWindow->layout()) {
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);
        }
        popupWindow->setContentsMargins(0, 0, 0, 0);

        // 컨테이너 배경을 흰색으로 칠해도 창 자체의 실제 모양은 여전히 각진 사각형이라,
        // 라운드 바깥 네 모서리를 지우려면 setMask()로 창 모양 자체를 잘라내야 한다.
        // WA_TranslucentBackground(+WA_NoSystemBackground)도 같이 써봤는데, 컴포지터가 없는
        // 환경(이 샌드박스 포함)에서는 완전 불투명(alpha=255) 영역은 멀쩡해도 테두리처럼
        // 안티앨리어싱되는(부분 투명) 픽셀이 전부 검게 나왔다(gui/issue/target10.png) —
        // 합성 없이는 알파를 제대로 못 다루는 것. setMask()는 X11 Shape 확장 기반이라 알파와
        // 완전히 무관하게(불투명 창을 그냥 그 모양대로 오려내는 것) 항상 동작하므로, 투명
        // 속성 없이 마스크만 쓴다. CareGuardRoundedPopupMask 이벤트 필터가 Show·Resize
        // 때마다 마스크를 다시 씌운다(목록 항목 수에 따라 팝업 크기가 늦게 바뀌어도 대응).
        popupWindow->installEventFilter(
            new CareGuard::CareGuardRoundedPopupMask(popupWindow, /*radius=*/kPopupRadius, /*gapFromAnchor=*/0));
        popupWindow->setProperty("cgPopupStyled", true);
    }

    // 필터 설치가 이번 showPopup() 안에서 처음 이뤄졌다면 이미 지나간 첫 Show 이벤트는
    // 못 받으므로, 지금 이 순간 것도 한 번 더 직접 씌워 첫 오픈부터 비어있지 않게 한다.
    QPainterPath path;
    path.addRoundedRect(popupWindow->rect(), kPopupRadius, kPopupRadius);
    popupWindow->setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void CareGuardComboBox::paintEvent(QPaintEvent* event) {
    QComboBox::paintEvent(event);

    const QColor color = isEnabled() ? Tokens::TEXT_SECONDARY : Tokens::BORDER_STRONG;
    constexpr int kIconSize = 10;
    constexpr int kDropDownWidth = 22;  // careguard.qss QComboBox::drop-down width와 동일
    const QRect iconRect(width() - kDropDownWidth + (kDropDownWidth - kIconSize) / 2,
                          (height() - kIconSize) / 2, kIconSize, kIconSize);

    QPainter painter(this);
    SimpleIcons::chevronDown(color, kIconSize).paint(&painter, iconRect);
}
