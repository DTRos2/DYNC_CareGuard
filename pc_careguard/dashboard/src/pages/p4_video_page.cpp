#include "p4_video_page.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QDateEdit>
#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QPainter>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoWidget>

#include "net/server_config.h"
#include "net/ws_client.h"
#include "theme/calendar_style.h"
#include "theme/tokens.h"
#include "widgets/careguard_date_edit.h"
#include "widgets/live_search_line_edit.h"
#include "widgets/simple_icons.h"

QVector<VideoClip> mockVideoClips() {
    return {
        {QStringLiteral("R-01"), QStringLiteral("김순자"), QStringLiteral("SOS 호출"),
         QStringLiteral("긴급"), VideoTagStyle::Emergency, QStringLiteral("08.26 14:29"),
         QStringLiteral("EV-260826-014"), QStringLiteral("02:14"), /*viewCount=*/1,
         /*expireDays=*/30, QStringLiteral("행복빌라 302 거실"),
         /*viewHistory=*/{QStringLiteral("14:30:12 · 도희정 · 사유: 긴급 경보 초기 상황 확인")}},

        {QStringLiteral("R-02"), QStringLiteral("박영호"), QStringLiteral("복약 미수령"),
         QStringLiteral("주의"), VideoTagStyle::Attention, QStringLiteral("08.26 12:41"),
         QStringLiteral("EV-260826-011"), QStringLiteral("01:38"), /*viewCount=*/0,
         /*expireDays=*/30, QStringLiteral("스카이빌 704호"), /*viewHistory=*/{}},

        {QStringLiteral("R-03"), QStringLiteral("이말순"), QStringLiteral("순찰 스냅샷"),
         QStringLiteral("순찰"), VideoTagStyle::Neutral, QStringLiteral("08.26 10:04"),
         Tokens::Format::EMPTY, QStringLiteral("00:22"), /*viewCount=*/0,
         /*expireDays=*/5, QString(), /*viewHistory=*/{}},

        {QStringLiteral("R-03"), QStringLiteral("이말순"), QStringLiteral("음성 무응답"),
         QStringLiteral("긴급"), VideoTagStyle::Emergency, QStringLiteral("08.25 16:12"),
         QStringLiteral("EV-260825-041"), QStringLiteral("03:05"), /*viewCount=*/3,
         /*expireDays=*/29, QStringLiteral("은빛노인정 201호"),
         /*viewHistory=*/
         {QStringLiteral("16:45:11 · 도희정 · 사유: 종결 보고"),
          QStringLiteral("16:20:03 · 도희정 · 사유: 재확인"),
          QStringLiteral("16:15:40 · 도희정 · 사유: 무응답 확인")}},

        {QStringLiteral("R-01"), QStringLiteral("김순자"), QStringLiteral("정기 순찰"),
         QStringLiteral("순찰"), VideoTagStyle::Neutral, QStringLiteral("08.25 09:30"),
         Tokens::Format::EMPTY, QStringLiteral("00:47"), /*viewCount=*/1,
         /*expireDays=*/6, QString(),
         /*viewHistory=*/{QStringLiteral("09:31:05 · 도희정 · 사유: 정기 점검 확인")}},
    };
}

namespace {

// state 프로퍼티 변경을 QSS에 반영하려면 폴리시를 다시 태워야 한다.
void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

// server/app/media.py GET /video-events 응답을 VideoClip으로 매핑하는 헬퍼들.

// 서버는 마이크로초(6자리)까지 주는데 Qt::ISODateWithMs는 밀리초(3자리)까지만 인식해서 잘라낸다.
QDateTime parseIsoDateTime(const QString& raw) {
    if (raw.isEmpty()) return QDateTime();
    QString trimmed = raw;
    static const QRegularExpression fracRe(QStringLiteral("\\.(\\d{3})\\d*"));
    trimmed.replace(fracRe, QStringLiteral(".\\1"));
    QDateTime dt = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (!dt.isValid()) dt = QDateTime::fromString(trimmed, Qt::ISODate);
    return dt;
}

VideoTagStyle tagStyleFromString(const QString& style) {
    if (style == QStringLiteral("emergency")) return VideoTagStyle::Emergency;
    if (style == QStringLiteral("attention")) return VideoTagStyle::Attention;
    return VideoTagStyle::Neutral;
}

// "MM:SS" 형식. duration_sec이 없거나 음수면 "00:00".
QString formatDurationLabel(int durationSec) {
    const int sec = qMax(0, durationSec);
    return QStringLiteral("%1:%2")
        .arg(sec / 60, 2, 10, QChar('0'))
        .arg(sec % 60, 2, 10, QChar('0'));
}

// view_history 배열을 "HH:MM:SS · 열람자 · 사유: ..." 문자열 리스트로 바꾼다. 두 API 응답이
// 같은 모양이라 videoClipFromJson·recordVideoView에서 공용으로 쓴다.
QStringList parseViewHistoryArray(const QJsonArray& history) {
    QStringList result;
    for (const QJsonValue& entry : history) {
        const QJsonObject h = entry.toObject();
        const QDateTime occurredAt = parseIsoDateTime(h.value("occurred_at").toString());
        const QString loginId = h.value("login_id").toString();
        const QString reason = h.value("reason").toString();
        result.append(
            QStringLiteral("%1 · %2 · 사유: %3")
                .arg(occurredAt.isValid() ? occurredAt.toString(QStringLiteral("HH:mm:ss"))
                                           : Tokens::Format::EMPTY,
                     loginId.isEmpty() ? QStringLiteral("알 수 없음") : loginId, reason));
    }
    return result;
}

// GET /video-events 응답 한 건을 VideoClip으로 매핑한다.
VideoClip videoClipFromJson(const QJsonObject& obj) {
    VideoClip clip;
    clip.cameraRecordingId = obj.value("camera_recording_id").toInt();
    clip.robotId = obj.value("robot_code").toString();
    clip.recipientName = obj.value("recipient_name").toString();

    const QString summary = obj.value("summary").toString();
    clip.eventLabel = summary.isEmpty() ? QStringLiteral("순찰 스냅샷") : summary;

    clip.tagText = obj.value("tag_text").toString();
    clip.tagStyle = tagStyleFromString(obj.value("tag_style").toString());

    const QDateTime capturedAt = parseIsoDateTime(obj.value("captured_at").toString());
    clip.capturedAt = capturedAt.isValid() ? capturedAt.toString(QStringLiteral("MM.dd HH:mm"))
                                            : Tokens::Format::EMPTY;
    clip.capturedAtRaw = capturedAt;  // 기간 필터(matchesFilters())가 원본 날짜로 계산할 때 사용

    const QString eventUid = obj.value("event_uid").toString();
    clip.eventId = eventUid.isEmpty() ? Tokens::Format::EMPTY : eventUid;

    clip.durationLabel = formatDurationLabel(obj.value("duration_sec").toInt());
    clip.viewCount = obj.value("view_count").toInt();

    // TODO(team): retention_until이 아직 항상 null이라 임시로 30일 기본값을 쓴다.
    const QDateTime retentionUntil = parseIsoDateTime(obj.value("retention_until").toString());
    clip.expireDays = retentionUntil.isValid()
                           ? qMax(0, static_cast<int>(QDateTime::currentDateTime().daysTo(retentionUntil)))
                           : 30;

    clip.location = obj.value("location").toString();
    // video_url은 상대 경로("/media/files/{id}") — serverUrl_은 handleVideoClipsReply()에서 붙인다.
    clip.videoUrl = obj.value("video_url").toString();
    clip.viewHistory = parseViewHistoryArray(obj.value("view_history").toArray());

    return clip;
}

}  // namespace

// 클릭을 신호로 내보내는 카드. QFrame엔 기본 clicked()가 없어서 이렇게 감싼다.
class ClickableCard : public QFrame {
    Q_OBJECT
public:
    using QFrame::QFrame;

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) emit clicked();
        QFrame::mousePressEvent(event);
    }
};

// 보관 정책 안내 모달. VideoPlayerDialog와 동일한 헤더+본문 패턴을 쓰는 단순 버전.
class RetentionPolicyDialog : public QDialog {
    Q_OBJECT
public:
    explicit RetentionPolicyDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setModal(true);
        setFixedWidth(Tokens::Layout::MODAL_W_M);
        // setWindowTitle(QStringLiteral("영상 보관 정책"));

        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        outer->addWidget(buildHeader());
        outer->addWidget(buildBody(), /*stretch=*/1);
        outer->addWidget(buildFooter());
    }

