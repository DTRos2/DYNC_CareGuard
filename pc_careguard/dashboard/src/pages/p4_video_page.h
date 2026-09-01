#pragma once
#include <QString>
#include <QVector>
#include <QWidget>

// P4 영상 관리 — 담당 팀원이 이 페이지의 메인 뷰를 구현한다.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 3), §7 참고.

// 카드 좌상단 태그 스타일. EMERGENCY/ATTENTION은 Tokens::Level과 동일한 색 규칙을 쓰지만,
// "순찰"·"수동 열람"은 상태 판정(§7-1)과 무관한 중립 태그라 별도 값으로 둔다.
enum class VideoTagStyle { Emergency, Attention, Neutral };

// TODO(team): DB 스키마(pc_careguard/db/schema)와 model/VideoModel이 생기면
// 이 struct와 아래 mock 데이터는 그쪽으로 옮기고 여기서는 삭제한다.
struct VideoClip {
    QString robotId;        // "R-01"
    QString recipientName;  // "김순자"
    QString eventLabel;     // "심박 이상" (카드 제목에 이어붙는 설명)
    QString tagText;        // "긴급" / "주의" / "순찰" / "수동 열람"
    VideoTagStyle tagStyle;
    QString capturedAt;     // "08.26 14:29" (§10: MM.DD HH:MM)
    QString eventId;        // "EV-260826-014" 또는 "—" (이벤트 미연동)
    QString durationLabel;  // "02:14" (§10: MM:SS)
    int viewCount = 0;      // 0 = "열람 없음"
    int expireDays = 0;     // 만료 D-N (§10: D-7 이하 ATTENTION 색)
};

// 와이어프레임(p4_video_management_wireframe) 그대로의 mock 데이터.
// 실 연동 전까지 GUI 검증용으로만 사용한다.
QVector<VideoClip> mockVideoClips();

class P4VideoPage : public QWidget {
    Q_OBJECT
public:
    explicit P4VideoPage(QWidget* parent = nullptr);
};
