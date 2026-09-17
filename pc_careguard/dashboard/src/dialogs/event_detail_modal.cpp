#include "event_detail_modal.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "audio/call_audio_client.h"
#include "net/api_client.h"
#include "net/server_config.h"
#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {

using EventRow = EventTableModel::EventRow;
using Severity = EventTableModel::Severity;

QColor severityColor(Severity sev) {
    switch (sev) {
        case Severity::Emergency: return Tokens::EMERGENCY.main;
        case Severity::Attention: return Tokens::ATTENTION.main;
        default: return Tokens::INFO.main;
    }
}

QString severityText(Severity sev) {
    switch (sev) {
        case Severity::Emergency: return QStringLiteral("긴급");
        case Severity::Attention: return QStringLiteral("주의");
        default: return QStringLiteral("정보");
    }
}

QString severityBadgeLevel(Severity sev) {
    switch (sev) {
        case Severity::Emergency: return QStringLiteral("emergencyFill");
        case Severity::Attention: return QStringLiteral("attention");
        default: return QStringLiteral("info");
    }
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", "label");
    return label;
}

QFrame* makeBoxFrame(QWidget* parent) {
    // 타입 셀렉터("QFrame{...}")로 스타일시트를 지정하면 QLabel이 QFrame의 서브클래스라
    // 이 박스 안의 모든 QLabel(시간·본문·상태 텍스트)에도 border/radius가 그대로 상속되어
    // 각 텍스트마다 캡슐 테두리가 둘러지는 문제가 있었다. objectName + ID 셀렉터로 스코프를
    // 이 프레임 자신에게만 한정한다.
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("eventDetailBox"));
    frame->setStyleSheet(QStringLiteral("QFrame#eventDetailBox{border:0.5px solid %1; border-radius:8px;}")
                              .arg(Tokens::BORDER.name()));
    return frame;
}

}  // namespace

EventDetailModal::EventDetailModal(const EventTableModel::EventRow& row, bool readOnly, QWidget* parent)
    : QDialog(parent), row_(row), readOnly_(readOnly) {
    setWindowTitle(QStringLiteral("이벤트 상세 — %1").arg(row_.eventId));
    setModal(true);
    // OS 창관리자 타이틀바를 없애 커스텀 헤더(제목·X)만 남긴다 — 안 그러면 X가 두 개로 보임
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setFixedWidth(640);

    if (row_.resolved) {
        currentStep_ = 4;
        allStepsDone_ = true;
    } else if (!row_.assignee.isEmpty()) {
        currentStep_ = 2;  // 감지·출동 완료, 음성 확인 진행 중
    } else {
        currentStep_ = 1;  // 감지 완료, 출동 진행 중
    }

    apiClient_ = new ApiClient(this);
    callAudio_ = new CallAudioClient(this);
    // 통화 중 오디오 소켓이 끊기면(로봇 다운링크 재기동, 네트워크 단절 등) 버튼을
    // "통화 종료"로 남겨두지 않고 "스피커 통화"로 되돌린다 — 서버에는 이미 없는
    // 연결을 사람이 계속 "통화 중"으로 오해하지 않게 하기 위함.
    connect(callAudio_, &CallAudioClient::audioDisconnected, this, [this] {
        callActive_ = false;
        resetSpeakerCallButton();
    });
    // 통화 중에 모달을 닫아도(X 버튼 등) 통화가 계속 열려있으면 안 된다 — 로봇 쪽
    // 스피커/마이크가 계속 켜진 채로 남는 것을 막는다.
    connect(this, &QDialog::finished, this, [this](int) {
        if (!callActive_) return;
        callActive_ = false;
        callAudio_->stop();
        apiClient_->endCall(row_.robotId, [](bool, bool, const QString&) {});
    });
    // 모의 신고 중에 모달을 닫아도(X 버튼 등) 키오스크에 "119 신고중"이 계속 남으면
    // 안 된다 — callActive_와 같은 이유.
    connect(this, &QDialog::finished, this, [this](int) {
        if (!emergencyReportActive_) return;
        emergencyReportActive_ = false;
        apiClient_->endEmergencyReport(row_.robotId, [](bool, const QString&) {});
    });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());
    root->addWidget(buildProgressBar());

    auto* body = new QHBoxLayout();
    body->setContentsMargins(16, 14, 16, 14);
    body->setSpacing(12);
    body->addWidget(buildLeftColumn(), 6);
    body->addWidget(buildRightColumn(), 5);
    root->addLayout(body);
}