private:
    QWidget* buildHeader() {
        auto* header = new QFrame(this);
        header->setObjectName("modalHeader");
        header->setFixedHeight(46);
        auto* layout = new QHBoxLayout(header);
        layout->setContentsMargins(16, 0, 16, 0);

        auto* title = new QLabel(QStringLiteral("영상 보관 정책"), header);
        QFont titleFont = title->font();
        titleFont.setPixelSize(Tokens::Font::H3);
        titleFont.setWeight(QFont::DemiBold);
        title->setFont(titleFont);
        layout->addWidget(title);
        layout->addStretch();

        // auto* closeBtn = new QPushButton(header);
        // closeBtn->setProperty("variant", "ghost");
        // closeBtn->setProperty("size", "s");
        // closeBtn->setIcon(SimpleIcons::close(Tokens::TEXT_SECONDARY, 16));
        // closeBtn->setFixedWidth(28);
        // connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
        // layout->addWidget(closeBtn);

        return header;
    }

    QWidget* buildBody() {
        auto* body = new QWidget(this);
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(20, 16, 20, 16);
        layout->setSpacing(12);

        auto addParagraph = [layout, body](const QString& text) {
            auto* label = new QLabel(text, body);
            label->setWordWrap(true);
            layout->addWidget(label);
        };

        addParagraph(QStringLiteral(
            "로봇은 상시 녹화하지 않습니다. 긴급, 주의, 순찰 등 이벤트가 발생한 시점의 "
            "전후 구간만 클립으로 저장되며, 평상시 상황은 영상으로 남지 않습니다."));
        addParagraph(QStringLiteral(
            "이벤트와 연동되어 저장된 영상은 30일간 보관됩니다. 사건과 직접 관련이 없는 "
            "일반 영상(예: 순찰 스냅샷)은 7일간 보관되며, 두 경우 모두 기간이 지나면 "
            "자동으로 삭제됩니다."));
        addParagraph(QStringLiteral(
            "저장된 영상을 열람하려면 반드시 열람 사유를 입력해야 하며, 모든 열람 기록"
            "(열람자, 시각, 사유)은 별도로 남습니다. 사유 없이는 영상을 재생할 수 없습니다."));

        return body;
    }

    QWidget* buildFooter() {
        auto* footer = new QFrame(this);
        footer->setObjectName("modalFooter");
        auto* layout = new QHBoxLayout(footer);
        layout->setContentsMargins(20, 0, 20, 0);
        layout->addStretch();

        auto* closeBtn = new QPushButton(QStringLiteral("닫기"), footer);
        closeBtn->setProperty("variant", "secondary");
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
        layout->addWidget(closeBtn);

        return footer;
    }
};

namespace {

// 카드 그리드 열 수 계산 기준값 (columns_는 resizeEvent()가 갱신).
constexpr int kMinColumns = 1;
constexpr int kCardMinWidth = 280;  // 카드 1개가 이보다 좁아지면 다음 줄로 넘어간다

// 재생 모달 배속 버튼이 클릭할 때마다 돌아가며 쓰는 값 목록. 끝까지 가면 처음으로 돌아간다.
constexpr double kPlaybackRates[] = {0.5, 1.0, 1.25, 1.5, 2.0};
constexpr const char* kPlaybackRateLabels[] = {"0.5×", "1.0×", "1.25×", "1.5×", "2.0×"};
constexpr int kPlaybackRateCount = sizeof(kPlaybackRates) / sizeof(kPlaybackRates[0]);
constexpr int kDefaultPlaybackRateIndex = 1;  // 1.0× — QMediaPlayer 기본 배속과 동일

// 상태 배지. "순찰"은 별도 레벨이 없어 OFFLINE 색(#F1F4F6/#5A6774)을 재사용한다.
QLabel* makeBadge(const VideoClip& clip) {
    auto* badge = new QLabel(clip.tagText);
    badge->setObjectName("badge");
    switch (clip.tagStyle) {
        case VideoTagStyle::Emergency:
            badge->setProperty("level", "emergencyFill");  // 썸네일용 본색 채움
            break;
        case VideoTagStyle::Attention:
            badge->setProperty("level", "attention");
            break;
        case VideoTagStyle::Neutral:
            badge->setProperty("level", "offline");
            break;
    }
    return badge;
}

// 카드 상단 썸네일: 배지(좌상단) / 재생 아이콘(중앙) / 길이(우하단) 3단 구성.
QWidget* makeThumbnail(const VideoClip& clip) {
    auto* thumb = new QFrame;
    thumb->setObjectName("videoThumb");
    thumb->setFixedHeight(96);

    auto* layout = new QVBoxLayout(thumb);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);

    auto* topRow = new QHBoxLayout();
    topRow->addWidget(makeBadge(clip));
    topRow->addStretch();
    layout->addLayout(topRow);

    auto* playIcon = new QLabel;
    playIcon->setPixmap(SimpleIcons::play(QColor(255, 255, 255, 217), 22).pixmap(22, 22));
    playIcon->setAlignment(Qt::AlignCenter);
    layout->addWidget(playIcon, /*stretch=*/1);

    auto* bottomRow = new QHBoxLayout();
    bottomRow->addStretch();
    auto* duration = new QLabel(clip.durationLabel);
    duration->setObjectName("videoDuration");
    bottomRow->addWidget(duration);
    layout->addLayout(bottomRow);

    return thumb;
}

// 카드 하단 정보: 제목(로봇·대상자·이벤트) / 촬영시각·event_id(§10 MONO) / 열람·만료.
QWidget* makeInfoPanel(const VideoClip& clip) {
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(10, 9, 10, 9);
    layout->setSpacing(3);

    auto* title = new QLabel(
        QStringLiteral("%1 · %2 · %3").arg(clip.robotId, clip.recipientName, clip.eventLabel));
    QFont titleFont = title->font();
    titleFont.setPixelSize(Tokens::Font::LABEL);
    titleFont.setWeight(QFont::Medium);
    title->setFont(titleFont);
    // 줄바꿈이 없으면 라벨의 최소 너비가 텍스트 전체 길이가 돼버려, 이벤트 설명이 긴
    // 카드가 속한 컬럼만 setColumnStretch()의 균등 배분과 무관하게 더 넓어진다
    // (1/2/3열 카드 가로 길이가 서로 달라 보이는 원인). wordWrap으로 최소 너비를
    // "가장 긴 단어" 수준까지 낮춰 컬럼 폭이 고르게 나뉘게 한다.
    title->setWordWrap(true);
    layout->addWidget(title);

    auto* meta = new QLabel(QStringLiteral("%1 · %2").arg(clip.capturedAt, clip.eventId));
    meta->setProperty("role", "mono");
    layout->addWidget(meta);

    auto* statsRow = new QHBoxLayout();
    statsRow->setSpacing(6);
    if (clip.viewCount > 0) {
        auto* eyeIcon = new QLabel;
        eyeIcon->setPixmap(SimpleIcons::eye(Tokens::TEXT_MUTED, 11).pixmap(11, 11));
        statsRow->addWidget(eyeIcon);
        auto* viewLabel = new QLabel(QStringLiteral("열람 %1회").arg(clip.viewCount));
        viewLabel->setProperty("role", "muted");
        statsRow->addWidget(viewLabel);
    } else {
        auto* viewLabel = new QLabel(QStringLiteral("열람 없음"));
        viewLabel->setProperty("role", "muted");
        statsRow->addWidget(viewLabel);
    }
    statsRow->addStretch();

    auto* expireLabel = new QLabel(QStringLiteral("만료 D-%1").arg(clip.expireDays));
    expireLabel->setProperty("role", clip.expireDays <= 7 ? "warning" : "muted");  // §10
    statsRow->addWidget(expireLabel);
    layout->addLayout(statsRow);

    return panel;
}

ClickableCard* makeVideoCard(const VideoClip& clip, QWidget* parent) {
    auto* card = new ClickableCard(parent);
    card->setObjectName("card");
    card->setCursor(Qt::PointingHandCursor);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makeThumbnail(clip));
    layout->addWidget(makeInfoPanel(clip));
    return card;
}

QFrame* makeEmptySlot(QWidget* parent) {
    auto* slot = new QFrame(parent);
    slot->setObjectName("videoEmptySlot");
    slot->setMinimumHeight(150);
    auto* layout = new QVBoxLayout(slot);
    auto* label = new QLabel(QStringLiteral("이벤트 발생 시\n자동으로 추가됩니다"));
    label->setProperty("role", "muted");
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    return slot;
}

