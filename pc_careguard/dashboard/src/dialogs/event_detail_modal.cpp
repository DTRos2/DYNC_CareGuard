#include "event_detail_modal.h"

#include <algorithm>

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "audio/call_audio_client.h"
#include "dialogs/guardian_mail_dialog.h"
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
        currentStep_ = 2;
        allStepsDone_ = true;
    } else {
        currentStep_ = 1;  // 감지 완료, 조치 진행 중
    }

    apiClient_ = new ApiClient(this);
    callAudio_ = new CallAudioClient(this);
    // 통화 중 오디오 소켓이 끊기면(로봇 다운링크 재기동, 네트워크 단절 등) 버튼을
    // "통화 종료"로 남겨두지 않고 "스피커 통화"로 되돌린다 — 서버에는 이미 없는
    // 연결을 사람이 계속 "통화 중"으로 오해하지 않게 하기 위함.
    connect(callAudio_, &CallAudioClient::audioDisconnected, this, [this] {
        callActive_ = false;
        resetSpeakerCallButton();
        markActionTaken(QStringLiteral("(스피커)"));
    });
    // 통화 중에 모달을 닫아도(X 버튼 등) 통화가 계속 열려있으면 안 된다 — 로봇 쪽
    // 스피커/마이크가 계속 켜진 채로 남는 것을 막는다.
    connect(this, &QDialog::finished, this, [this](int) {
        if (!callActive_) return;
        callActive_ = false;
        callAudio_->stop();
        apiClient_->endCall(row_.robotId, [](bool, bool, const QString&) {});
    });
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());
    root->addWidget(buildProgressBar());

    auto* body = new QHBoxLayout();
    body->setContentsMargins(16, 14, 16, 0);
    body->setSpacing(12);
    body->addWidget(buildLeftColumn(), 6);
    body->addWidget(buildRightColumn(), 5);
    root->addLayout(body);

    // "완료 처리"는 좌우 컬럼에 눌려 좁아지지 않도록 컬럼 밖으로 빼서 모달 너비 전체를 쓴다.
    auto* resolveLayout = new QVBoxLayout();
    resolveLayout->setContentsMargins(16, 12, 16, 14);
    resolveLayout->addWidget(buildResolveSection());
    root->addLayout(resolveLayout);
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

    // 3단계: 감지(0)/조치(1)/완료(2). 조치는 스피커 통화 종료·보호자 메일 클릭으로
    // 체크되고(markActionTaken), 라벨 텍스트도 그때 "조치(스피커)"/"조치(메일)"로 바뀐다
    // — 그래서 dot·label 포인터를 멤버로 들고 있다가 refreshProgressBar()에서 다시 그린다.
    for (int i = 0; i < 3; ++i) {
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
        stepDots_[i] = dot;
        stepLayout->addWidget(dot);

        auto* label = new QLabel(step);
        stepLabels_[i] = label;
        stepLayout->addWidget(label);

        layout->addWidget(step);

        if (i < 2) {
            auto* line = new QFrame(bar);
            line->setFixedHeight(2);
            stepLines_[i] = line;
            layout->addWidget(line, /*stretch=*/1);
        }
    }

    refreshProgressBar();
    return bar;
}

QString EventDetailModal::stepLabelText(int i) const {
    static const QStringList kBase = {QStringLiteral("감지"), QStringLiteral("조치"),
                                       QStringLiteral("완료")};
    if (i == 1 && !actionSuffix_.isEmpty()) return QStringLiteral("조치%1").arg(actionSuffix_);
    return kBase[i];
}

