// ============================================================
// CareGuard 디자인 토큰 — 단일 정의 (Qt C++)
// 기준 문서: CareGuard_DESIGN_SYSTEM.md §1~3
// QSS 밖의 모든 색상·크기는 이 헤더의 상수만 사용한다.
// 값 변경 시 careguard.qss 및 문서와 반드시 동기화할 것.
// ============================================================
#pragma once
#include <QColor>
#include <QString>

namespace Tokens {

// ---------- 1-1. 베이스 ----------
inline const QColor BG_BASE       {"#F5F7F9"};
inline const QColor BG_SURFACE    {"#FFFFFF"};
inline const QColor BG_SUNKEN     {"#EDF1F4"};
inline const QColor BG_HOVER      {"#F0F4F7"};
inline const QColor BORDER        {"#E3E8ED"};
inline const QColor BORDER_STRONG {"#CDD5DD"};

// ---------- 1-2. 텍스트 ----------
inline const QColor TEXT_PRIMARY  {"#16202B"};
inline const QColor TEXT_SECONDARY{"#5A6774"};
inline const QColor TEXT_MUTED    {"#93A0AC"};
inline const QColor TEXT_INVERSE  {"#FFFFFF"};

// ---------- 1-3. 브랜드 액센트 ----------
inline const QColor ACCENT            {"#0E9384"};
inline const QColor ACCENT_HOVER      {"#0B7A6E"};
inline const QColor ACCENT_SOFT       {"#E0F5F2"};
inline const QColor ACCENT_SOFT_BORDER{"#B9E2DB"};
// ACCENT_BRIGHT는 로봇 LCD 전용 — 대시보드 코드에서 사용 금지 (§11-2)
inline const QColor LCD_ONLY_ACCENT_BRIGHT{"#2DD4BF"};

// ---------- 1-4. 상태 색 ----------
struct StatusColors { QColor main; QColor soft; QColor border; };
inline const StatusColors EMERGENCY {{"#D93B47"}, {"#FDECEE"}, {"#F5C2C7"}};
inline const StatusColors ATTENTION {{"#C27500"}, {"#FFF6E5"}, {"#F2DCAE"}};
inline const StatusColors OK        {{"#16875A"}, {"#E8F6EF"}, {"#BCE3D0"}};
inline const StatusColors INFO      {{"#2563A8"}, {"#E9F2FB"}, {"#C2D9F0"}};
inline const StatusColors OFFLINE   {{"#8A97A3"}, {"#F1F4F6"}, {"#DFE5EA"}};

// EMERGENCY 펄스 (배경 2초 주기 교차)
inline const QColor EMERGENCY_PULSE_A{"#FDECEE"};
inline const QColor EMERGENCY_PULSE_B{"#FBDDE0"};

// 상태 우선순위: EMERGENCY > ATTENTION > OFFLINE > NORMAL (문서 §1-4)
enum class Level { Normal, Offline, Attention, Emergency };

// 동적 프로퍼티 값 문자열 (QSS 셀렉터와 1:1 — 오타 방지용)
namespace Prop {
    inline const char* STATUS      = "status";     // recipientCard
    inline const char* LEVEL       = "level";      // badge, kpiCard, banner, progressbar
    inline const char* VARIANT     = "variant";    // button: primary/secondary/danger/ghost
    inline const char* SIZE        = "size";       // button: s/l
    inline const char* NORMAL      = "normal";
    inline const char* ATTENTION   = "attention";
    inline const char* EMERGENCY   = "emergency";
    inline const char* OFFLINE     = "offline";
    inline const char* OK          = "ok";
    inline const char* INFO        = "info";
}

// ---------- 1-5. 그림자 (QGraphicsDropShadowEffect) ----------
struct ShadowSpec { int blur; int dy; QColor color; };
inline const ShadowSpec SHADOW_SM {  8,  1, QColor(22, 32, 43,  15) };  // 0.06
inline const ShadowSpec SHADOW_MD { 24,  4, QColor(22, 32, 43,  26) };  // 0.10
inline const ShadowSpec SHADOW_LG { 48, 12, QColor(22, 32, 43,  41) };  // 0.16
inline const QColor MODAL_BACKDROP  { 22, 32, 43, 115 };                // 0.45
inline const QColor VIDEO_LOCK_BG   { 22, 32, 43, 153 };                // 0.60

// ---------- 2. 타이포그래피 ----------
namespace Font {
    inline const QString SANS = "Pretendard";       // fallback: Noto Sans KR, 맑은 고딕
    inline const QString MONO = "JetBrains Mono";   // fallback: D2Coding, Consolas
    // px 크기 (문서 §2)
    constexpr int DISPLAY = 32;  // weight 700 — 심박 대형 숫자
    constexpr int H1      = 24;  // 600
    constexpr int H2      = 18;  // 600
    constexpr int H3      = 15;  // 600
    constexpr int BODY    = 14;  // 400
    constexpr int BODY_S  = 13;  // 400
    constexpr int LABEL   = 12;  // 500, 자간 +0.3
    constexpr int CAPTION = 11;  // 400
    constexpr int MONO_SZ = 13;  // 400
}

// ---------- 3. 레이아웃 상수 ----------
namespace Layout {
    constexpr int SIDEBAR_W          = 240;
    constexpr int SIDEBAR_W_COLLAPSED= 64;
    constexpr int TOPBAR_H           = 48;
    constexpr int PAGE_PADDING       = 24;
    constexpr int CARD_PADDING       = 16;
    constexpr int CARD_GAP           = 16;
    constexpr int TABLE_ROW_H        = 48;
    constexpr int TABLE_ROW_H_COMPACT= 40;
    constexpr int STATUS_BAR_W       = 4;    // 테이블 행 좌측 상태 바 폭 (델리게이트)
    constexpr int MODAL_W_S          = 480;
    constexpr int MODAL_W_M          = 720;
    constexpr int MODAL_W_L          = 1040;
    constexpr int RADIUS_SM          = 4;
    constexpr int RADIUS_MD          = 8;
    constexpr int RADIUS_LG          = 12;
    constexpr int MIN_W              = 1440;
    constexpr int MIN_H              = 900;
    constexpr int SPARKLINE_W        = 120;
    constexpr int SPARKLINE_H        = 32;
}

// ---------- 9. 모션 ----------
namespace Motion {
    constexpr int MODAL_FADE_MS   = 200;
    constexpr int PULSE_PERIOD_MS = 2000;  // EMERGENCY 배경 교차 주기
    constexpr int FLASH_MS        = 300;   // 데이터 갱신 플래시
}

// ---------- 10. 표기 규칙 도우미 ----------
namespace Format {
    inline const QString EMPTY = QStringLiteral("\u2014");  // "—" (TEXT_MUTED로 표시)
}

// ---------- 12. 로봇 LCD 전용 (대시보드 사용 금지) ----------
namespace Lcd {
    inline const QColor BG       {"#0F1419"};
    inline const QColor SURFACE  {"#161C24"};
    inline const QColor TEXT     {"#E8EDF2"};
    inline const QColor TEXT_SUB {"#9AA7B4"};
    inline const QColor ACCENT   {"#2DD4BF"};
    inline const QColor DANGER   {"#F4525C"};
    inline const QColor WARN     {"#F0A73A"};
}

} // namespace Tokens