// QSS는 셀 단위로만 칠해져서, 컬럼 틈이 있는 이 테이블에선 선택된 행이 8개로 끊겨
// 보인다. 행 전체를 감싸는 테두리 하나를 paintEvent에서 직접 그려서 해결한다.
class RowBorderTableWidget : public QTableWidget {
public:
    using QTableWidget::QTableWidget;

protected:
    void paintEvent(QPaintEvent* event) override {
        QTableWidget::paintEvent(event);  // 셀 내용(텍스트 등)을 먼저 평소대로 그린다

        const QModelIndexList selectedRows = selectionModel()->selectedRows();
        if (selectedRows.isEmpty()) return;
        const int row = selectedRows.first().row();

        // 첫 컬럼과 마지막 컬럼의 뷰포트 좌표를 합쳐 행 전체를 감싸는 사각형을 구한다.
        const QRect rowRect = visualRect(model()->index(row, 0))
                                   .united(visualRect(model()->index(row, columnCount() - 1)));
        if (rowRect.isEmpty()) return;

        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor("#0E9384"));  // Tokens::ACCENT
        pen.setWidthF(1.5);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rowRect).adjusted(0.75, 0.75, -0.75, -0.75), 6, 6);
    }
};

}  // namespace

// ============================================================
// 재생 모달. Q_OBJECT 클래스를 헤더 없이 .cpp에 두고 파일 끝 #include "p4_video_page.moc"로
// AUTOMOC이 처리하게 하는 패턴을 쓴다.
// ============================================================
class VideoPlayerDialog : public QDialog {
    Q_OBJECT
public:
    // hideEventLink: P2 이벤트 상세 모달에서 "사유 입력 후 열람"으로 이 모달을 띄운
    // 경우 true. 그 경우엔 이 모달 바로 밑에 이미 그 이벤트의 상세 모달이 떠 있으므로
    // "연결 이벤트" 카드의 "이벤트 상세로 이동" 버튼을 감춘다 — 안 감추면 눌렀을 때
    // 새 이벤트 상세 모달이 하나 더 뜨고, 이 재생 모달만 accept()로 닫혀서 밑에 있던
    // 원래 모달까지 X를 두 번 눌러야 닫히는 문제가 있었다.
    VideoPlayerDialog(const VideoClip& clip, QWidget* parent = nullptr, bool hideEventLink = false)
        : QDialog(parent), clip_(clip), hideEventLink_(hideEventLink) {
        setModal(true);
        // 창이 M 사이즈보다 좁으면 모달도 S 사이즈까지 줄어든다.
        int modalWidth = Tokens::Layout::MODAL_W_M;
        if (parent) {
            modalWidth = qMin(modalWidth, parent->width() - 2 * Tokens::Layout::PAGE_PADDING);
        }
        setFixedWidth(qMax(modalWidth, Tokens::Layout::MODAL_W_S));
        setWindowTitle(QStringLiteral("영상 재생"));

        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        outer->addWidget(buildHeader());
        outer->addWidget(buildBody());

        // §9: 잠금 카운트다운
        remainingSeconds_ = 10 * 60 ;
        updateCountdownLabel();
        countdownTimer_ = new QTimer(this);
        connect(countdownTimer_, &QTimer::timeout, this, [this] {
            if (--remainingSeconds_ <= 0) {
                countdownTimer_->stop();
                expireSession();
            }
            updateCountdownLabel();
        });
        countdownTimer_->start(1000);
    }

    bool wasViewed() const { return viewed_; }
    QString enteredReason() const { return reasonInput_->text().trimmed(); }

signals:
    // "이벤트 상세로 이동" 클릭. P4VideoPage::openVideoPlayer()가 자기 시그널로 그대로 이어
    // 내보낸다 (다이얼로그가 스택 로컬 변수라 MainWindow가 직접 연결할 수 없어서).
    void eventDetailRequested(const QString& eventUid);

private:
    QWidget* buildHeader() {
        auto* header = new QFrame(this);
        header->setObjectName("modalHeader");
        header->setFixedHeight(46);
        auto* layout = new QHBoxLayout(header);
        layout->setContentsMargins(16, 0, 16, 0);
        layout->setSpacing(8);

        if (clip_.tagStyle != VideoTagStyle::Neutral) {
            layout->addWidget(makeBadge(clip_));
        }

        auto* title =
            new QLabel(QStringLiteral("%1 전방 카메라 · 이벤트 전후 구간").arg(clip_.robotId), header);
        QFont titleFont = title->font();
        titleFont.setPixelSize(13);
        titleFont.setWeight(QFont::Medium);
        title->setFont(titleFont);
        layout->addWidget(title);

        auto* meta =
            new QLabel(QStringLiteral("%1 · %2").arg(clip_.capturedAt, clip_.eventId), header);
        meta->setProperty("role", "mono");
        layout->addWidget(meta);

        layout->addStretch();

        // auto* closeBtn = new QPushButton(header);
        // closeBtn->setProperty("variant", "ghost");
        // closeBtn->setProperty("size", "s");
        // closeBtn->setIcon(SimpleIcons::close(Tokens::TEXT_SECONDARY, 16));
        // closeBtn->setFixedWidth(28);
        // connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
        // layout->addWidget(closeBtn);

        return header;
    }


    QWidget* buildBody() {
        auto* body = new QWidget(this);
        auto* layout = new QHBoxLayout(body);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(12);
        layout->addWidget(buildLeftColumn(), /*stretch=*/7);
        layout->addWidget(buildRightColumn(), /*stretch=*/5);
        return body;
    }

    QWidget* buildLeftColumn() {
        auto* col = new QWidget(this);
        leftColumnLayout_ = new QVBoxLayout(col);
        leftColumnLayout_->setContentsMargins(0, 0, 0, 0);
        leftColumnLayout_->setSpacing(8);

        // videoArea_는 멤버로 들고 있다가 전체화면 진입 시 fullscreenHost_로 옮긴다
        // (toggleFullScreen() 참고).
        videoArea_ = buildVideoArea();
        leftColumnLayout_->addWidget(videoArea_, /*stretch=*/1);
        leftColumnLayout_->addLayout(buildPlaybackRow());
        return col;
    }

    QWidget* buildVideoArea() {
        auto* area = new QFrame(this);
        area->setObjectName("videoPlayerArea");
        area->setMinimumHeight(320);  // §4-4 M 모달 기준 200 → 심박 타임라인 제거분만큼 확대
        area->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        auto* layout = new QVBoxLayout(area);
        layout->setContentsMargins(8, 8, 8, 8);

        auto* topRow = new QHBoxLayout();
        topRow->setSpacing(6);
        topRow->addStretch();

        countdownLabel_ = new QLabel(area);
        countdownLabel_->setObjectName("videoCountdown");
        topRow->addWidget(countdownLabel_);
        layout->addLayout(topRow);

        fullscreenBtn_ = new QPushButton(area);
        fullscreenBtn_->setProperty("variant", "ghost");
        fullscreenBtn_->setProperty("size", "s");
        fullscreenBtn_->setFixedWidth(28);
        fullscreenBtn_->setIcon(SimpleIcons::expand(Qt::white, 14));
        fullscreenBtn_->setToolTip(QStringLiteral("전체화면"));
        connect(fullscreenBtn_, &QPushButton::clicked, this, &VideoPlayerDialog::toggleFullScreen);
        topRow->addWidget(fullscreenBtn_);



        layout->addStretch();
        auto* centerRow = new QHBoxLayout();
        centerRow->addStretch();
        centerRow->addWidget(buildLockCard());

        // QMediaPlayer가 GET /media/files/{id}를 직접 받아 재생한다. unlock() 전엔 숨김.
        videoWidget_ = new QVideoWidget(area);
        // 영상 해상도는 640×480 고정 — maximumSize로 캡을 걸어 그 이상 커지지 않게 한다.
        videoWidget_->setMinimumSize(320, 240);
        videoWidget_->setMaximumSize(640, 480);
        videoWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        videoWidget_->hide();
        centerRow->addWidget(videoWidget_);

        player_ = new QMediaPlayer(this);
        audioOutput_ = new QAudioOutput(this);
        player_->setAudioOutput(audioOutput_);
        player_->setVideoOutput(videoWidget_);
        player_->setSource(QUrl(clip_.videoUrl));
        connect(player_, &QMediaPlayer::errorOccurred, this,
                [this](QMediaPlayer::Error, const QString& errorString) {
                    videoWidget_->hide();
                    playingLabel_->setText(QStringLiteral("재생 오류: %1").arg(errorString));
                    playingLabel_->show();
                });

        // 재생 오류 전용 — 정상 재생 중엔 videoWidget_가 이 자리를 대신한다.
        playingLabel_ = new QLabel(QString(), area);
        playingLabel_->setObjectName("videoLockLabel");
        playingLabel_->hide();
        centerRow->addWidget(playingLabel_);
        centerRow->addStretch();
        layout->addLayout(centerRow);
        layout->addStretch();

        return area;
    }

