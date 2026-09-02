#pragma once
#include <QString>
#include <QStringList>
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

    // ---- 재생 모달(p4_video_playback_modal_wireframe) 전용 ----
    int heartRateBpm = 0;      // 연결 이벤트 카드용. 0이면 심박 관련 이벤트가 아니라 표시 안 함
    QString location;          // "행복빌라 302 거실" 등. eventId가 있을 때만 사용
    QString deviationStart;    // 타임라인 차트 "이탈 시작 HH:MM" 라벨. 비어있으면 정상 구간만 표시
    QStringList viewHistory;   // "HH:MM:SS · 사유: ..." 최신순. 재생할 때마다 맨 앞에 추가된다
};

// 와이어프레임(p4_video_management_wireframe) 그대로의 mock 데이터.
// 실 연동 전까지 GUI 검증용으로만 사용한다.
QVector<VideoClip> mockVideoClips();

class QGridLayout;
class QComboBox;
class QResizeEvent;

class P4VideoPage : public QWidget {
    Q_OBJECT
public:
    explicit P4VideoPage(QWidget* parent = nullptr);

    // 로그인 사용자 이름. MainWindow::setUserName()에서 로그인 성공 시 내려준다.
    // §7 열람 이력에 "누가 열람했는지" 기록할 때 이 값을 쓴다.
    void setUserName(const QString& name);

protected:
    // 창 크기가 바뀔 때마다 카드 그리드 열 수를 다시 계산해 반응형으로 재배치한다.
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildCardGrid();
    void openVideoPlayer(int clipIndex);
    bool matchesFilters(const VideoClip& clip) const;
    int computeColumnCount() const;

    // mock 상태 보관용. model/VideoModel이 생기면 이 멤버들은 그쪽으로 옮긴다 (VideoClip 주석 참고).
    QVector<VideoClip> clips_;
    QWidget* gridPage_ = nullptr;
    QGridLayout* cardGrid_ = nullptr;
    int columns_ = 3;  // resizeEvent()가 창 폭에 맞춰 갱신 — 초기값은 첫 rebuild 전 임시값
    QString currentUserName_ = QStringLiteral("담당자");  // setUserName() 호출 전 fallback

    // 필터 바 콤보박스. currentIndexChanged 시 rebuildCardGrid()가 이 값들을 읽어 다시 걸러낸다.
    QComboBox* robotFilter_ = nullptr;
    QComboBox* recipientFilter_ = nullptr;
    QComboBox* periodFilter_ = nullptr;  // TODO(team): mock엔 "오늘" 기준이 없어 아직 미적용
    QComboBox* typeFilter_ = nullptr;
};
