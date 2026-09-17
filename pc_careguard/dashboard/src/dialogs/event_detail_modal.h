#pragma once
#include <QDialog>

#include "widgets/event_table_model.h"

class QWidget;
class QLabel;
class QTextEdit;
class QPushButton;
class ApiClient;
class CallAudioClient;

// P2 이벤트 상세 모달 — gui/wireframe/v1/p2_event_detail_modal_wireframe.html 대응.
// 이벤트 API가 아직 없어 EventTableModel::EventRow 스냅샷만으로 화면을 구성한다
// (트리거 그래프·음성 확인 로그·처리 이력은 행 데이터로부터 파생한 표시용 목데이터).
class EventDetailModal : public QDialog {
    Q_OBJECT
public:
    // readOnly=true면(guardian 로그인) "영상 열람"만 클릭 가능하게 남기고, 나머지 조치
    // (스피커 통화·대상자 전화·보호자 연락/문자·119 모의 신고·재확인 예약·이벤트 취소·완료
    // 처리)는 버튼으로 넣지(insert) 않고 같은 문구를 그냥 텍스트로만 보여준다(요청).
    explicit EventDetailModal(const EventTableModel::EventRow& row, bool readOnly = false,
                               QWidget* parent = nullptr);

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
    void handleSpeakerCallClicked();
    void resetSpeakerCallButton();
    void handleEmergencyReportClicked();
    void resetEmergencyReportButton();

    EventTableModel::EventRow row_;
    bool readOnly_ = false;
    int currentStep_ = 0;   // 0=감지 1=출동 2=음성확인 3=조치 4=완료
    bool allStepsDone_ = false;

    QTextEdit* resolveNoteEdit_ = nullptr;
    QPushButton* resolveBtn_ = nullptr;

    // "스피커 통화" — REST(ApiClient::startCall/endCall)로 로봇에 신호를 보내고,
    // 실제 오디오 바이트는 CallAudioClient가 별도 WebSocket으로 주고받는다.
    QPushButton* speakerBtn_ = nullptr;
    ApiClient* apiClient_ = nullptr;
    CallAudioClient* callAudio_ = nullptr;
    bool callActive_ = false;

    // "119 모의 신고" — 실제 119 연동이 아니라, 로봇 후면 키오스크에 "보호자가 119
    // 신고중" 상태를 띄우는 토글(스피커 통화와 같은 시작/종료 패턴).
    QPushButton* emergencyBtn_ = nullptr;
    bool emergencyReportActive_ = false;
};
