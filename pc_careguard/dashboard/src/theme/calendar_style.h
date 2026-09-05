// ============================================================
// CareGuard — 날짜 선택 위젯 스타일 적용 헬퍼
// resources/careguard.qss 의 QCalendarWidget 규칙과 짝. QSS로 제어 불가능한
// 항목만 처리한다 (gui/wireframe/v1/calendar_style.h 원본 그대로 이식).
//   · 주말 빨간 글씨 제거 (weekdayTextFormat)
//   · 요일 헤더 "..." 잘림 해결 (최소 폭 / 섹션 리사이즈)
//   · 오늘 날짜 표시, 세로 주차 헤더 제거, 그리드선 제거
//   · 이전/다음 버튼 아이콘 → 텍스트 셰브론 (기본 아이콘은 색 변경 불가)
// ============================================================
#pragma once

#include <QCalendarWidget>
#include <QDateEdit>
#include <QEvent>
#include <QHeaderView>
#include <QLocale>
#include <QPainterPath>
#include <QRegion>
#include <QTableView>
#include <QTextCharFormat>
#include <QToolButton>
#include "tokens.h"

namespace CareGuard {

// 팝업 모서리를 실제 픽셀 단위로 잘라내는 하드 마스크 + 입력창과의 간격 확보.
// QCalendarWidget 자신은 QSS border-radius로 자기 배경만 둥글게 그리고, 그 안의
// 자식 위젯(네비게이션바, 달력 그리드)은 라운드를 모르는 사각형이라 바깥 라운드
// 밖으로 각진 모서리가 삐져나온다. 팝업(top-level) 창 자체를 이 모양대로
// 하드 마스킹하면 내부 위젯이 무엇을 그리든 창 밖 픽셀은 아예 존재하지 않게 되어
// 컴포지터 유무와 무관하게 항상 깔끔하게 잘린다.
// radius: 전체 모달 바깥 4모서리에 동일하게 적용 (careguard.qss의
// QCalendarWidget { border-radius: 8px; } 와 반드시 같은 값을 유지할 것).
class CareGuardRoundedPopupMask : public QObject {
public:
    CareGuardRoundedPopupMask(QWidget* target, int radius, int gapFromAnchor)
        : QObject(target), m_radius(radius), m_gap(gapFromAnchor) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* w = qobject_cast<QWidget*>(watched);
        if (!w) return false;

        if (event->type() == QEvent::Show) {
            // 입력창 바로 아래 틈 없이 붙어 나오던 것을 살짝 띄운다.
            w->move(w->x(), w->y() + m_gap);
        }
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            QPainterPath path;
            path.addRoundedRect(w->rect(), m_radius, m_radius);
            w->setMask(QRegion(path.toFillPolygon().toPolygon()));
        }
        return false;
    }

private:
    int m_radius;
    int m_gap;
};

