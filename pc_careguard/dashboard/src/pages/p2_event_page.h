#pragma once
#include <functional>

#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QButtonGroup;
class QTableView;
class QTimer;
class QModelIndex;
class CareGuardDateEdit;
class QEvent;
class EventTableModel;
class EventFilterProxyModel;
class EventRowDelegate;
class LiveSearchLineEdit;
class ApiClient;

// P2 이벤트 관제. gui/wireframe/v1/p2_event_queue_wireframe_v2.html,
// CareGuard_DESIGN_SYSTEM.md §4-3(테이블 규약)·§6(P2 행) 기준.
// GET /events로 care_event를 불러오고, 완료 처리는 POST /events/{event_uid}/resolve로
// 서버에 반영한다(P3 대상자 페이지와 같은 ApiClient 패턴). 서버가 응답하기 전까지 화면이
// 비어 보이지 않도록 생성자에서 seedMockData()로 먼저 채워두고, 응답이 오면 덮어쓴다 —
// 서버 미기동 시에도 화면은 mock으로 계속 동작한다.
class P2EventPage : public QWidget {
    Q_OBJECT
public:
    explicit P2EventPage(QWidget* parent = nullptr);

    // P3 대상자 카드의 "이벤트 확인" 클릭 진입점. 검색창에 대상자 이름을 채워 그 대상자의
    // 이벤트만 보이게 하고, 기간을 30일로 넓혀 오늘 범위 밖 이벤트도 가리지 않게 한다.
    void filterToRecipient(const QString& recipientName);

    // P4 영상 페이지 "이벤트 상세로 이동" 클릭 진입점. eventUid로 검색·기간을 맞춘 뒤,
    // 최신 상태를 다시 받아와(P4에서 방금 막 생긴 이벤트일 수 있음) 상세 모달을 바로 연다.
    void openEventByUid(const QString& eventUid);

    // P1 로봇 상세 모달의 "이벤트 상세 열기" 진입점. openEventByUid와 검색·기간 맞추는
    // 부분만 같고, 상세 모달은 열지 않는다 — 로봇 상세 모달이 이미 닫히면서 이 페이지로
    // 넘어온 직후라, 곧바로 또 다른 모달(EventDetailModal)이 뜨면 모달이 연달아 두 번
    // 뜨는 것처럼 보인다는 리포트(2026-09-14)로 분리했다. 목록에서 검색된 행을 사용자가
    // 직접 눌러 열람하면 된다.
    void filterToEventUid(const QString& eventUid);

    // role=guardian 전용. P3RecipientPage::guardianRecipientNamesChanged를 MainWindow가
    // 여기로 연결한다 — 빈 목록이면 필터 없음(admin/operator).
    void setRecipientNameFilter(const QStringList& names);

signals:
    // MainWindow가 TopBar 뱃지(setEmergencyCount/setAttentionCount)에 연결한다.
    void unresolvedCountsChanged(int emergencyCount, int attentionCount);
    // EventDetailModal::videoViewRequested를 그대로 위로 전달한다. MainWindow가
    // P4VideoPage::openVideoPlayerByEventUid()에 연결한다.
    void videoViewRequested(const QString& eventUid);

protected:
    // 기간 세그먼트가 "직접"이 아닐 때 날짜 칸은 읽기 전용으로 계산된 범위만 보여준다.
    // 그 상태에서 칸을 클릭하면 "직접"로 전환해야 하는데, 읽기 전용 위젯은 클릭을 그대로
    // 통과시키므로(=편집 불가만 막힘) 여기서 잡아서 처리한다.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildHeaderRow();
    QWidget* buildFilterBar();
    QWidget* buildBulkBar();
    QWidget* buildTable();
    QWidget* buildPagination();

    void seedMockData();
    // onLoaded: 목록 갱신이 끝난 뒤(성공 시에만) 호출할 콜백. openEventByUid()가 재조회 후
    // 상세 모달을 여는 데 쓴다 — 기본값(nullptr)이면 그냥 갱신만 한다.
    void loadEventsFromServer(std::function<void()> onLoaded = nullptr);
    // loadEventsFromServer() 콜백에서 호출. model_에서 eventUid로 행을 찾아 상세 모달을
    // 열고, 못 찾으면(아직 처리 중이거나 취소된 이벤트) 안내만 띄운다.
    void openEventRowIfPresent(const QString& eventUid);
    void resolveEvent(const QString& eventId);
    void refreshCounts();
    void onCellClicked(const QModelIndex& index);
    void onBulkComplete();
    void onDateRangeChanged();
    void applyPeriodPreset(int spanDays);   // -1 = 직접(수동 편집 허용), 0/6/29 = 오늘/7일/30일
    void setDateEditsReadOnly(bool readOnly);
    void showComingSoon(const QString& feature);

    ApiClient* apiClient_;
    EventTableModel* model_;
    EventFilterProxyModel* proxy_;
    EventRowDelegate* delegate_;
    QTableView* tableView_ = nullptr;

    QWidget* bulkBar_;
    QLabel* bulkCountLabel_;
    QLabel* filterCountLabel_;
    QPushButton* unresolvedOnlyChip_;
    QPushButton* csvBtn_ = nullptr;
    LiveSearchLineEdit* searchEdit_;
    // guardian 로그인이면 true — setRecipientNameFilter(비어있지 않은 목록)가 켠다.
    // 요청: 이벤트 상세는 "영상 열람"만 가능하고 나머지 조치는 못 하게(텍스트로만 보이게).
    // 체크박스로 여러 이벤트를 골라 일괄 처리/CSV로 내보내는 것도 같은 이유로 막는다.
    bool readOnly_ = false;
    QButtonGroup* periodGroup_;
    QPushButton* periodMonthBtn_;
    QPushButton* periodCustomBtn_;
    CareGuardDateEdit* startDateEdit_;
    CareGuardDateEdit* endDateEdit_;

    QTimer* elapsedTimer_;
    QTimer* pulseTimer_;
    bool pulseOn_ = true;
    int hoveredRow_ = -1;  // 프록시 모델 기준 행 번호. EventRowDelegate가 행 단위 호버 판정에 참조.
};
