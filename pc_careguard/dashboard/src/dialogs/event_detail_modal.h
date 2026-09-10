#pragma once
#include <QDialog>

#include "widgets/event_table_model.h"

class QWidget;
class QLabel;
class QTextEdit;
class QPushButton;

// P2 이벤트 상세 모달 — gui/wireframe/v1/p2_event_detail_modal_wireframe.html 대응.
// 이벤트 API가 아직 없어 EventTableModel::EventRow 스냅샷만으로 화면을 구성한다
// (트리거 그래프·음성 확인 로그·처리 이력은 행 데이터로부터 파생한 표시용 목데이터).
class EventDetailModal : public QDialog {
    Q_OBJECT
public:
    explicit EventDetailModal(const EventTableModel::EventRow& row, QWidget* parent = nullptr);

signals:
    // "RESOLVED 전환" 확정 시 P2EventPage가 모델에 반영하도록 전달한다.
    void eventResolved(const QString& eventId);
    // "사유 입력 후 열람" 클릭. P2EventPage -> MainWindow -> P4VideoPage::openVideoPlayerByEventUid()로
    // 이어져, 이 모달 위에 재생 모달(사유 입력 잠금 포함)을 그대로 띄운다.
    void videoViewRequested(const QString& eventUid);

private:
    QWidget* buildHeader();
    QWidget* buildProgressBar();
    QWidget* buildLeftColumn();
    QWidget* buildRightColumn();
    QWidget* buildTriggerSection();
    QWidget* buildVoiceLogSection();
    QWidget* buildVideoSection();
    QWidget* buildActionGrid();
    QWidget* buildHistorySection();
    QWidget* buildResolveSection();

    void showComingSoon(const QString& feature);
    void updateResolveButtonState();
    void handleResolveClicked();

    EventTableModel::EventRow row_;
    int currentStep_ = 0;   // 0=감지 1=출동 2=음성확인 3=조치 4=완료
    bool allStepsDone_ = false;

    QTextEdit* resolveNoteEdit_ = nullptr;
    QPushButton* resolveBtn_ = nullptr;
};