    QWidget* buildLockCard() {
        lockCard_ = new QFrame(this);
        lockCard_->setObjectName("videoLockCard");
        // setFixedWidth로 폭을 처음부터 고정해야 wordWrap 높이 계산이 정확해져 라벨이 안 겹친다.
        lockCard_->setFixedWidth(300);
        auto* layout = new QVBoxLayout(lockCard_);
        layout->setContentsMargins(18, 14, 18, 14);
        layout->setSpacing(6);

        auto* lockIcon = new QLabel(lockCard_);
        lockIcon->setPixmap(SimpleIcons::lock(Qt::white, 20).pixmap(20, 20));
        lockIcon->setAlignment(Qt::AlignCenter);
        layout->addWidget(lockIcon);

        lockMessage_ =
            new QLabel(QStringLiteral("열람 사유를 입력해야 재생할 수 있습니다."), lockCard_);
        lockMessage_->setObjectName("videoLockLabel");
        lockMessage_->setAlignment(Qt::AlignCenter);
        lockMessage_->setWordWrap(true);
        layout->addWidget(lockMessage_);

        auto* subMessage =
            new QLabel(QStringLiteral("열람자·시각·사유가 감사 로그에 기록됩니다."), lockCard_);
        subMessage->setProperty("role", "muted");
        subMessage->setAlignment(Qt::AlignCenter);
        subMessage->setWordWrap(true);
        layout->addWidget(subMessage);

        reasonInput_ = new QLineEdit(lockCard_);
        reasonInput_->setFixedHeight(28);
        const QString placeholder = clip_.eventId == Tokens::Format::EMPTY
                                         ? QStringLiteral("예: 정기 점검 확인차 열람")
                                         : QStringLiteral("예: %1 관련 확인 사유").arg(clip_.eventId);
        reasonInput_->setPlaceholderText(placeholder);
        layout->addWidget(reasonInput_);

        playBtn_ = new QPushButton(QStringLiteral("사유 확인 후 재생"), lockCard_);
        playBtn_->setProperty("variant", "primary");
        playBtn_->setProperty("size", "s");
        playBtn_->setEnabled(false);
        connect(playBtn_, &QPushButton::clicked, this, &VideoPlayerDialog::unlock);
        layout->addWidget(playBtn_);

        connect(reasonInput_, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (!viewed_) playBtn_->setEnabled(!text.trimmed().isEmpty());
        });

