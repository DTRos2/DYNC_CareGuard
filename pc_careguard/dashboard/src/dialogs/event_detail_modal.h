#pragma once
#include <QDialog>
#include <QVector>

#include "net/api_client.h"
#include "widgets/event_table_model.h"

class QWidget;
class QLabel;
class QTextEdit;
class QPushButton;
class QFrame;
class CallAudioClient;

// P2 이벤트 상세 모달 — gui/wireframe/v1/p2_event_detail_modal_wireframe.html 대응.
// 이벤트 API가 아직 없어 EventTableModel::EventRow 스냅샷만으로 화면을 구성한다
// (트리거 그래프·음성 확인 로그·처리 이력은 행 데이터로부터 파생한 표시용 목데이터).
class EventDetailModal : public QDialog {
    Q_OBJECT
public:
    explicit EventDetailModal(const EventTableModel::EventRow& row, QWidget* parent = nullptr);

signals:
    // "처리 완료" 확정 시 P2EventPage가 모델에 반영하도록 전달한다.
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
    QWidget* buildVideoSection();
    QWidget* buildActionGrid();
    QWidget* buildHistorySection();
    QWidget* buildResolveSection();

    void showComingSoon(const QString& feature);
    void updateResolveButtonState();
    void handleResolveClicked();
    void handleSpeakerCallClicked();
    void resetSpeakerCallButton();
    // 진행 단계 표시(감지/조치/완료)를 currentStep_·allStepsDone_·actionSuffix_ 현재 값대로
    // 다시 그린다. buildProgressBar()가 처음 그릴 때, markActionTaken()이 "조치" 단계를
    // 완료 처리할 때 각각 호출한다.
    void refreshProgressBar();
    // 조치 버튼(스피커 통화 종료·보호자 문자) 클릭 시 "조치" 단계에 체크를 찍고 "완료"
    // 단계를 다음 단계로 넘긴다. suffix는 "(스피커)"/"(문자)"처럼 "조치" 라벨 뒤에 붙는
    // 텍스트 — 어느 수단으로 조치했는지 남긴다.
    void markActionTaken(const QString& suffix);
    QString stepLabelText(int i) const;
    // "보호자 연락처" 클릭 — GET /recipients에서 row_.recipientName으로 대상자 id를 찾고,
    // GET /recipients/{id}(fetchRecipientDetail)의 guardians를 팝오버로 보여준다. 대상자
    // 상세 모달(P3RecipientDetailDialog)과 같은 서버 데이터를 그대로 쓴다 — 이 모달은
    // 읽기 전용이라 별도 화면 전용 데이터(119·112 고정 행 등)는 덧붙이지 않는다.
    void handleGuardianContactClicked();
    void showGuardianContactPopover(const QVector<ApiClient::GuardianInfo>& guardians);

    EventTableModel::EventRow row_;
    int currentStep_ = 0;   // 0=감지 1=조치 2=완료
    bool allStepsDone_ = false;
    // "조치" 단계 라벨에 붙는 접미사 — 스피커 통화 종료·보호자 문자 중 실제로 쓴 수단을
    // 남긴다("(스피커)"/"(문자)"). 비어있으면 아직 조치 전.
    QString actionSuffix_;
    QLabel* stepDots_[3] = {nullptr, nullptr, nullptr};
    QLabel* stepLabels_[3] = {nullptr, nullptr, nullptr};
    QFrame* stepLines_[2] = {nullptr, nullptr};

    QTextEdit* resolveNoteEdit_ = nullptr;
    QPushButton* resolveBtn_ = nullptr;

    // "스피커 통화" — REST(ApiClient::startCall/endCall)로 로봇에 신호를 보내고,
    // 실제 오디오 바이트는 CallAudioClient가 별도 웹소켓으로 주고받는다.
    QPushButton* speakerBtn_ = nullptr;
    // "보호자 연락처" — 팝오버가 떠 있는 동안/응답을 기다리는 동안 버튼 위치 앵커 겸
    // 중복 클릭 방지용 비활성화 대상.
    QPushButton* guardianContactBtn_ = nullptr;
    ApiClient* apiClient_ = nullptr;
    CallAudioClient* callAudio_ = nullptr;
    bool callActive_ = false;
};