void EventDetailModal::refreshProgressBar() {
    const QColor activeColor = severityColor(row_.severity);

    for (int i = 0; i < 3; ++i) {
        const bool done = allStepsDone_ || i < currentStep_;
        const bool active = !allStepsDone_ && i == currentStep_;

        QLabel* dot = stepDots_[i];
        if (done) {
            dot->setText(QStringLiteral("✓"));
            dot->setStyleSheet(QStringLiteral("border-radius:7px; background:%1; color:#FFFFFF;")
                                    .arg(Tokens::OK.main.name()));
        } else if (active) {
            dot->setText(QStringLiteral("●"));
            dot->setStyleSheet(QStringLiteral("border-radius:7px; background:%1; color:#FFFFFF;")
                                    .arg(activeColor.name()));
        } else {
            dot->setText(QString());
            dot->setStyleSheet(QStringLiteral("border-radius:7px; border:1px solid %1;")
                                    .arg(Tokens::BORDER_STRONG.name()));
        }

        QLabel* label = stepLabels_[i];
        label->setText(stepLabelText(i));
        QColor textColor = Tokens::TEXT_MUTED;
        if (done) textColor = Tokens::OK.main;
        else if (active) textColor = activeColor;
        label->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:%2;")
                                  .arg(textColor.name(), (done || active) ? "500" : "400"));
    }

    for (int i = 0; i < 2; ++i) {
        const bool lineDone = allStepsDone_ || (i + 1) <= currentStep_;
        stepLines_[i]->setStyleSheet(QStringLiteral("background:%1;")
                                          .arg((lineDone ? Tokens::OK.main : Tokens::BORDER).name()));
    }
}

void EventDetailModal::markActionTaken(const QString& suffix) {
    if (allStepsDone_) return;  // 이미 완료 처리된 이벤트를 다시 여는 경우 — 손댈 필요 없음
    actionSuffix_ = suffix;
    if (currentStep_ < 2) currentStep_ = 2;  // "조치" 체크, "완료" 단계로 진행
    refreshProgressBar();
}

QWidget* EventDetailModal::buildLeftColumn() {
    auto* col = new QWidget(this);
    auto* layout = new QVBoxLayout(col);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    layout->addWidget(buildTriggerSection());
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

    // 요청 사양: 스피커 통화·보호자 연락·보호자 메일 세 개만 남기고 나머지(대상자 전화,
    // 119 모의 신고, 재확인 예약, 이벤트 취소)는 제거한다.
    //
    // guardian(readOnly_)은 "영상 열람"만 클릭 가능해야 한다(요청) — 이 세 개도 버튼으로
    // 넣지 않고 같은 문구를 그냥 텍스트(QLabel)로만 보여준다. speakerBtn_/
    // guardianContactBtn_/guardianMailBtn_은 이 모드에선 안 만들어지므로 nullptr로 남고,
    // 그 값을 쓰는 handleSpeakerCallClicked 등은 버튼이 없어 애초에 호출될 일이 없다
    // (callActive_도 항상 false로 유지됨).
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
        addTextCell(QStringLiteral("보호자 연락처"), 0, 1);
        addTextCell(QStringLiteral("보호자 메일"), 1, 0, 1, 2);
        layout->addLayout(grid);
        return wrap;
    }

    speakerBtn_ =
        new QPushButton(SimpleIcons::speakerphone(Tokens::TEXT_INVERSE, 12), QStringLiteral("스피커 통화"), wrap);
    speakerBtn_->setProperty("variant", "primary");
    speakerBtn_->setProperty("size", "s");
    connect(speakerBtn_, &QPushButton::clicked, this, &EventDetailModal::handleSpeakerCallClicked);
    grid->addWidget(speakerBtn_, 0, 0);

    guardianContactBtn_ =
        new QPushButton(SimpleIcons::users(Tokens::TEXT_PRIMARY, 12), QStringLiteral("보호자 연락처"), wrap);
    guardianContactBtn_->setProperty("variant", "secondary");
    guardianContactBtn_->setProperty("size", "s");
    connect(guardianContactBtn_, &QPushButton::clicked, this, &EventDetailModal::handleGuardianContactClicked);
    grid->addWidget(guardianContactBtn_, 0, 1);

    guardianMailBtn_ =
        new QPushButton(SimpleIcons::message(Tokens::TEXT_PRIMARY, 12), QStringLiteral("보호자 메일"), wrap);
    guardianMailBtn_->setProperty("variant", "secondary");
    guardianMailBtn_->setProperty("size", "s");
    connect(guardianMailBtn_, &QPushButton::clicked, this, &EventDetailModal::handleGuardianMailClicked);
    // 2열 그리드 유지(한 줄에 3개를 다 넣으면 버튼 폭이 좁아져 "보호자 연락"/"보호자
    // 메일" 글자가 잘렸다) — 마지막 버튼만 2칸을 차지해 아래 한 줄 전체를 채운다.
    grid->addWidget(guardianMailBtn_, 1, 0, 1, 2);

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
    resolveBtn_->setText(ready ? QStringLiteral("처리 완료")
                                : QStringLiteral("처리 완료 (메모 입력 후 활성화)"));
}