        return lockCard_;
    }

    QHBoxLayout* buildPlaybackRow() {
        auto* row = new QHBoxLayout();
        row->setSpacing(6);

        // player_는 buildVideoArea()에서 이미 만들어진 뒤다. unlock() 전엔 비활성으로 시작.
        auto makeIconBtn = [row, this](const QIcon& icon, int px, const QString& tooltip) {
            auto* btn = new QPushButton(this);
            btn->setProperty("variant", "ghost");
            btn->setProperty("size", "s");
            btn->setFixedSize(22, 22);
            btn->setIcon(icon);
            btn->setIconSize(QSize(px, px));
            btn->setToolTip(tooltip);
            btn->setEnabled(false);
            row->addWidget(btn);
            return btn;
        };

        playToggleBtn_ =
            makeIconBtn(SimpleIcons::play(Tokens::TEXT_MUTED, 13), 13, QStringLiteral("재생"));
        seekBackBtn_ = makeIconBtn(SimpleIcons::trackPrev(Tokens::TEXT_MUTED, 11), 11,
                                    QStringLiteral("10초 되감기"));
        seekForwardBtn_ = makeIconBtn(SimpleIcons::trackNext(Tokens::TEXT_MUTED, 11), 11,
                                       QStringLiteral("10초 앞으로"));

        connect(playToggleBtn_, &QPushButton::clicked, this, [this] {
            if (player_->playbackState() == QMediaPlayer::PlayingState) {
                player_->pause();
            } else {
                player_->play();
            }
        });
        connect(seekBackBtn_, &QPushButton::clicked, this, [this] {
            player_->setPosition(qMax(qint64(0), player_->position() - 10000));
        });
        connect(seekForwardBtn_, &QPushButton::clicked, this, [this] {
            player_->setPosition(qMin(player_->duration(), player_->position() + 10000));
        });
        connect(player_, &QMediaPlayer::playbackStateChanged, this,
                [this](QMediaPlayer::PlaybackState state) {
                    playToggleBtn_->setIcon(state == QMediaPlayer::PlayingState
                                                 ? SimpleIcons::pause(Tokens::TEXT_MUTED, 13)
                                                 : SimpleIcons::play(Tokens::TEXT_MUTED, 13));
                });
        connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 posMs) {
            timeLabel_->setText(QStringLiteral("%1 / %2").arg(
                formatDurationLabel(static_cast<int>(posMs / 1000)), clip_.durationLabel));
        });

        // 클릭할 때마다 kPlaybackRates를 한 칸씩 돌며 배속을 바꾼다. unlock() 전엔 비활성.
        speedBtn_ = new QPushButton(
            QStringLiteral("배속 %1").arg(QString::fromUtf8(kPlaybackRateLabels[kDefaultPlaybackRateIndex])),
            this);
        speedBtn_->setProperty("variant", "ghost");
        speedBtn_->setProperty("size", "s");
        speedBtn_->setEnabled(false);
        row->addWidget(speedBtn_);
        row->addStretch();

        connect(speedBtn_, &QPushButton::clicked, this, [this] {
            playbackRateIndex_ = (playbackRateIndex_ + 1) % kPlaybackRateCount;
            player_->setPlaybackRate(kPlaybackRates[playbackRateIndex_]);
            speedBtn_->setText(
                QStringLiteral("배속 %1").arg(QString::fromUtf8(kPlaybackRateLabels[playbackRateIndex_])));
        });

        timeLabel_ = new QLabel(QStringLiteral("--:-- / %1").arg(clip_.durationLabel));
        timeLabel_->setProperty("role", "mono");
        row->addWidget(timeLabel_);

        return row;
    }

    QWidget* buildRightColumn() {
        auto* col = new QWidget(this);
        auto* layout = new QVBoxLayout(col);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        if (clip_.eventId != Tokens::Format::EMPTY) {
            layout->addWidget(sectionLabel(QStringLiteral("연결 이벤트")));
            layout->addWidget(buildConnectedEventCard());
            addSpacing(layout, 12);
        }

        layout->addWidget(sectionLabel(QStringLiteral("열람 이력")));
        layout->addWidget(buildHistoryBox());
        addSpacing(layout, 12);

        auto* fileHeaderRow = new QHBoxLayout();
        fileHeaderRow->setSpacing(4);
        auto* fileLabel = sectionLabel(QStringLiteral("파일 작업"));
        fileHeaderRow->addWidget(fileLabel);
        auto* fileNote = new QLabel(QStringLiteral("— 모두 사유 기록"), col);
        fileNote->setProperty("role", "muted");
        fileHeaderRow->addWidget(fileNote);
        fileHeaderRow->addStretch();
        layout->addLayout(fileHeaderRow);

        layout->addWidget(buildFileActionButtons());
        layout->addStretch();

        return col;
    }

    static QLabel* sectionLabel(const QString& text) {
        auto* label = new QLabel(text);
        label->setProperty("role", "secondary");
        QFont f = label->font();
        f.setWeight(QFont::Medium);
        label->setFont(f);
        return label;
    }

    static void addSpacing(QVBoxLayout* layout, int px) {
        layout->addSpacerItem(new QSpacerItem(0, px));
    }

    QWidget* buildConnectedEventCard() {
        auto* card = new QFrame(this);
        card->setObjectName("eventSummaryCard");
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(10, 9, 10, 9);
        layout->setSpacing(3);

        auto* summary =
            new QLabel(QStringLiteral("%1 · %2").arg(clip_.recipientName, clip_.eventLabel), card);
        QFont sf = summary->font();
        sf.setWeight(QFont::Medium);
        summary->setFont(sf);
        // wordWrap 없으면 이 라벨의 최소 너비가 텍스트 전체 길이가 돼버려, 다이얼로그가
        // setFixedWidth로 고정된 상태에서 오른쪽 컬럼(stretch 5) 최소 너비를 밀어올리고
        // 그만큼 왼쪽 영상 영역(stretch 7)이 줄어든다 — 클립마다 재생 화면 좌/우 비율이
        // 달라 보이는 원인. makeInfoPanel() title과 같은 이유·같은 처방.
        summary->setWordWrap(true);
        layout->addWidget(summary);  // 기본 QWidget 색(TEXT_PRIMARY) 그대로 — 와이어프레임 제목은 검정

        const QString status = clip_.tagStyle == VideoTagStyle::Emergency ? QStringLiteral("대응 중")
                                                                            : QStringLiteral("완료");
        auto* subtitle = new QLabel(
            QStringLiteral("%1 · %2 · 담당 도희정").arg(clip_.location, status), card);
        subtitle->setProperty("role", "secondary");  // TEXT_SECONDARY — 와이어프레임 부제는 회색
        subtitle->setWordWrap(true);  // summary와 같은 이유
        layout->addWidget(subtitle);

        // hideEventLink_이면(P2 이벤트 상세에서 열람으로 들어온 경우) 이 버튼 자체를 안
        // 만든다 — 생성자 주석 참고.
        if (!hideEventLink_) {
            auto* link = new QPushButton(QStringLiteral("이벤트 상세로 이동 →"), card);
            link->setObjectName("linkBtn");
            connect(link, &QPushButton::clicked, this, [this] {
                emit eventDetailRequested(clip_.eventId);
                accept();  // P2로 화면이 전환되니 이 재생 모달은 닫는다
            });
            layout->addWidget(link, 0, Qt::AlignLeft);
        }

        return card;
    }

    // 목록이 길어져도 다이얼로그가 늘어나지 않도록 스크롤 영역 높이를 고정한다.
    static constexpr int kHistoryVisibleHeight = 132;  // 대략 항목 3개 분량

    QWidget* buildHistoryBox() {
        auto* box = new QFrame(this);
        box->setObjectName("videoTimelineBox");  // 동일한 보더+라운드 스타일 재사용
        auto* layout = new QVBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        if (clip_.viewHistory.isEmpty()) {
            auto* empty = new QLabel(QStringLiteral("열람 이력 없음"), box);
            empty->setProperty("role", "muted");
            empty->setContentsMargins(10, 6, 10, 6);
            layout->addWidget(empty);
        } else {
            auto* scrollArea = new QScrollArea(box);
            scrollArea->setObjectName("videoHistoryScroll");
            scrollArea->setFrameShape(QFrame::NoFrame);          // 바깥 box 테두리와 겹치지 않게
            scrollArea->setWidgetResizable(true);
            scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            scrollArea->setFixedHeight(kHistoryVisibleHeight);

            auto* list = new QWidget(scrollArea);
            auto* listLayout = new QVBoxLayout(list);
            listLayout->setContentsMargins(0, 0, 0, 0);
            listLayout->setSpacing(0);
            for (const QString& entry : clip_.viewHistory) {
                listLayout->addWidget(buildHistoryRow(entry));
            }
            listLayout->addStretch();  // 항목이 몇 개 안 될 때 위쪽으로 붙여둔다

            scrollArea->setWidget(list);
            layout->addWidget(scrollArea);
        }
        auto* footer = new QLabel(
            clip_.viewHistory.isEmpty() ? QString() : QStringLiteral("이후 열람 없음"), box);
        if (!clip_.viewHistory.isEmpty()) {
            footer->setProperty("role", "muted");
            footer->setContentsMargins(10, 6, 10, 6);
            layout->addWidget(footer);
        }
        return box;
    }

    QWidget* buildHistoryRow(const QString& entry) {
        // entry 형식: "HH:MM:SS · 열람자 · 사유: ..."
        const int sep1 = entry.indexOf(QStringLiteral(" · "));
        const QString time = sep1 >= 0 ? entry.left(sep1) : entry;
        const QString rest = sep1 >= 0 ? entry.mid(sep1 + 3) : QString();

        const int sep2 = rest.indexOf(QStringLiteral(" · "));
        const QString viewer = sep2 >= 0 ? rest.left(sep2) : QString();
        const QString reason = sep2 >= 0 ? rest.mid(sep2 + 3) : rest;

        auto* row = new QWidget(this);
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(10, 6, 10, 6);
        layout->setSpacing(2);

        auto* line1 = new QLabel(QStringLiteral("%1  %2").arg(viewer, time), row);
        QFont f = line1->font();
        f.setPixelSize(Tokens::Font::CAPTION + 1);
        line1->setFont(f);
        layout->addWidget(line1);

        auto* line2 = new QLabel(reason, row);
        line2->setProperty("role", "muted");
        line2->setWordWrap(true);  // 사람이 직접 입력하는 자유 텍스트라 길이가 들쭉날쭉함 — summary와 같은 이유
        layout->addWidget(line2);

        return row;
    }

    QWidget* buildFileActionButtons() {
        auto* box = new QWidget(this);
        auto* layout = new QVBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        // TODO(team): 실제 다운로드/공유/삭제 로직은 server/media.py 연동 후 구현.
        auto* downloadBtn =
            new QPushButton(SimpleIcons::download(Tokens::TEXT_PRIMARY, 11), QStringLiteral(" 다운로드 (사유 입력)"), box);
        downloadBtn->setProperty("variant", "secondary");
        downloadBtn->setProperty("size", "s");
        downloadBtn->setProperty("align", "left");
        layout->addWidget(downloadBtn);

        auto* shareBtn = new QPushButton(SimpleIcons::share(Tokens::TEXT_PRIMARY, 11),
                                          QStringLiteral(" 보호자 공유 링크 발급 (24시간 유효)"), box);
        shareBtn->setProperty("variant", "secondary");
        shareBtn->setProperty("size", "s");
        shareBtn->setProperty("align", "left");
        layout->addWidget(shareBtn);

        auto* deleteBtn = new QPushButton(SimpleIcons::trash(Tokens::EMERGENCY.main, 11),
                                           QStringLiteral(" 즉시 삭제 (관리자 승인 필요)"), box);
        deleteBtn->setProperty("variant", "secondary");
        deleteBtn->setProperty("tone", "danger");
        deleteBtn->setProperty("size", "s");
        deleteBtn->setProperty("align", "left");
        layout->addWidget(deleteBtn);

        return box;
    }

    void updateCountdownLabel() {
        const int mm = remainingSeconds_ / 60;
        const int ss = remainingSeconds_ % 60;
        countdownLabel_->setText(
            QStringLiteral("열람 가능 %1:%2 남음")
                .arg(mm, 2, 10, QLatin1Char('0'))
                .arg(ss, 2, 10, QLatin1Char('0')));
    }

    void unlock() {
        viewed_ = true;
        lockCard_->hide();
        videoWidget_->show();
        player_->play();
        playBtn_->setEnabled(false);
        reasonInput_->setEnabled(false);
        playToggleBtn_->setEnabled(true);
        seekBackBtn_->setEnabled(true);
        seekForwardBtn_->setEnabled(true);
        speedBtn_->setEnabled(true);

        remainingSeconds_ = 10 * 60;
        updateCountdownLabel();
        countdownTimer_->start(1000);

    }

    void expireSession() {
    // 재생 화면을 다시 잠그고, 사유를 새로 입력해야 재생 가능하게 만든다.
    viewed_ = false;
    player_->pause();
    videoWidget_->hide();
    lockCard_->show();
    playToggleBtn_->setEnabled(false);
    seekBackBtn_->setEnabled(false);
    seekForwardBtn_->setEnabled(false);
    speedBtn_->setEnabled(false);

    reasonInput_->clear();
    reasonInput_->setEnabled(true);
    playBtn_->setEnabled(false);

    lockMessage_->setText(
        QStringLiteral("열람 세션이 만료되었습니다. 계속 보려면 사유를 다시 입력하세요."));
    }

    void toggleFullScreen() {
        // 리눅스에서 showFullScreen()이 잘 안 먹혀서, videoArea_만 떼어 별도 창으로 띄운다.
        if (!isFullScreen_) {
            if (!fullscreenHost_) {
                fullscreenHost_ = new QWidget(this, Qt::Window | Qt::FramelessWindowHint);
                fullscreenHost_->setObjectName("videoFullscreenHost");
                auto* hostLayout = new QVBoxLayout(fullscreenHost_);
                hostLayout->setContentsMargins(0, 0, 0, 0);
                fullscreenHost_->installEventFilter(this);
            }
            leftColumnLayout_->removeWidget(videoArea_);
            static_cast<QVBoxLayout*>(fullscreenHost_->layout())->addWidget(videoArea_, 1);
            fullscreenHost_->setGeometry(parentWidget()->geometry());
            // 전체화면에선 640×480 캡을 풀어야 화면 크기에 맞춰 커진다.
            videoWidget_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            fullscreenHost_->show();
            fullscreenHost_->raise();
            fullscreenHost_->activateWindow();
            fullscreenHost_->setFocus();
            fullscreenBtn_->setIcon(SimpleIcons::collapse(Qt::white, 14));
            fullscreenBtn_->setToolTip(QStringLiteral("전체화면 종료 (Esc)"));
        } else {
            fullscreenHost_->layout()->removeWidget(videoArea_);
            fullscreenHost_->hide();
            videoWidget_->setMaximumSize(640, 480);
            leftColumnLayout_->insertWidget(0, videoArea_, /*stretch=*/1);
            fullscreenBtn_->setIcon(SimpleIcons::expand(Qt::white, 14));
            fullscreenBtn_->setToolTip(QStringLiteral("전체화면"));
        }
        isFullScreen_ = !isFullScreen_;
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        // fullscreenHost_에 포커스가 가 있는 동안의 Esc는 다이얼로그가 아니라 여기서 받는다.
        if (watched == fullscreenHost_ && event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                toggleFullScreen();
                return true;
            }
        }
        return QDialog::eventFilter(watched, event);
    }

    VideoClip clip_;
    bool hideEventLink_ = false;
    bool viewed_ = false;
    bool isFullScreen_ = false;
    int remainingSeconds_ = 0;

    QLineEdit* reasonInput_ = nullptr;
    QFrame* lockCard_ = nullptr;
    QLabel* playingLabel_ = nullptr;
    QLabel* lockMessage_ = nullptr;
    QLabel* countdownLabel_ = nullptr;
    QPushButton* playBtn_ = nullptr;
    QPushButton* fullscreenBtn_ = nullptr;
    QTimer* countdownTimer_ = nullptr;
    QVBoxLayout* leftColumnLayout_ = nullptr;
    QWidget* videoArea_ = nullptr;
    QWidget* fullscreenHost_ = nullptr;

    QVideoWidget* videoWidget_ = nullptr;
    QMediaPlayer* player_ = nullptr;
    QAudioOutput* audioOutput_ = nullptr;

    // 하단 재생 컨트롤. unlock() 전까지는 비활성.
    QPushButton* playToggleBtn_ = nullptr;
    QPushButton* seekBackBtn_ = nullptr;
    QPushButton* seekForwardBtn_ = nullptr;
    QPushButton* speedBtn_ = nullptr;
    int playbackRateIndex_ = kDefaultPlaybackRateIndex;  // kPlaybackRates 인덱스. speedBtn_ 클릭마다 순환
    QLabel* timeLabel_ = nullptr;
};