QWidget* EventDetailModal::buildHeader() {
    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    auto* badge = new QLabel(severityText(row_.severity), header);
    badge->setObjectName("badge");
    badge->setProperty("level", severityBadgeLevel(row_.severity));
    layout->addWidget(badge);

    auto* eventId = new QLabel(row_.eventId, header);
    eventId->setProperty("role", "mono");
    eventId->setStyleSheet(QStringLiteral("font-weight:500; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
    layout->addWidget(eventId);

    const QString infoText = QStringLiteral("%1 · %2 · %3")
                                  .arg(row_.recipientName, row_.robotId, row_.timeText);
    auto* info = new QLabel(infoText, header);
    info->setProperty("role", "secondary");
    layout->addWidget(info);

    layout->addStretch();

    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_SECONDARY, 16), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(closeBtn);

    return header;
}

QWidget* EventDetailModal::buildProgressBar() {
    auto* bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("eventProgressBar"));
    bar->setStyleSheet(
        QStringLiteral("QFrame#eventProgressBar{border-bottom:0.5px solid %1;}").arg(Tokens::BORDER.name()));
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 10, 16, 10);
    layout->setSpacing(4);

    static const QStringList kSteps = {QStringLiteral("감지"), QStringLiteral("출동"),
                                        QStringLiteral("음성 확인"), QStringLiteral("조치"),
                                        QStringLiteral("완료")};
    const QColor activeColor = severityColor(row_.severity);

    for (int i = 0; i < kSteps.size(); ++i) {
        const bool done = allStepsDone_ || i < currentStep_;
        const bool active = !allStepsDone_ && i == currentStep_;

        auto* step = new QWidget(bar);
        auto* stepLayout = new QHBoxLayout(step);
        stepLayout->setContentsMargins(0, 0, 0, 0);
        stepLayout->setSpacing(4);

        auto* dot = new QLabel(step);
        dot->setFixedSize(14, 14);
        dot->setAlignment(Qt::AlignCenter);
        QFont dotFont = dot->font();
        dotFont.setPointSizeF(8);
        dot->setFont(dotFont);
        if (done) {
            dot->setText(QStringLiteral("✓"));
            dot->setStyleSheet(QStringLiteral("border-radius:7px; background:%1; color:#FFFFFF;")
                                    .arg(Tokens::OK.main.name()));
        } else if (active) {
            dot->setText(QStringLiteral("●"));
            dot->setStyleSheet(QStringLiteral("border-radius:7px; background:%1; color:#FFFFFF;")
                                    .arg(activeColor.name()));
        } else {
            dot->setStyleSheet(QStringLiteral("border-radius:7px; border:1px solid %1;")
                                    .arg(Tokens::BORDER_STRONG.name()));
        }
        stepLayout->addWidget(dot);

        auto* label = new QLabel(kSteps[i], step);
        QColor textColor = Tokens::TEXT_MUTED;
        if (done) textColor = Tokens::OK.main;
        else if (active) textColor = activeColor;
        label->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:%2;")
                                  .arg(textColor.name(), (done || active) ? "500" : "400"));
        stepLayout->addWidget(label);

        layout->addWidget(step);

        if (i < kSteps.size() - 1) {
            auto* line = new QFrame(bar);
            line->setFixedHeight(2);
            const bool lineDone = allStepsDone_ || (i + 1) <= currentStep_;
            line->setStyleSheet(QStringLiteral("background:%1;")
                                     .arg((lineDone ? Tokens::OK.main : Tokens::BORDER).name()));
            layout->addWidget(line, /*stretch=*/1);
        }
    }

    return bar;
}

QWidget* EventDetailModal::buildLeftColumn() {
    auto* col = new QWidget(this);
    auto* layout = new QVBoxLayout(col);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    layout->addWidget(buildTriggerSection());
    layout->addWidget(buildVoiceLogSection());
    layout->addWidget(buildVideoSection());
    layout->addStretch();

    return col;
}