void EventDetailModal::handleResolveClicked() {
    emit eventResolved(row_.eventId);
    accept();
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
            // 연결 시점이 아니라 "통화 종료"를 눌러 통화가 끝난 시점에 "조치"를 체크한다.
            markActionTaken(QStringLiteral("(스피커)"));
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

void EventDetailModal::fetchRecipientIdAndGuardians(
    std::function<void(bool ok, int recipientId, const QVector<ApiClient::GuardianInfo>& guardians,
                        const QString& err)>
        callback) {
    if (row_.recipientName.isEmpty()) {
        callback(false, 0, {}, QStringLiteral("이 이벤트에는 연결된 대상자가 없습니다."));
        return;
    }

    // GET /recipients에는 이름만 있고 id가 없어(EventTableModel::EventRow가 EventInfo::
    // recipientName만 들고 있다 — 서버 이벤트 응답 자체에 recipient_id가 안 실려 있음),
    // 이름으로 먼저 목록에서 id를 찾은 뒤 그 id로 상세(guardians)를 받는 2단계 조회다.
    const QString recipientName = row_.recipientName;
    apiClient_->fetchRecipients([this, recipientName, callback](bool ok,
                                                                  const QVector<ApiClient::RecipientInfo>& items) {
        int recipientId = 0;
        if (ok) {
            for (const auto& r : items) {
                if (r.name == recipientName) {
                    recipientId = r.id;
                    break;
                }
            }
        }
        if (recipientId <= 0) {
            callback(false, 0, {}, QStringLiteral("대상자 정보를 불러오지 못했습니다."));
            return;
        }
        apiClient_->fetchRecipientDetail(
            recipientId, [callback, recipientId](bool ok2, const ApiClient::RecipientDetailInfo& detail,
                                                   const QString& err) {
                if (!ok2) {
                    callback(false, recipientId, {},
                             err.isEmpty() ? QStringLiteral("보호자 정보를 불러오지 못했습니다.") : err);
                    return;
                }
                callback(true, recipientId, detail.guardians, QString());
            });
    });
}

void EventDetailModal::handleGuardianContactClicked() {
    guardianContactBtn_->setEnabled(false);
    fetchRecipientIdAndGuardians([this](bool ok, int, const QVector<ApiClient::GuardianInfo>& guardians,
                                         const QString& err) {
        guardianContactBtn_->setEnabled(true);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("보호자 연락처"), err);
            return;
        }
        showGuardianContactPopover(guardians);
    });
}

void EventDetailModal::handleGuardianMailClicked() {
    guardianMailBtn_->setEnabled(false);
    fetchRecipientIdAndGuardians([this](bool ok, int recipientId, const QVector<ApiClient::GuardianInfo>& guardians,
                                         const QString& err) {
        guardianMailBtn_->setEnabled(true);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("보호자 메일"), err);
            return;
        }
        // guardians는 서버가 이미 priority 오름차순으로 내려준다(recipient.py의
        // GET /recipients/{id} SELECT ... ORDER BY priority) — 그대로 첫 번째가 1순위.
        const QString defaultEmail = guardians.isEmpty() ? QString() : guardians.first().email;
        auto* dialog = new GuardianMailDialog(recipientId, row_.recipientName, defaultEmail, this);
        const bool sent = dialog->exec() == QDialog::Accepted;
        dialog->deleteLater();
        if (sent) markActionTaken(QStringLiteral("(메일)"));
    });
}