bool P4VideoPage::eventFilter(QObject* watched, QEvent* event) {
    // 읽기 전용 날짜 칸 클릭을 "직접"로 전환한다 (P2/P7과 동일 패턴).
    if (event->type() == QEvent::MouseButtonPress &&
        (watched == startDateEdit_ || watched == endDateEdit_) && startDateEdit_->isReadOnly()) {
        applyPeriodPreset(-1);
    }
    return QWidget::eventFilter(watched, event);
}

P4VideoPage::P4VideoPage(QWidget* parent) : QWidget(parent) {
    serverUrl_ = ServerConfig::resolveServerUrl();
    netManager_ = new QNetworkAccessManager(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    // ---------- 헤더: 타이틀 + 보관 정책 ----------
    auto* headerRow = new QHBoxLayout();

    auto* title = new QLabel(QStringLiteral("영상 관리"), this);
    title->setObjectName("pageTitle");
    headerRow->addWidget(title);
    headerRow->addStretch();

    auto* policyBtn = new QPushButton(QStringLiteral("보관 정책"), this);
    policyBtn->setProperty("variant", "secondary");
    policyBtn->setIcon(SimpleIcons::settings(Tokens::TEXT_PRIMARY, 12));
    headerRow->addWidget(policyBtn);

    connect(policyBtn, &QPushButton::clicked, this, [this] {
    QWidget* topLevel = window();
    auto* overlay = new QWidget(topLevel);
    overlay->setObjectName("modalOverlay");
    overlay->setGeometry(topLevel->rect());
    overlay->show();
    overlay->raise();

    RetentionPolicyDialog dialog(topLevel);
    dialog.exec();

    overlay->deleteLater();
});

    layout->addLayout(headerRow);

    // ---------- 필터 바 ----------
    // P7 "로그" 탭과 같은 구성 — 기간 세그먼트 + 날짜 범위 + 검색 하나. 로봇 ID·대상자
    // 이름·유형은 검색어 하나로 흡수한다(matchesFilters() 참고).
    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(6);

    const QDate today = QDate::currentDate();

    auto* periodSeg = new QWidget(this);
    auto* periodSegLayout = new QHBoxLayout(periodSeg);
    periodSegLayout->setContentsMargins(0, 0, 0, 0);
    periodSegLayout->setSpacing(0);

    auto* periodTodayBtn = new QPushButton(QStringLiteral("오늘"), periodSeg);
    auto* periodWeekBtn = new QPushButton(QStringLiteral("7일"), periodSeg);
    auto* periodMonthBtn = new QPushButton(QStringLiteral("30일"), periodSeg);
    periodCustomBtn_ = new QPushButton(QStringLiteral("직접"), periodSeg);
    periodTodayBtn->setProperty("pos", "first");
    periodCustomBtn_->setProperty("pos", "last");

    auto* periodGroup = new QButtonGroup(this);
    periodGroup->setExclusive(true);
    for (auto* btn : {periodTodayBtn, periodWeekBtn, periodMonthBtn, periodCustomBtn_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        periodSegLayout->addWidget(btn);
        periodGroup->addButton(btn);
    }
    filterRow->addWidget(periodSeg);

    connect(periodTodayBtn, &QPushButton::clicked, this, [this] { applyPeriodPreset(0); });
    connect(periodWeekBtn, &QPushButton::clicked, this, [this] { applyPeriodPreset(6); });
    connect(periodMonthBtn, &QPushButton::clicked, this, [this] { applyPeriodPreset(29); });
    connect(periodCustomBtn_, &QPushButton::clicked, this, [this] { applyPeriodPreset(-1); });

    startDateEdit_ = new CareGuardDateEdit(today, this);
    CareGuard::applyCareGuardDateEdit(startDateEdit_);
    filterRow->addWidget(startDateEdit_);

    auto* tildeLabel = new QLabel(QStringLiteral("~"), this);
    tildeLabel->setProperty("role", "muted");
    filterRow->addWidget(tildeLabel);

    endDateEdit_ = new CareGuardDateEdit(today, this);
    CareGuard::applyCareGuardDateEdit(endDateEdit_);
    filterRow->addWidget(endDateEdit_);

    connect(startDateEdit_, &QDateEdit::dateChanged, this, &P4VideoPage::rebuildCardGrid);
    connect(endDateEdit_, &QDateEdit::dateChanged, this, &P4VideoPage::rebuildCardGrid);
    // 읽기 전용 칸 클릭을 잡아 "직접"로 전환한다 (eventFilter() 참고).
    startDateEdit_->installEventFilter(this);
    endDateEdit_->installEventFilter(this);

    applyPeriodPreset(6);  // 초기 상태: 최근 7일, 읽기 전용 (기존 "기간 전체" 기본값 자리)
    periodWeekBtn->setChecked(true);

    searchEdit_ = new LiveSearchLineEdit(this);
    searchEdit_->setPlaceholderText(QStringLiteral("로봇 · 대상자 · 유형 검색"));
    connect(searchEdit_, &LiveSearchLineEdit::liveTextChanged, this, &P4VideoPage::rebuildCardGrid);
    filterRow->addWidget(searchEdit_, /*stretch=*/1);

    auto* gridToggle = new QPushButton(QStringLiteral("그리드"), this);
    gridToggle->setObjectName("segBtn");
    gridToggle->setProperty("pos", "first");
    gridToggle->setCheckable(true);
    gridToggle->setChecked(true);
    gridToggle->setIcon(SimpleIcons::dashboard(Tokens::ACCENT_HOVER, 11));

    auto* tableToggle = new QPushButton(QStringLiteral("테이블"), this);
    tableToggle->setObjectName("segBtn");
    tableToggle->setProperty("pos", "last");
    tableToggle->setCheckable(true);
    tableToggle->setIcon(SimpleIcons::list(Tokens::TEXT_SECONDARY, 11));

    auto* viewToggleGroup = new QButtonGroup(this);
    viewToggleGroup->setExclusive(true);
    viewToggleGroup->addButton(gridToggle);
    viewToggleGroup->addButton(tableToggle);

    filterRow->addWidget(gridToggle);
    filterRow->addWidget(tableToggle);
    layout->addLayout(filterRow);

    // ---------- 정책 안내 배너 ----------
    auto* banner = new QFrame(this);
    banner->setObjectName("banner");
    banner->setProperty("level", "info");
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(10, 7, 10, 7);
    bannerLayout->setSpacing(8);

    auto* bannerIcon = new QLabel(banner);
    bannerIcon->setPixmap(SimpleIcons::lock(Tokens::ACCENT_HOVER, 13).pixmap(13, 13));
    bannerLayout->addWidget(bannerIcon);

    auto* bannerText = new QLabel(
        QStringLiteral(
            "상시 녹화하지 않음 — 이벤트 전후 구간만 저장 · 이벤트 연동 30일 / 일반 7일 자동 삭제 · "
            "모든 열람은 사유와 함께 기록됨"),
        banner);
    bannerLayout->addWidget(bannerText, /*stretch=*/1);
    layout->addWidget(banner);

    // ---------- 카드 그리드 (그리드 뷰) ----------
    gridPage_ = new QWidget(this);
    cardGrid_ = new QGridLayout(gridPage_);
    cardGrid_->setSpacing(Tokens::Layout::CARD_GAP);
    cardGrid_->setContentsMargins(0, 0, 0, 0);

    // ---------- 테이블 뷰 ----------
    // TODO(team): 원래 델리게이트 기반 설계인데 지금은 QTableWidget으로 간소화했다.
    static const QStringList kTableHeaders = {QStringLiteral("유형"),   QStringLiteral("로봇ID"),
                                               QStringLiteral("대상자"), QStringLiteral("이벤트"),
                                               QStringLiteral("촬영일시"), QStringLiteral("길이"),
                                               QStringLiteral("열람횟수"), QStringLiteral("만료")};
    tableView_ = new RowBorderTableWidget(0, kTableHeaders.size(), this);
    tableView_->setHorizontalHeaderLabels(kTableHeaders);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    // 전역 QSS는 선택 배경을 transparent로 비워둬서 텍스트가 흰 배경에 흰 글씨로 안 보일
    // 수 있다 — 글자색만 로컬로 지정한다. margin은 컬럼·행 사이 여백, 선택 표시는 셀
    // 배경 대신 RowBorderTableWidget::paintEvent()가 행 전체 테두리로 그린다.
    // outline: none은 Qt 기본 점선 포커스 테두리 제거(setFocusPolicy(NoFocus)와 이중 방지).
    tableView_->setStyleSheet(
        QStringLiteral("QTableView::item { padding: 8px 14px; margin: 4px 8px; outline: none; }"
                        "QTableView::item:selected { background: transparent; border: none; "
                        "color: #16202B; }"));
    tableView_->setShowGrid(false);
    // 셀 편집이 없고 클릭 즉시 재생 모달을 여는 용도라 "현재 셀" 표시가 필요 없다.
    tableView_->setFocusPolicy(Qt::NoFocus);
    // 컬럼 전부 Stretch — 창 폭에 맞춰 8개 컬럼이 비례해서 늘어나 오른쪽 여백이 안 남는다.
    auto* tableHeader = tableView_->horizontalHeader();
    for (int col = 0; col < kTableHeaders.size(); ++col) {
        tableHeader->setSectionResizeMode(col, QHeaderView::Stretch);
    }
    // 그리드 카드 클릭과 동일하게, 선택 상태를 남기지 않고 클릭 즉시 재생 모달을 연다.
    connect(tableView_, &QTableWidget::cellClicked, this, [this](int row, int) {
        const int clipIndex = tableView_->item(row, 0)->data(Qt::UserRole).toInt();
        openVideoPlayer(clipIndex);
    });

    rebuildCardGrid();  // rebuildTableView()도 안에서 같이 호출해 두 뷰를 한 번에 채운다

    // 카드가 페이지 높이를 넘으면 스크롤되도록 gridPage_를 QScrollArea로 감싼다.
    auto* gridScrollArea = new QScrollArea(this);
    gridScrollArea->setWidgetResizable(true);
    gridScrollArea->setFrameShape(QFrame::NoFrame);
    gridScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gridScrollArea->setWidget(gridPage_);

    auto* viewStack = new QStackedWidget(this);
    viewStack->addWidget(gridScrollArea);  // index 0
    viewStack->addWidget(tableView_);      // index 1
    layout->addWidget(viewStack, /*stretch=*/1);

    connect(gridToggle, &QPushButton::toggled, this, [viewStack](bool checked) {
        if (checked) viewStack->setCurrentIndex(0);
    });
    connect(tableToggle, &QPushButton::toggled, this, [viewStack](bool checked) {
        if (checked) viewStack->setCurrentIndex(1);
    });

    fetchVideoClips();

    // video_event_created push를 구독해 목록을 자동으로 다시 받아온다.
    wsClient_ = new WsClient(this);
    connect(wsClient_, &WsClient::messageReceived, this, &P4VideoPage::handleWsMessage);
    wsClient_->connectToServer();
}

void P4VideoPage::handleWsMessage(const QJsonObject& message) {
    if (message.value(QStringLiteral("type")).toString() !=
        QStringLiteral("video_event_created")) {
        return;
    }
    // 부분 필드만 오므로 diff 대신 GET /video-events를 다시 호출해 통째로 채운다.
    fetchVideoClips();
}

void P4VideoPage::fetchVideoClips(std::function<void()> onLoaded) {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/video-events")));
    if (!authToken_.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + authToken_).toUtf8());
    }
    QNetworkReply* reply = netManager_->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, onLoaded] {
        handleVideoClipsReply(reply);
        reply->deleteLater();
        if (onLoaded) onLoaded();
    });
}