QWidget* EventDetailModal::buildRightColumn() {
    auto* col = new QWidget(this);
    auto* layout = new QVBoxLayout(col);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    layout->addWidget(buildActionGrid());
    layout->addWidget(buildHistorySection());
    layout->addWidget(buildResolveSection());
    layout->addStretch();

    return col;
}

QWidget* EventDetailModal::buildTriggerSection() {
    auto* wrap = new QWidget(this);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);

    layout->addWidget(makeSectionLabel(QStringLiteral("트리거 근거"), wrap));

    auto* box = makeBoxFrame(wrap);
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(10, 10, 10, 10);
    boxLayout->setSpacing(6);

    auto* summary = new QLabel(row_.summary, box);
    summary->setProperty("role", "secondary");
    summary->setWordWrap(true);
    boxLayout->addWidget(summary);

    layout->addWidget(box);
    return wrap;
}

QWidget* EventDetailModal::buildVoiceLogSection() {
    auto* wrap = new QWidget(this);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addWidget(makeSectionLabel(QStringLiteral("음성 확인 결과"), wrap));

    auto* box = makeBoxFrame(wrap);
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setSpacing(0);

    struct LogLine { QString time; QString text; QString status; QColor statusColor; };
    QVector<LogLine> lines;
    if (row_.resolved) {
        lines.append({row_.timeText, QStringLiteral("확인 완료 · 정상 종료"), QStringLiteral("완료"),
                      Tokens::OK.main});
    } else if (row_.severity == Severity::Emergency) {
        lines.append({row_.timeText, QStringLiteral("1차 질문 송출 \"괜찮으세요?\""),
                      QStringLiteral("무응답"), Tokens::EMERGENCY.main});
        lines.append({QString(), QStringLiteral("2차 질문 송출 · 담당자 연락 예고"),
                      QStringLiteral("응답 대기 중"), Tokens::INFO.main});
    } else {
        lines.append({row_.timeText, QStringLiteral("알림 발송 · 확인 대기"),
                      QStringLiteral("대기 중"), Tokens::ATTENTION.main});
    }

    for (int i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        auto* row = new QWidget(box);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 6, 10, 6);
        rowLayout->setSpacing(8);

        auto* time = new QLabel(line.time.isEmpty() ? Tokens::Format::EMPTY : line.time, row);
        time->setProperty("role", "mono");
        time->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_SECONDARY.name()));
        rowLayout->addWidget(time);

        auto* text = new QLabel(line.text, row);
        text->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
        rowLayout->addWidget(text, /*stretch=*/1);

        auto* status = new QLabel(line.status, row);
        status->setStyleSheet(
            QStringLiteral("font-size:11px; font-weight:500; color:%1;").arg(line.statusColor.name()));
        rowLayout->addWidget(status);

        boxLayout->addWidget(row);

        if (i < lines.size() - 1) {
            auto* sep = new QFrame(box);
            sep->setFixedHeight(1);
            sep->setStyleSheet(QStringLiteral("background:%1;").arg(Tokens::BORDER.name()));
            boxLayout->addWidget(sep);
        }
    }

    layout->addWidget(box);
    return wrap;
}

