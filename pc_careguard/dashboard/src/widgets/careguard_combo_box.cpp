#include "careguard_combo_box.h"

#include <QAbstractItemView>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QRegion>
#include <QStyleOptionComboBox>

#include "theme/calendar_style.h"
#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {
constexpr int kPopupRadius = 8;  // careguard.qss의 다른 팝업류(QCalendarWidget 등)와 맞춘 값

// SH_ComboBox_Popup이 참이면 비편집형 콤보의 팝업이 "메뉴형"으로 열린다 — 이 모드에서
// QComboBoxPrivateContainer::paintEvent()가 우리가 인라인 QSS로 준 흰 라운드 배경 위에
// PE_PanelMenu(각진 네이티브 메뉴 패널)를 한 겹 더 그려서 "라운드 박스를 감싼 각진 박스"로
// 보였다(gui/issue: 로봇·맵 배정 콤보에서만 재현, 편집형인 복약 시각 콤보는 이 경로를 안
// 타서 멀쩡했음). 이 힌트를 항상 거짓으로 답해 편집형과 같은 일반 리스트 팝업 경로를
// 타게 만든다 — 컨테이너가 한 겹만 그려지므로 라운드 배경이 그대로 유지된다.
class NoMenuPopupStyle : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption* opt, const QWidget* w,
                  QStyleHintReturn* ret) const override {
        if (hint == QStyle::SH_ComboBox_Popup) return 0;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};
}  // namespace

CareGuardComboBox::CareGuardComboBox(QWidget* parent) : QComboBox(parent) {
    auto* style = new NoMenuPopupStyle;
    style->setParent(this);  // 위젯과 수명을 묶는다
    setStyle(style);
}

void CareGuardComboBox::showPopup() {
    // view()->window()는 QComboBox::showPopup()을 부르기 전에도 이미 실제 팝업 컨테이너
    // (QComboBoxPrivateContainer)를 반환한다 — view() 호출 자체가 컨테이너를 지연 생성시키기
    // 때문이다(showPopup() 안에서만 생성된다는 건 사실이 아니었음, 직접 계측해서 확인함).
    // 그래서 테두리 스타일시트를 QComboBox::showPopup() *이전에* 걸 수 있다 — 이게 중요한
    // 이유: QComboBox::showPopup()은 이 함수가 호출된 시점의 컨테이너 상태(스타일시트 포함)를
    // 기준으로 팝업 크기를 계산한다. 스타일시트를 나중에(호출 후) 걸면, 처음 열 때는
    // "테두리 없는 크기"로 이미 계산·표시된 뒤에야 1px 테두리가 붙어 리스트 뷰포트가 사후에
    // 살짝 줄어들고, 그 결과 마지막 항목이 미세하게 잘려 스크롤바가 "빈 옵션이 하나 더
    // 있는 것처럼" 한 칸 더 여유를 갖게 됐다. 두 번째로 열 때부터 멀쩡했던 이유도 이거다 —
    // cgPopupStyled 가드 때문에 스타일시트는 최초 1회만 적용되고 그 뒤로도 계속 컨테이너에
    // 남아있으므로, 두 번째 open부터는 showPopup()이 크기를 계산하는 시점에 이미 테두리가
    // 반영돼 있어 정확히 맞아떨어졌던 것. 스타일시트 적용을 showPopup() 호출보다 앞으로
    // 옮기면 최초 open부터 같은 결과가 나온다.
    QWidget* popupWindow = view() ? view()->window() : nullptr;

    // radius가 실제로 보이려면 목록(QAbstractItemView)이 아니라 팝업 실체인 컨테이너
    // (QComboBoxPrivateContainer) 쪽에 배경·테두리·radius를 줘야 한다 — 목록에 불투명
    // 배경을 칠하면 그 각진 사각형이 컨테이너가 그린 둥근 모서리를 그대로 덮어버리기
    // 때문이다(careguard.qss의 QComboBox QAbstractItemView 규칙이 그래서 배경을 투명으로
    // 비워둠). 비공개 클래스 이름에 기대는 QSS 타입 선택자 대신, 여기서 인라인
    // setStyleSheet()로 직접 걸어 Qt 버전 변화에 덜 취약하게 한다.
    if (popupWindow && !popupWindow->property("cgPopupStyled").toBool()) {
        popupWindow->setStyleSheet(QStringLiteral("background-color: #FFFFFF; "
                                                    "border: 1px solid #CDD5DD; "
                                                    "border-radius: %1px;")
                                        .arg(kPopupRadius));
        // 예전엔 여기서 컨테이너의 프레임 폭(PM_MenuPanelWidth)·레이아웃 여백을 0으로
        // 없앴는데, 그건 "메뉴형" 팝업일 때만 붙는 여백이었다 — NoMenuPopupStyle로 그
        // 경로 자체를 껐으니 이제 그 여백도 안 생긴다. 오히려 Qt가 "메뉴형 여백 포함"
        // 크기로 팝업 창을 먼저 만든 *뒤에* 여백을 0으로 지우면, 창 크기는 그대로인 채
        // 내용만 줄어들어 아래쪽에 빈 공간이 남았다 — 항목이 몇 개 안 되는 콤보(성별
        // 등)에서 그 빈 공간 때문에 불필요한 스크롤바가 뜨거나 "빈 옵션이 하나 더 있는
        // 것처럼" 스크롤이 과하게 내려가던 원인이었다. 그래서 이 블록은 통째로 뺀다.

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

    // 테두리 스타일시트가 걸린 뒤에야 크기를 계산하도록, 실제 팝업을 여는 건 항상 이
    // 시점 이후다.
    QComboBox::showPopup();
    if (!popupWindow) return;

    // 일반(비편집형) 콤보박스는 원래 "현재 선택 항목을 트리거 박스 위치에 맞추는" 메뉴형
    // 팝업 배치를 써서 트리거 안쪽에 겹쳐 열렸다(gui/issue/target9.png) — 위 NoMenuPopupStyle로
    // 그 메뉴형 경로 자체를 껐으니 이제 Qt가 기본으로도 트리거 바로 아래에 열어줄 가능성이
    // 높다. 그래도 위치를 명시적으로 한 번 더 강제해 어떤 스타일 조합에서도 항상 같은
    // 자리(트리거 바로 아래)에 열리도록 보장한다 — 이미 맞는 위치면 그대로 두는 것과
    // 다르지 않아 부작용이 없다.
    popupWindow->move(mapToGlobal(QPoint(0, height())));

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