void EventDetailModal::showGuardianContactPopover(const QVector<ApiClient::GuardianInfo>& guardians) {
    // WaypointPopover(P6)와 같은 패턴 — Qt::Popup이라 바깥을 클릭하면 자동으로 닫힌다.
    //
    // 바깥(popup)과 안쪽(panel)을 분리해야 한다: WA_TranslucentBackground를 준 최상위
    // 위젯에 QGraphicsDropShadowEffect까지 같이 걸면 오프스크린 합성 과정에서 스타일시트
    // 배경(흰 카드)이 통째로 사라지는 Qt 버그가 있다(그림자+투명배경을 같은 위젯에 적용
    // 시 발생). 바깥은 그림자 없는 순수 투명 셸로 두고, 흰 카드+그림자는 안쪽 QFrame
    // 하나에만 몰아서 해결한다.
    auto* popup = new QWidget(this);
    popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_TranslucentBackground);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    // 그림자(blur=48, dy=12)가 번질 여백을 팝업 창 자체에 남겨둬야 한다 — 마진이 0이면
    // panel과 popup 크기가 똑같아져서 그림자가 창 경계에 그대로 잘려 특히 아래쪽 모서리에
    // 흐릿한 회색 사각 잔상으로 남는다.
    constexpr int kShadowMarginSide = 30;
    constexpr int kShadowMarginTop = 30;
    constexpr int kShadowMarginBottom = 30 + 12;  // + SHADOW_LG.dy
    auto* outerLayout = new QVBoxLayout(popup);
    outerLayout->setContentsMargins(kShadowMarginSide, kShadowMarginTop, kShadowMarginSide, kShadowMarginBottom);

    auto* panel = new QFrame(popup);
    panel->setObjectName(QStringLiteral("mappingStatusPanel"));  // 흰 카드 + 테두리 재사용
    panel->setMinimumWidth(220);
    panel->setMaximumWidth(220);
    outerLayout->addWidget(panel);

    auto* shadow = new QGraphicsDropShadowEffect(panel);
    shadow->setBlurRadius(Tokens::SHADOW_LG.blur);
    shadow->setOffset(0, Tokens::SHADOW_LG.dy);
    shadow->setColor(Tokens::SHADOW_LG.color);
    panel->setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("보호자 연락처 · %1").arg(row_.recipientName), panel);
    title->setStyleSheet(
        QStringLiteral("font-size:11px; font-weight:500; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
    layout->addWidget(title);

    if (guardians.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("등록된 보호자가 없습니다."), panel);
        empty->setProperty("role", "secondary");
        layout->addWidget(empty);
    } else {
        // 대상자 상세 모달(P3RecipientDetailDialog)과 같은 순위(priority) 오름차순.
        QVector<ApiClient::GuardianInfo> sorted = guardians;
        std::sort(sorted.begin(), sorted.end(),
                  [](const ApiClient::GuardianInfo& a, const ApiClient::GuardianInfo& b) {
                      return a.priority < b.priority;
                  });
        for (const ApiClient::GuardianInfo& g : sorted) {
            const QString relationText = g.relation.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(g.relation);
            const QString notifyText = g.notifyEnabled ? QString() : QStringLiteral(" · 알림 꺼짐");
            auto* nameLabel =
                new QLabel(QStringLiteral("%1순위 · %2%3%4").arg(g.priority).arg(g.name, relationText, notifyText),
                           panel);
            nameLabel->setStyleSheet(
                QStringLiteral("font-size:12px; font-weight:500; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
            layout->addWidget(nameLabel);

            auto* phoneLabel = new QLabel(g.phone.isEmpty() ? QStringLiteral("전화번호 없음") : g.phone, panel);
            phoneLabel->setProperty("role", "secondary");
            phoneLabel->setStyleSheet(QStringLiteral("font-family:'%1'; margin-bottom:4px;").arg(Tokens::Font::MONO));
            layout->addWidget(phoneLabel);
        }
    }

    popup->adjustSize();
    // panel은 popup 안에서 (kShadowMarginSide, kShadowMarginTop)만큼 안쪽에 있으므로,
    // panel의 좌상단이 버튼 바로 아래에 오도록 그 마진만큼 빼서 popup 위치를 잡는다.
    const QPoint anchor = guardianContactBtn_->mapToGlobal(QPoint(0, guardianContactBtn_->height() + 4));
    popup->move(anchor - QPoint(kShadowMarginSide, kShadowMarginTop));
    popup->show();
}