QWidget* EventDetailModal::buildVideoSection() {
    auto* wrap = new QWidget(this);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addWidget(makeSectionLabel(
        QStringLiteral("영상 열람 — 사유 입력 필수 · 열람 기록 저장"), wrap));

    auto* box = makeBoxFrame(wrap);
    auto* boxLayout = new QHBoxLayout(box);
    boxLayout->setContentsMargins(10, 8, 10, 8);
    boxLayout->setSpacing(8);

    auto* thumb = new QLabel(box);
    thumb->setFixedSize(64, 40);
    thumb->setAlignment(Qt::AlignCenter);
    thumb->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;").arg(Tokens::TEXT_PRIMARY.name()));
    thumb->setPixmap(SimpleIcons::play(Tokens::TEXT_INVERSE, 14).pixmap(14, 14));
    boxLayout->addWidget(thumb);

    auto* textCol = new QVBoxLayout();
    textCol->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("%1 전방 카메라 · 이벤트 전후 구간").arg(row_.robotId), box);
    title->setStyleSheet(QStringLiteral("font-size:11px; font-weight:500; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
    textCol->addWidget(title);
    auto* sub = new QLabel(QStringLiteral("열람 가능 시간 남음"), box);
    sub->setProperty("role", "muted");
    sub->setStyleSheet(QStringLiteral("font-size:11px;"));
    textCol->addWidget(sub);
    boxLayout->addLayout(textCol, /*stretch=*/1);

    auto* viewBtn = new QPushButton(QStringLiteral("사유 입력 후 열람"), box);
    viewBtn->setProperty("variant", "secondary");
    viewBtn->setProperty("size", "s");
    // 실제 사유 입력·잠금 UI는 P4의 재생 모달(VideoPlayerDialog)이 이미 갖고 있어 여기서
    // 새로 안 만든다 — eventId만 실어 올려보내면 P4VideoPage가 그 클립을 찾아 그 모달을
    // 그대로 띄운다. 이 상세 모달은 닫지 않는다 — 재생 모달이 그 위에 뜨고, 닫으면 다시
    // 이 화면으로 돌아온다.
    connect(viewBtn, &QPushButton::clicked, this,
            [this] { emit videoViewRequested(row_.eventId); });
    boxLayout->addWidget(viewBtn);

    layout->addWidget(box);
    return wrap;
}

QWidget* EventDetailModal::buildActionGrid() {
    auto* wrap = new QWidget(this);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addWidget(makeSectionLabel(QStringLiteral("조치"), wrap));

    auto* grid = new QGridLayout();
    grid->setSpacing(6);

    // guardian(readOnly_)은 "영상 열람"만 클릭 가능해야 한다(요청) — 나머지 조치는
    // 버튼으로 넣지 않고 같은 문구를 그냥 텍스트(QLabel)로만 보여준다. speakerBtn_/
    // emergencyBtn_는 이 모드에선 안 만들어지므로 nullptr로 남고, 그 값을 쓰는
    // handleSpeakerCallClicked/handleEmergencyReportClicked 등은 버튼이 없어 애초에
    // 호출될 일이 없다(callActive_/emergencyReportActive_도 항상 false로 유지됨).
    if (readOnly_) {
        auto addTextCell = [&](const QString& text, int row, int col, int rowSpan = 1, int colSpan = 1) {
            auto* label = new QLabel(text, wrap);
            label->setAlignment(Qt::AlignCenter);
            label->setStyleSheet(QStringLiteral("font-size:11px; color:%1; padding:6px 8px; "
                                                 "border:0.5px solid %2; border-radius:6px;")
                                      .arg(Tokens::TEXT_MUTED.name(), Tokens::BORDER.name()));
            grid->addWidget(label, row, col, rowSpan, colSpan);
        };
        addTextCell(QStringLiteral("스피커 통화"), 0, 0);
        addTextCell(QStringLiteral("대상자 전화"), 0, 1);
        addTextCell(QStringLiteral("보호자 연락"), 1, 0);
        addTextCell(QStringLiteral("보호자 문자"), 1, 1);
        addTextCell(QStringLiteral("119 모의 신고"), 2, 0, 1, 2);
        addTextCell(QStringLiteral("재확인 예약"), 3, 0);
        addTextCell(QStringLiteral("이벤트 취소"), 3, 1);
        layout->addLayout(grid);
        return wrap;
    }

    speakerBtn_ =
        new QPushButton(SimpleIcons::speakerphone(Tokens::TEXT_INVERSE, 12), QStringLiteral("스피커 통화"), wrap);
    speakerBtn_->setProperty("variant", "primary");
    speakerBtn_->setProperty("size", "s");
    connect(speakerBtn_, &QPushButton::clicked, this, &EventDetailModal::handleSpeakerCallClicked);
    grid->addWidget(speakerBtn_, 0, 0);

    auto* callBtn =
        new QPushButton(SimpleIcons::phone(Tokens::TEXT_PRIMARY, 12), QStringLiteral("대상자 전화"), wrap);
    callBtn->setProperty("variant", "secondary");
    callBtn->setProperty("size", "s");
    connect(callBtn, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("대상자 전화")); });
    grid->addWidget(callBtn, 0, 1);

    auto* guardianCallBtn =
        new QPushButton(SimpleIcons::users(Tokens::TEXT_PRIMARY, 12), QStringLiteral("보호자 연락"), wrap);
    guardianCallBtn->setProperty("variant", "secondary");
    guardianCallBtn->setProperty("size", "s");
    connect(guardianCallBtn, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("보호자 연락")); });
    grid->addWidget(guardianCallBtn, 1, 0);

    auto* guardianMsgBtn =
        new QPushButton(SimpleIcons::message(Tokens::TEXT_PRIMARY, 12), QStringLiteral("보호자 문자"), wrap);
    guardianMsgBtn->setProperty("variant", "secondary");
    guardianMsgBtn->setProperty("size", "s");
    connect(guardianMsgBtn, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("보호자 문자")); });
    grid->addWidget(guardianMsgBtn, 1, 1);

    emergencyBtn_ = new QPushButton(SimpleIcons::phoneCall(Tokens::TEXT_INVERSE, 12),
                                     QStringLiteral("119 모의 신고"), wrap);
    emergencyBtn_->setProperty("variant", "danger");
    emergencyBtn_->setProperty("size", "s");
    connect(emergencyBtn_, &QPushButton::clicked, this, &EventDetailModal::handleEmergencyReportClicked);
    grid->addWidget(emergencyBtn_, 2, 0, 1, 2);

    auto* recheckBtn = new QPushButton(QStringLiteral("재확인 예약"), wrap);
    recheckBtn->setProperty("variant", "secondary");
    recheckBtn->setProperty("size", "s");
    connect(recheckBtn, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("재확인 예약")); });
    grid->addWidget(recheckBtn, 3, 0);

    auto* cancelBtn = new QPushButton(QStringLiteral("이벤트 취소"), wrap);
    cancelBtn->setProperty("variant", "secondary");
    cancelBtn->setProperty("size", "s");
    connect(cancelBtn, &QPushButton::clicked, this,
            [this] { showComingSoon(QStringLiteral("이벤트 취소")); });
    grid->addWidget(cancelBtn, 3, 1);

    layout->addLayout(grid);
    return wrap;
}