void P4VideoPage::openVideoPlayerByEventUid(const QString& eventUid) {
    auto tryOpen = [this, eventUid]() -> bool {
        for (int i = 0; i < clips_.size(); ++i) {
            if (clips_[i].eventId == eventUid) {
                openVideoPlayer(i, /*cameFromEventDetail=*/true);
                return true;
            }
        }
        return false;
    };
    if (tryOpen()) return;

    // P2에서 방금 막 생긴 이벤트에 연동된 영상일 수 있어(이 페이지가 아직
    // video_event_created push를 못 받았을 수 있음) 한 번 더 받아온 뒤 재시도한다.
    fetchVideoClips([this, eventUid, tryOpen] {
        if (!tryOpen()) {
            QMessageBox::information(this, QStringLiteral("CareGuard"),
                                      QStringLiteral("이 이벤트에 연동된 영상을 찾을 수 없습니다."));
        }
    });
}

void P4VideoPage::handleVideoClipsReply(QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        // 네트워크/서버 에러 — clips_를 비우고 makeEmptySlot() 안내만 보여준다.
        clips_.clear();
        rebuildCardGrid();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QVector<VideoClip> clips;
    for (const QJsonValue& value : doc.array()) {
        VideoClip clip = videoClipFromJson(value.toObject());
        clip.videoUrl = serverUrl_ + clip.videoUrl;  // video_url은 서버 기준 상대 경로로 옴
        clips.append(clip);
    }
    clips_ = clips;
    rebuildCardGrid();
}

