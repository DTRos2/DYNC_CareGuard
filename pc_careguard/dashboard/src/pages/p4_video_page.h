#pragma once
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QNetworkAccessManager;
class QNetworkReply;

// P4 영상 관리 — 담당 팀원이 이 페이지의 메인 뷰를 구현한다.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 3), §7 참고.

// 카드 좌상단 태그 스타일. EMERGENCY/ATTENTION은 Tokens::Level과 동일한 색 규칙을 쓰지만,
// "순찰"은 상태 판정(§7-1)과 무관한 중립 태그라 별도 값으로 둔다.
enum class VideoTagStyle { Emergency, Attention, Neutral };

// TODO(team): DB 스키마(pc_careguard/db/schema)와 model/VideoModel이 생기면
// 이 struct와 아래 mock 데이터는 그쪽으로 옮기고 여기서는 삭제한다.
struct VideoClip {
    QString robotId;        // "R-01"
    QString recipientName;  // "김순자"
    QString eventLabel;     // "SOS 호출" (카드 제목에 이어붙는 설명)
    QString tagText;        // "긴급" / "주의" / "순찰"
    VideoTagStyle tagStyle;
    QString capturedAt;     // "08.26 14:29" (§10: MM.DD HH:MM)
    QString eventId;        // "EV-260826-014" 또는 "—" (이벤트 미연동)
    QString durationLabel;  // "02:14" (§10: MM:SS)
    int viewCount = 0;      // 0 = "열람 없음"
    int expireDays = 0;     // 만료 D-N (§10: D-7 이하 ATTENTION 색)

    // ---- 재생 모달(p4_video_playback_modal_wireframe) 전용 ----
    QString location;          // "행복빌라 302 거실" 등. eventId가 있을 때만 사용
    QStringList viewHistory;   // "HH:MM:SS · 사유: ..." 최신순. 재생할 때마다 맨 앞에 추가된다
    QString videoUrl;          // 재생용 절대 URL (serverUrl + GET /video-events의 video_url)

    // mockVideoClips()의 항목들은 이 필드를 안 채워도 된다 — 기본값 0 그대로 두면
    // recordVideoView()가 "서버에서 온 클립이 아님"으로 보고 API 호출을 건너뛴다.
    int cameraRecordingId = 0;  // camera_recording.id. POST /video-events/{id}/view 호출에 사용

    // capturedAt은 이미 화면 표시용으로 포맷된 문자열이라 기간 필터 계산엔 못 써서 원본을 따로 든다.
    // 기본값(invalid QDateTime)이면 matchesFilters()가 기간 필터 선택 시 이 클립을 제외한다.
    QDateTime capturedAtRaw;
};

// 와이어프레임(p4_video_management_wireframe) 그대로의 mock 데이터.
// 실 연동 전까지 GUI 검증용으로만 사용한다.
QVector<VideoClip> mockVideoClips();

class QGridLayout;
class QPushButton;
class QResizeEvent;
class QTableWidget;
class QJsonObject;
class WsClient;
class LiveSearchLineEdit;
class CareGuardDateEdit;

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
    // 읽기 전용 날짜 칸 클릭을 "직접"로 전환한다 (P2EventPage::eventFilter,
    // P7SettingsPage::eventFilter와 동일한 패턴 — applyPeriodPreset() 참고).
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildCardGrid();
    // 그리드 뷰(rebuildCardGrid())와 같은 clips_/matchesFilters()를 표로 다시 그린다.
    // rebuildCardGrid() 끝에서 항상 같이 호출되어, 필터 변경·목록 새로고침 시 두 뷰가 같이 갱신된다.
    void rebuildTableView();
    void openVideoPlayer(int clipIndex);
    bool matchesFilters(const VideoClip& clip) const;
    int computeColumnCount() const;

    // 서버 GET /video-events 호출해 clips_를 채운 뒤 rebuildCardGrid()로 다시 그린다.
    // 실패해도(네트워크 에러 등) clips_를 비운 채로 두고 조용히 넘어간다 — §4 카드 그리드는
    // 비어 있으면 makeEmptySlot() 안내만 보여주므로 별도 에러 배너는 아직 안 만든다.
    void fetchVideoClips();
    void handleVideoClipsReply(QNetworkReply* reply);

    // /ws/dashboard가 push하는 "video_event_created"(새 영상 클립 저장 완료)를 받아
    // fetchVideoClips()를 다시 호출한다 — P3/P5/P6와 같은 패턴(페이지마다 자기 WsClient를
    // 따로 둠). 목록 전체를 다시 세는 방식(list_video_events)과 맞춰, 부분 diff 대신
    // 항상 서버에서 다시 받아 그린다.
    void handleWsMessage(const QJsonObject& message);

    // 열람 사유 확인 후 재생이 시작되면(unlock()) 호출한다. 서버 POST
    // /video-events/{id}/view가 성공하면 응답의 view_count/view_history로 clips_[clipIndex]를
    // 갱신하고 다시 그린다 — 로컬에서 임의로 +1 하지 않고 항상 서버가 다시 센 값을 그대로 쓴다.
    void recordVideoView(int clipIndex, const QString& reason);

    // 기간 세그먼트(오늘/7일/30일/직접) 클릭 시 날짜 범위를 맞춘다. spanDays >= 0이면
    // "오늘 - spanDays일 ~ 오늘"로 채우고 읽기 전용으로 잠근다. spanDays < 0("직접")이면
    // 편집 가능하게 풀어준다. P7SettingsPage::applyAuditPeriodPreset()과 동일한 패턴.
    void applyPeriodPreset(int spanDays);
    void setDateEditsReadOnly(bool readOnly);

    // mock 상태 보관용. model/VideoModel이 생기면 이 멤버들은 그쪽으로 옮긴다 (VideoClip 주석 참고).
    QVector<VideoClip> clips_;
    QWidget* gridPage_ = nullptr;
    QGridLayout* cardGrid_ = nullptr;
    QTableWidget* tableView_ = nullptr;  // 필터바 "테이블" 토글로 전환되는 뷰. rebuildTableView()가 채운다
    int columns_ = 3;  // resizeEvent()가 창 폭에 맞춰 갱신 — 초기값은 첫 rebuild 전 임시값
    QString currentUserName_ = QStringLiteral("담당자");  // setUserName() 호출 전 fallback

    // 필터 바 — P7(시스템 설정) "로그" 탭과 같은 구성(기간 세그먼트 + 날짜 범위 + 검색
    // 하나). 로봇 ID·대상자 이름·유형(긴급/주의/순찰)은 콤보박스로 하나하나 고르는 대신
    // searchEdit_ 검색어 하나로 흡수한다 — matchesFilters()가 세 필드를 한 번에 대조한다.
    LiveSearchLineEdit* searchEdit_ = nullptr;
    QPushButton* periodCustomBtn_ = nullptr;
    CareGuardDateEdit* startDateEdit_ = nullptr;
    CareGuardDateEdit* endDateEdit_ = nullptr;

    // GET /video-events 호출용. 서버 주소는 loginwindow.cpp와 동일하게 환경변수
    // CAREGUARD_SERVER_URL, 없으면 http://127.0.0.1:8000.
    QNetworkAccessManager* netManager_ = nullptr;
    QString serverUrl_;

    // 실시간 새로고침용. handleWsMessage() 참고.
    WsClient* wsClient_ = nullptr;
};