QWidget* EventDetailModal::buildHistorySection() {
    auto* wrap = new QWidget(this);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addWidget(makeSectionLabel(QStringLiteral("처리 이력 · CARE_ACTION"), wrap));

    auto* timeline = new QFrame(wrap);
    timeline->setObjectName(QStringLiteral("eventHistoryTimeline"));
    timeline->setStyleSheet(
        QStringLiteral("QFrame#eventHistoryTimeline{border-left:2px solid %1;}").arg(Tokens::BORDER.name()));
    auto* timelineLayout = new QVBoxLayout(timeline);
    timelineLayout->setContentsMargins(10, 0, 0, 0);
    timelineLayout->setSpacing(6);

    auto addEntry = [&](const QString& time, const QString& text) {
        auto* entry = new QLabel(
            QStringLiteral("<span style='font-family:%1; color:%2;'>%3</span> %4")
                .arg(Tokens::Font::MONO, Tokens::TEXT_SECONDARY.name(), time, text),
            timeline);
        entry->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
        entry->setWordWrap(true);
        timelineLayout->addWidget(entry);
    };

    addEntry(row_.timeText, QStringLiteral("시스템 — 이벤트 생성 · %1 분류").arg(severityText(row_.severity)));
    if (!row_.assignee.isEmpty()) {
        addEntry(QString(), QStringLiteral("%1 — 담당 배정").arg(row_.assignee));
    }
    if (row_.resolved) {
        addEntry(QString(), QStringLiteral("완료 처리"));
    }

    layout->addWidget(timeline);
    return wrap;
}

