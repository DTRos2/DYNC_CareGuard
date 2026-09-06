#pragma once
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

// P2 이벤트 관제. gui/wireframe/v1/p2_event_queue_wireframe_v2.html,
// CareGuard_DESIGN_SYSTEM.md §4-3(테이블 규약)·§6(P2 행) 기준.
// 이벤트 API가 아직 없어 인메모리 mock 데이터로 동작한다 (P6 순찰·경로와 동일한 방침).
class P2EventPage : public QWidget {
    Q_OBJECT
public:
    explicit P2EventPage(QWidget* parent = nullptr);

signals:
    // MainWindow가 TopBar 뱃지(setEmergencyCount/setAttentionCount)에 연결한다.
    void unresolvedCountsChanged(int emergencyCount, int attentionCount);

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
    void refreshCounts();
    void onCellClicked(const QModelIndex& index);
    void onBulkComplete();
    void onDateRangeChanged();
    void applyPeriodPreset(int spanDays);   // -1 = 직접(수동 편집 허용), 0/6/29 = 오늘/7일/30일
    void setDateEditsReadOnly(bool readOnly);
    void showComingSoon(const QString& feature);

    EventTableModel* model_;
    EventFilterProxyModel* proxy_;
    EventRowDelegate* delegate_;
    QTableView* tableView_ = nullptr;

    QWidget* bulkBar_;
    QLabel* bulkCountLabel_;
    QLabel* filterCountLabel_;
    QPushButton* unresolvedOnlyChip_;
    LiveSearchLineEdit* searchEdit_;
    QButtonGroup* periodGroup_;
    QPushButton* periodCustomBtn_;
    CareGuardDateEdit* startDateEdit_;
    CareGuardDateEdit* endDateEdit_;

    QTimer* elapsedTimer_;
    QTimer* pulseTimer_;
    bool pulseOn_ = true;
    int hoveredRow_ = -1;  // 프록시 모델 기준 행 번호. EventRowDelegate가 행 단위 호버 판정에 참조.
};