void P4VideoPage::rebuildCardGrid() {
    // 생성자의 날짜 초기화가 cardGrid_ 생성 전에 이 함수를 부를 수 있어 가드를 둔다.
    if (cardGrid_ == nullptr) return;

    // deleteLater() 필수 — 즉시 delete하면 mousePressEvent 콜스택 중에 자기 자신을
    // 지워서 use-after-free가 난다.
    QLayoutItem* item;
    while ((item = cardGrid_->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    // takeAt()은 stretch 계수를 안 지우므로, 레이아웃을 통째로 새로 만들어 리셋한다.
    delete cardGrid_;
    cardGrid_ = new QGridLayout(gridPage_);
    cardGrid_->setSpacing(Tokens::Layout::CARD_GAP);
    cardGrid_->setContentsMargins(0, 0, 0, 0);

    // shown(그리드 위치)과 i(clips_ 인덱스)를 분리해 필터링 중에도 인덱스가 안 어긋나게 한다.
    int shown = 0;
    for (int i = 0; i < clips_.size(); ++i) {
        if (!matchesFilters(clips_[i])) continue;
        auto* card = makeVideoCard(clips_[i], gridPage_);
        connect(card, &ClickableCard::clicked, this, [this, i] { openVideoPlayer(i); });
        cardGrid_->addWidget(card, shown / columns_, shown % columns_);
        ++shown;
    }
    cardGrid_->addWidget(makeEmptySlot(gridPage_), shown / columns_, shown % columns_);
    for (int c = 0; c < columns_; ++c) {
        cardGrid_->setColumnStretch(c, 1);
    }
    // 카드 행은 stretch 없이 원래 크기 유지, 마지막 빈 행에 stretch=1로 남는 공간을 흡수시킨다.
    const int rowCount = (shown + 1 + columns_ - 1) / columns_;
    cardGrid_->setRowStretch(rowCount, 1);

    rebuildTableView();
}

// rebuildCardGrid()와 같은 필터 결과를 표로 그린다. 색 강조는 셀 글자색 정도로만 표현.
void P4VideoPage::rebuildTableView() {
    tableView_->setRowCount(0);

    for (int i = 0; i < clips_.size(); ++i) {
        const VideoClip& clip = clips_[i];
        if (!matchesFilters(clip)) continue;

        const int row = tableView_->rowCount();
        tableView_->insertRow(row);

        // 모든 컬럼 값을 가운데 정렬로 통일한다 (기본은 왼쪽 정렬).
        auto setCentered = [this, row](int col, QTableWidgetItem* item) {
            item->setTextAlignment(Qt::AlignCenter);
            tableView_->setItem(row, col, item);
        };

        auto* typeItem = new QTableWidgetItem(clip.tagText);
        switch (clip.tagStyle) {
            case VideoTagStyle::Emergency:
                typeItem->setForeground(QBrush(Tokens::EMERGENCY.main));
                break;
            case VideoTagStyle::Attention:
                typeItem->setForeground(QBrush(Tokens::ATTENTION.main));
                break;
            case VideoTagStyle::Neutral:
                break;  // 기본 텍스트 색 그대로
        }
        // cellClicked가 clips_ 인덱스를 알아야 해서 0번 칸에 심어둔다.
        typeItem->setData(Qt::UserRole, i);
        setCentered(0, typeItem);

        setCentered(1, new QTableWidgetItem(clip.robotId));
        setCentered(2, new QTableWidgetItem(clip.recipientName));
        setCentered(3, new QTableWidgetItem(clip.eventLabel));
        setCentered(4, new QTableWidgetItem(clip.capturedAt));
        setCentered(5, new QTableWidgetItem(clip.durationLabel));
        setCentered(6, new QTableWidgetItem(clip.viewCount > 0
                                                 ? QStringLiteral("열람 %1회").arg(clip.viewCount)
                                                 : QStringLiteral("열람 없음")));

        auto* expireItem = new QTableWidgetItem(QStringLiteral("D-%1").arg(clip.expireDays));
        if (clip.expireDays <= 7) {
            expireItem->setForeground(QBrush(QColor("#C27500")));  // QLabel[role="warning"]과 동일 색
        }
        setCentered(7, expireItem);
    }
}

int P4VideoPage::computeColumnCount() const {
    // CSS grid의 auto-fill과 같은 공식: (가용폭 + gap) / (카드 최소폭 + gap)를 내림.
    const int available = width() - 2 * Tokens::Layout::PAGE_PADDING;
    const int gap = Tokens::Layout::CARD_GAP;
    const int columns = (available + gap) / (kCardMinWidth + gap);
    return qMax(kMinColumns, columns);
}

void P4VideoPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int newColumns = computeColumnCount();
    if (newColumns != columns_) {
        columns_ = newColumns;
        rebuildCardGrid();
    }
}

void P4VideoPage::applyPeriodPreset(int spanDays) {
    // spanDays >= 0이면 "오늘 - spanDays일 ~ 오늘"로 채우고 읽기 전용으로 잠근다.
    if (spanDays >= 0) {
        const QDate today = QDate::currentDate();
        startDateEdit_->setDate(today.addDays(-spanDays));
        endDateEdit_->setDate(today);
        setDateEditsReadOnly(true);
    } else {
        setDateEditsReadOnly(false);
        if (!periodCustomBtn_->isChecked()) {
            periodCustomBtn_->setChecked(true);  // 날짜 칸을 직접 클릭해 진입한 경우
        }
    }
}

void P4VideoPage::setDateEditsReadOnly(bool readOnly) {
    for (auto* edit : {startDateEdit_, endDateEdit_}) {
        edit->setReadOnly(readOnly);
        edit->setProperty("state", readOnly ? QStringLiteral("off") : QVariant());
        repolish(edit);
    }
}

bool P4VideoPage::matchesFilters(const VideoClip& clip) const {
    // capturedAtRaw가 없는 클립은 날짜 범위를 알 수 없으니 제외한다.
    if (!clip.capturedAtRaw.isValid()) return false;
    const QDate capturedDate = clip.capturedAtRaw.date();
    if (capturedDate < startDateEdit_->date() || capturedDate > endDateEdit_->date()) {
        return false;
    }

    // 로봇 ID·대상자 이름·유형을 검색어 하나로 부분 일치(대소문자 무시)로 건다.
    const QString keyword = searchEdit_->liveText().trimmed();
    if (!keyword.isEmpty() && !clip.robotId.contains(keyword, Qt::CaseInsensitive) &&
        !clip.recipientName.contains(keyword, Qt::CaseInsensitive) &&
        !clip.tagText.contains(keyword, Qt::CaseInsensitive)) {
        return false;
    }
    return true;
}

void P4VideoPage::openVideoPlayer(int clipIndex, bool cameFromEventDetail) {
    // §4-4: 배경 흐림은 부모 위 반투명 오버레이 QWidget을 띄우고 그 위에 다이얼로그를 얹는다.
    QWidget* topLevel = window();
    auto* overlay = new QWidget(topLevel);
    overlay->setObjectName("modalOverlay");
    overlay->setGeometry(topLevel->rect());
    overlay->show();
    overlay->raise();

    VideoPlayerDialog dialog(clips_[clipIndex], topLevel, cameFromEventDetail);
    connect(&dialog, &VideoPlayerDialog::eventDetailRequested, this,
            &P4VideoPage::eventDetailRequested);
    dialog.exec();

    overlay->deleteLater();

    if (dialog.wasViewed()) {
        recordVideoView(clipIndex, dialog.enteredReason());
    }
}

// POST /video-events/{id}/view로 열람을 기록하고, 응답의 view_count/view_history를
// clips_에 그대로 반영한다 — 로컬에서 임의로 +1 하지 않는다.
void P4VideoPage::recordVideoView(int clipIndex, const QString& reason) {
    if (clipIndex < 0 || clipIndex >= clips_.size()) return;
    const int cameraRecordingId = clips_[clipIndex].cameraRecordingId;
    if (cameraRecordingId <= 0) return;  // mock 데이터 등 서버에서 안 온 클립은 기록 대상이 아님

    QNetworkRequest request(
        QUrl(QStringLiteral("%1/video-events/%2/view").arg(serverUrl_).arg(cameraRecordingId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!authToken_.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + authToken_).toUtf8());
    }

    const QJsonObject body{{"login_id", currentUserName_}, {"reason", reason}};
    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());

    connect(reply, &QNetworkReply::finished, this, [this, reply, clipIndex] {
        reply->deleteLater();
        // 응답이 오는 사이 목록이 새로 불려와 clips_가 바뀌었을 수 있어 인덱스를 다시 검사한다.
        if (reply->error() != QNetworkReply::NoError || clipIndex < 0 || clipIndex >= clips_.size()) {
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        VideoClip& clip = clips_[clipIndex];
        clip.viewCount = obj.value("view_count").toInt();
        clip.viewHistory = parseViewHistoryArray(obj.value("view_history").toArray());
        rebuildCardGrid();
    });
}

void P4VideoPage::setUserName(const QString& name) {
    currentUserName_ = name.trimmed().isEmpty() ? QStringLiteral("담당자") : name.trimmed();
}

#include "p4_video_page.moc"