QWidget* EventDetailModal::buildResolveSection() {
    auto* wrap = new QWidget(this);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(makeSectionLabel(
        row_.resolved ? QStringLiteral("완료 처리") : QStringLiteral("완료 처리 — 메모 필수"), wrap));

    if (row_.resolved) {
        auto* doneLabel = new QLabel(QStringLiteral("이미 완료 처리된 이벤트입니다."), wrap);
        doneLabel->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::OK.main.name()));
        layout->addWidget(doneLabel);
        return wrap;
    }

    // guardian(readOnly_)은 완료 처리를 할 수 없다(요청) — 메모 입력칸·전환 버튼 대신
    // 안내 텍스트만 보여준다.
    if (readOnly_) {
        auto* noteLabel = new QLabel(QStringLiteral("완료 처리는 관제담당자만 할 수 있습니다."), wrap);
        noteLabel->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
        noteLabel->setWordWrap(true);
        layout->addWidget(noteLabel);
        return wrap;
    }

    resolveNoteEdit_ = new QTextEdit(wrap);
    resolveNoteEdit_->setPlaceholderText(QStringLiteral("통화 결과 · 조치 내용을 입력하세요"));
    resolveNoteEdit_->setFixedHeight(44);
    connect(resolveNoteEdit_, &QTextEdit::textChanged, this, &EventDetailModal::updateResolveButtonState);
    layout->addWidget(resolveNoteEdit_);

    resolveBtn_ = new QPushButton(wrap);
    resolveBtn_->setProperty("variant", "secondary");
    connect(resolveBtn_, &QPushButton::clicked, this, &EventDetailModal::handleResolveClicked);
    layout->addWidget(resolveBtn_);

    updateResolveButtonState();
    return wrap;
}

void EventDetailModal::updateResolveButtonState() {
    if (!resolveBtn_ || !resolveNoteEdit_) return;
    const bool ready = !resolveNoteEdit_->toPlainText().trimmed().isEmpty();
    resolveBtn_->setEnabled(ready);
    resolveBtn_->setText(ready ? QStringLiteral("RESOLVED 전환")
                                : QStringLiteral("RESOLVED 전환 (메모 입력 후 활성화)"));
}

void EventDetailModal::handleResolveClicked() {
    emit eventResolved(row_.eventId);
    accept();
}

void EventDetailModal::showComingSoon(const QString& feature) {
    QMessageBox::information(this, QStringLiteral("CareGuard"),
                              QStringLiteral("추후 구현: %1").arg(feature));
}

void EventDetailModal::handleSpeakerCallClicked() {
    speakerBtn_->setEnabled(false);

    if (callActive_) {
        apiClient_->endCall(row_.robotId, [this](bool ok, bool, const QString& err) {
            callActive_ = false;
            callAudio_->stop();
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("통화 종료 실패"),
                                      err.isEmpty() ? QStringLiteral("서버 연결을 확인해주세요.") : err);
            }
            resetSpeakerCallButton();
        });
        return;
    }

    apiClient_->startCall(row_.robotId, row_.recipientName, [this](bool ok, bool robotConnected, const QString& err) {
        if (!ok || !robotConnected) {
            QMessageBox::warning(this, QStringLiteral("통화 연결 실패"),
                                  !ok && !err.isEmpty() ? err : QStringLiteral("로봇 연결을 확인해주세요."));
            resetSpeakerCallButton();
            return;
        }
        callActive_ = true;
        callAudio_->start(ServerConfig::resolveServerUrl(), row_.robotId);
        speakerBtn_->setText(QStringLiteral("통화 종료"));
        speakerBtn_->setEnabled(true);
    });
}

void EventDetailModal::resetSpeakerCallButton() {
    speakerBtn_->setText(QStringLiteral("스피커 통화"));
    speakerBtn_->setEnabled(true);
}

void EventDetailModal::handleEmergencyReportClicked() {
    emergencyBtn_->setEnabled(false);

    if (emergencyReportActive_) {
        apiClient_->endEmergencyReport(row_.robotId, [this](bool ok, const QString& err) {
            emergencyReportActive_ = false;
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("119 모의 신고 종료 실패"),
                                      err.isEmpty() ? QStringLiteral("서버 연결을 확인해주세요.") : err);
            }
            resetEmergencyReportButton();
        });
        return;
    }

    apiClient_->startEmergencyReport(row_.robotId, row_.recipientName, [this](bool ok, const QString& err) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("119 모의 신고 실패"),
                                  err.isEmpty() ? QStringLiteral("서버 연결을 확인해주세요.") : err);
            resetEmergencyReportButton();
            return;
        }
        emergencyReportActive_ = true;
        emergencyBtn_->setText(QStringLiteral("119 신고 종료"));
        emergencyBtn_->setEnabled(true);
    });
}

void EventDetailModal::resetEmergencyReportButton() {
    emergencyBtn_->setText(QStringLiteral("119 모의 신고"));
    emergencyBtn_->setEnabled(true);
}