// QCalendarWidget 단독 사용 시에도 호출 가능
inline void applyCareGuardCalendar(QCalendarWidget* cal)
{
    if (!cal) return;

    // ---- 구조 ----
    cal->setGridVisible(false);
    cal->setNavigationBarVisible(true);
    cal->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);   // 주차 열 제거
    // 영문 요일(ShortDayNames)이 열 폭에 잘려 "..."로 보이는 문제 — 로캘을
    // 한국어로 지정하면 한 글자(월/화/수/목/금/토/일)로 나와 잘릴 일이 없다.
    cal->setLocale(QLocale(QLocale::Korean, QLocale::SouthKorea));
    cal->setHorizontalHeaderFormat(QCalendarWidget::SingleLetterDayNames);
    cal->setFirstDayOfWeek(Qt::Sunday);                                // 국내 관례
    cal->setSelectionMode(QCalendarWidget::SingleSelection);
    cal->setMinimumWidth(300);                                         // 요일 "..." 잘림 방지
    cal->setMinimumHeight(268);

    // ---- 요일 헤더 서식 (QSS 미적용 영역) ----
    QTextCharFormat header;
    header.setForeground(Tokens::TEXT_MUTED);
    header.setFontWeight(QFont::Medium);
    cal->setHeaderTextFormat(header);

    // ---- 주말 색 통일: Qt 기본 빨강/파랑 제거 ----
    // 관제 화면이므로 EMERGENCY 계열 적색을 캘린더에 쓰지 않는다.
    QTextCharFormat weekend;
    weekend.setForeground(Tokens::TEXT_SECONDARY);
    cal->setWeekdayTextFormat(Qt::Saturday, weekend);
    cal->setWeekdayTextFormat(Qt::Sunday,   weekend);

    QTextCharFormat weekday;
    weekday.setForeground(Tokens::TEXT_PRIMARY);
    for (int d = Qt::Monday; d <= Qt::Friday; ++d)
        cal->setWeekdayTextFormat(static_cast<Qt::DayOfWeek>(d), weekday);

    // ---- 오늘 날짜: ACCENT 색 + 볼드 (선택 시엔 흰 글씨로 덮임) ----
    QTextCharFormat today;
    today.setForeground(Tokens::ACCENT);
    today.setFontWeight(QFont::DemiBold);
    cal->setDateTextFormat(QDate::currentDate(), today);

    // ---- 이전/다음 버튼: 기본 아이콘 제거 후 셰브론 텍스트 ----
    if (auto* prev = cal->findChild<QToolButton*>("qt_calendar_prevmonth")) {
        prev->setIcon(QIcon());
        prev->setText(QStringLiteral("‹"));   // ‹
        prev->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    if (auto* next = cal->findChild<QToolButton*>("qt_calendar_nextmonth")) {
        next->setIcon(QIcon());
        next->setText(QStringLiteral("›"));   // ›
        next->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }

    // ---- 그리드 셀 균등 분배 (요일 잘림의 근본 원인) ----
    if (auto* view = cal->findChild<QTableView*>("qt_calendar_calendarview")) {
        view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        view->horizontalHeader()->setMinimumSectionSize(36);
        view->verticalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        view->setShowGrid(false);
        view->setFrameShape(QFrame::NoFrame);
    }
}

// QDateEdit 캘린더 팝업 전체 적용
inline void applyCareGuardDateEdit(QDateEdit* edit)
{
    if (!edit) return;

    edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    edit->setCalendarPopup(true);
    edit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    edit->setMinimumWidth(118);

    auto* cal = new QCalendarWidget(edit);
    applyCareGuardCalendar(cal);
    edit->setCalendarWidget(cal);   // setCalendarWidget 이후 팝업 부모가 생성됨

    // 팝업(top-level) 윈도우 배경은 QCalendarWidget의 QSS border-radius와 무관하게
    // 그대로 사각형이라, 라운드 바깥 네 모서리에 배경이 비쳐 보인다. 팝업을 투명
    // 배경으로 만들고, 거기에 더해 실제 QCalendarWidget과 동일한 반경(8px)의 하드
    // 마스크를 씌워 컴포지터 유무와 무관하게 모서리 바깥이 항상 완전히 잘리도록 한다.
    // 입력창에 딱 붙어 나오던 것도 살짝 띄운다.
    // (블러 드롭섀도우는 쓰지 않는다 — 컴포지터 없는 환경에서는 반투명 픽셀이
    // 알파 블렌딩되지 않아 얼룩으로 보이고, 하드 마스크 안에서는 어차피 잘려나간다.
    // "떠 있는" 느낌은 QSS의 불투명 1px 테두리(border: 1px solid #CDD5DD)로 충분하다.)
    if (QWidget* popup = cal->parentWidget()) {
        popup->setAttribute(Qt::WA_TranslucentBackground);
        popup->setAttribute(Qt::WA_NoSystemBackground);
        popup->installEventFilter(new CareGuardRoundedPopupMask(popup, /*radius=*/8, /*gapFromAnchor=*/6));
    }
}

} // namespace CareGuard
