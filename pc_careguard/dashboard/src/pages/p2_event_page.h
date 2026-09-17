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
class WsClient;
class QJsonObject;

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

    // P3RecipientPage::recipientsUpdated를 MainWindow가 여기로 연결한다. 대상자 이름·배정
    // 로봇이 P3에서 바뀌면 목록을 다시 받아와 "대상자"/"로봇" 칸이 바로 새 값으로 보이게
    // 한다(안 그러면 다음 기간 재선택 전까지 옛 이름이 그대로 남는다).
    void refreshFromServer();

    // MainWindow가 앱 공유 WsClient를 내려준다(P1/P3 등과 같은 패턴) — scheduler.py가
    // MED_MISSED 등을 만들 때 보내는 event_created를 구독해, 새로고침 없이도 방금 생긴
    // 이벤트가 바로 목록에 뜨게 한다. 예전엔 이 페이지만 구독이 빠져있어서 실시간 반영이
    // 전혀 안 됐다(2026-09-17 리포트).
    void setWsClient(WsClient* client);

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
    void handleWsMessage(const QJsonObject& message);
    // 필터·정렬·모델 갱신 등으로 proxy_ 행 구성이 바뀔 때마다 proxy_ 시그널(연결부
    // 참고)로 불린다. 페이지 수를 다시 계산해 범위를 벗어나면 currentPage_를 안으로
    // 당기고, 현재 페이지 밖 행은 tableView_->setRowHidden()으로 숨긴 뒤 페이지 번호
    // 버튼 줄을 다시 그린다. QTableView는 스크롤만 있고 실제 "페이지" 개념이 없어서
    // 서버 쪽 페이징 대신 이 방식(행 숨김)으로 구현한다 — 목록이 몇백 건을 넘기 전까지는
    // 성능에 문제없다.
    void updatePagination();

    ApiClient* apiClient_;
    WsClient* wsClient_ = nullptr;
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

    // 페이징 — 한 페이지에 보여줄 행 수. 서버 페이징 없이 클라이언트에서 행을 숨기는
    // 방식이라(updatePagination() 주석 참고) 값을 바꿔도 API 호출은 그대로다.
    static constexpr int kEventsPerPage = 20;
    int currentPage_ = 0;  // 0-based
    QPushButton* paginationPrevBtn_ = nullptr;
    QPushButton* paginationNextBtn_ = nullptr;
    QWidget* paginationPagesContainer_ = nullptr;  // updatePagination()이 매번 안의 번호 버튼을 다시 채운다
};
