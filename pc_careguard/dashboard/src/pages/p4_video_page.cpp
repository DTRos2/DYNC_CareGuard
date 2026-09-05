#include "p4_video_page.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
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
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoWidget>

#include "theme/tokens.h"
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

// server/app/media.py GET /video-events 응답을 VideoClip으로 매핑하는 헬퍼들.

// Python datetime.isoformat()은 마이크로초(6자리)까지 내려주는데 Qt::ISODateWithMs는
// 밀리초(3자리)까지만 인식해서, 소수점 이하를 3자리로 잘라낸 뒤 파싱한다.
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

// user_action_log 기반 열람 이력 배열을 "HH:MM:SS · 열람자 · 사유: ..." 문자열 리스트로 만든다.
// GET /video-events와 POST /video-events/{id}/view 응답이 view_history를 같은 모양
// (occurred_at/login_id/reason)으로 내려주므로 두 곳(videoClipFromJson·recordVideoView)에서 같이 쓴다.
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

    // TODO(team): 보관 정책이 아직 없어 files.retention_until이 항상 null로 내려온다.
    // 그동안은 만료 배지가 거짓으로 D-0 경고를 띄우지 않도록 넉넉한 기본값을 쓴다.
    const QDateTime retentionUntil = parseIsoDateTime(obj.value("retention_until").toString());
    clip.expireDays = retentionUntil.isValid()
                           ? qMax(0, static_cast<int>(QDateTime::currentDateTime().daysTo(retentionUntil)))
                           : 30;

    clip.location = obj.value("location").toString();
    // video_url은 서버 기준 상대 경로("/media/files/{id}")라, serverUrl_은 이 함수가
    // 몰라서 여기선 그대로 담아두고 handleVideoClipsReply()에서 앞에 이어붙인다.
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

// §5-1 상태 배지. "순찰"은 §7-1 상태 판정(EMERGENCY/ATTENTION/OFFLINE/NORMAL)과
// 무관한 중립 태그라 badge level 표준 목록엔 없다 — OFFLINE 토큰과 배경/텍스트 색이 동일해
// (#F1F4F6 / #5A6774, §1-4) 재사용한다.
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

}  // namespace

// ============================================================
// 재생 모달 (p4_video_playback_modal_wireframe 그대로).
// 실제 스트림/파일 연동 전까지는 영상 자리를 플레이스홀더로만 표시한다.
// Q_OBJECT 클래스를 헤더 없이 .cpp에 직접 둔 뒤 파일 끝의 #include "p4_video_page.moc"로
// AUTOMOC이 처리하게 하는 표준 Qt/CMake 패턴을 쓴다 (다른 페이지에서 재사용할 일이 생기면
// dialogs/video_player_dialog.{h,cpp}로 옮긴다).
// ============================================================
class VideoPlayerDialog : public QDialog {
    Q_OBJECT
public:
    VideoPlayerDialog(const VideoClip& clip, QWidget* parent = nullptr)
        : QDialog(parent), clip_(clip) {
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

        // 영상 영역이 stretch=1로 남는 세로 공간을 다 가져간다 — 심박 타임라인 박스를 뺀 만큼
        // 더 커진다. videoArea_는 멤버로 들고 있다가 전체화면 진입 시 이 레이아웃에서 빼서
        // fullscreenHost_로 옮긴다 (toggleFullScreen() 참고).
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

        // 실제 재생 위젯. GET /media/files/{id}를 QMediaPlayer가 HTTP로 직접 받아 재생한다
        // (서버가 Range 요청을 지원해서 탐색도 됨). 잠금 해제 전엔 숨겨두고, unlock()에서 보여준다.
        videoWidget_ = new QVideoWidget(area);
        // 로봇 녹화 영상은 항상 640×480(4:3)으로 고정. QVideoWidget::sizeHint()가 영상 원본
        // 해상도를 그대로 돌려주는데, maximumSize를 그 해상도로 캡 걸어두면 레이아웃이 그 이상
        // 커지지 않으면서(=화면 밖으로 안 나감) 좁은 모달에서는 비율 유지하며 줄어든다.
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
        // setMaximumWidth만 쓰면 QLabel::wordWrap의 heightForWidth가 첫 레이아웃 패스에서
        // 아직 좁혀지지 않은 폭 기준으로 계산돼 줄바꿈 높이가 실제보다 작게 잡히고, 그 결과
        // 두 안내 문구 QLabel이 서로 겹쳐 그려진다. setFixedWidth로 폭을 처음부터 고정해야
        // 줄바꿈 높이가 그 폭 기준으로 정확히 계산된다.
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

        // buildVideoArea()가 먼저 호출돼 player_가 이미 만들어진 뒤라(buildLeftColumn() 순서
        // 참고) 여기서 바로 연결해도 된다. unlock() 전엔 재생 자체가 안 되니 비활성으로 시작.
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

        // 클릭할 때마다 kPlaybackRates를 한 칸씩 돌며 QMediaPlayer::setPlaybackRate()에 반영한다.
        // unlock() 전엔 재생 자체가 안 되니 다른 재생 컨트롤과 마찬가지로 비활성으로 시작.
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
        layout->addWidget(summary);  // 기본 QWidget 색(TEXT_PRIMARY) 그대로 — 와이어프레임 제목은 검정

        const QString status = clip_.tagStyle == VideoTagStyle::Emergency ? QStringLiteral("대응 중")
                                                                            : QStringLiteral("완료");
        auto* subtitle = new QLabel(
            QStringLiteral("%1 · %2 · 담당 도희정").arg(clip_.location, status), card);
        subtitle->setProperty("role", "secondary");  // TEXT_SECONDARY — 와이어프레임 부제는 회색
        layout->addWidget(subtitle);

        // TODO(team): P2 이벤트 상세 모달로 이동하는 라우팅은 아직 없음 (페이지 간 네비게이션 미구현).
        auto* link = new QPushButton(QStringLiteral("이벤트 상세로 이동 →"), card);
        link->setObjectName("linkBtn");
        layout->addWidget(link, 0, Qt::AlignLeft);

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
    // 세션 만료: 재생 중이던 화면을 다시 잠그고, 사유를 새로 입력해야만 재생 가능하게 만든다.
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
        // QDialog는 showFullScreen()을 걸어도 리눅스 창관리자가 무시하는 경우가 많아,
        // 영상 영역(videoArea_)만 떼어내 별도 창(Qt::Window)에 옮긴다. 모니터 전체가 아니라
        // CareGuard 창 영역만 채우도록 parentWidget()의 geometry()에 맞춘다.
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
            // 평소엔 videoWidget_을 영상 원본 해상도(640×480)로 캡 걸어두지만, 전체화면에선
            // 그 캡을 풀어야 화면 크기에 맞춰 비율 유지하며 커진다(QVideoWidget이 내부적으로
            // 종횡비를 유지해 레터박스 처리하므로 찌그러지진 않는다).
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

    // 하단 재생 컨트롤 (buildPlaybackRow()). unlock() 전까지는 비활성 — 사유 입력 전엔
    // 재생 자체가 안 되니 되감기/앞으로가기·배속 조절도 의미가 없다.
    QPushButton* playToggleBtn_ = nullptr;
    QPushButton* seekBackBtn_ = nullptr;
    QPushButton* seekForwardBtn_ = nullptr;
    QPushButton* speedBtn_ = nullptr;
    int playbackRateIndex_ = kDefaultPlaybackRateIndex;  // kPlaybackRates 인덱스. speedBtn_ 클릭마다 순환
    QLabel* timeLabel_ = nullptr;
};

P4VideoPage::P4VideoPage(QWidget* parent) : QWidget(parent) {
    serverUrl_ = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_URL"), QStringLiteral("http://127.0.0.1:8000"));
    netManager_ = new QNetworkAccessManager(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    // ---------- 헤더: 타이틀 + 저장 용량 게이지 + 보관 정책 ----------
    auto* headerRow = new QHBoxLayout();

    auto* title = new QLabel(QStringLiteral("영상 관리"), this);
    title->setObjectName("pageTitle");
    headerRow->addWidget(title);
    headerRow->addStretch();

    auto* storageLabel = new QLabel(QStringLiteral("저장 용량"), this);
    storageLabel->setProperty("role", "secondary");
    headerRow->addWidget(storageLabel);

    // TODO(team): 실사용량은 server/media.py 연동 후 대체 (지금은 wireframe 값 그대로 mock)
    constexpr int usedGb = 31;
    constexpr int totalGb = 50;
    auto* storageGauge = new QProgressBar(this);
    storageGauge->setRange(0, totalGb);
    storageGauge->setValue(usedGb);
    storageGauge->setTextVisible(false);
    storageGauge->setFixedWidth(90);
    headerRow->addWidget(storageGauge);

    auto* storageText = new QLabel(QStringLiteral("%1/%2 GB").arg(usedGb).arg(totalGb), this);
    storageText->setProperty("role", "mono");
    headerRow->addWidget(storageText);

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
    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(6);

    // 콤보박스 선택이 바뀌면(currentIndexChanged) rebuildCardGrid()가 matchesFilters()로
    // clips_를 다시 걸러서 그린다. rebuildCardGrid는 인자가 없어 시그널의 int index는 버려진다.
    auto addFilter = [this, filterRow](const QString& allLabel, const QStringList& items) {
        auto* combo = new QComboBox(this);
        combo->addItem(allLabel);
        combo->addItems(items);
        // 항목 중 가장 긴 텍스트에 맞춰 폭을 자동으로 늘린다 (일반 콤보박스에선 잘 동작함 —
        // editable로 바꿨을 때만 이 정책이 안 먹혀서 그때는 별도 계산이 필요했었음).
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        filterRow->addWidget(combo);
        connect(combo, &QComboBox::currentIndexChanged, this, &P4VideoPage::rebuildCardGrid);
        return combo;
    };
    robotFilter_ = addFilter(QStringLiteral("로봇 전체"),
                              {QStringLiteral("R-01"), QStringLiteral("R-02"), QStringLiteral("R-03")});
    recipientFilter_ =
        addFilter(QStringLiteral("대상자 전체"),
                  {QStringLiteral("김순자"), QStringLiteral("박영호"), QStringLiteral("이말순")});
    periodFilter_ = addFilter(
        QStringLiteral("기간 전체"),
        {QStringLiteral("오늘"), QStringLiteral("최근 7일"), QStringLiteral("최근 30일")});
    typeFilter_ = addFilter(
        QStringLiteral("유형 전체"),
        {QStringLiteral("긴급"), QStringLiteral("주의"), QStringLiteral("순찰")});
    filterRow->addStretch();

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
    // TODO(team): §4-3 원안은 QTableView + 커스텀 델리게이트(행 상태 배경·좌측 바 등)인데,
    // 지금은 QTableWidget으로 우선 동작하게만 만들어둔다. 필요해지면 모델/델리게이트로 교체.
    static const QStringList kTableHeaders = {QStringLiteral("유형"),   QStringLiteral("로봇ID"),
                                               QStringLiteral("대상자"), QStringLiteral("이벤트"),
                                               QStringLiteral("촬영일시"), QStringLiteral("길이"),
                                               QStringLiteral("열람횟수"), QStringLiteral("만료")};
    tableView_ = new QTableWidget(0, kTableHeaders.size(), this);
    tableView_->setHorizontalHeaderLabels(kTableHeaders);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    // 전역 QSS는 QTableView의 selection-background-color를 transparent로 비워둔다 —
    // §4-3 원안처럼 커스텀 델리게이트가 선택 테두리를 따로 그려준다는 전제라, 방법 A(델리게이트
    // 없음)에선 배경 없이 텍스트만 하이라이트색(흰색)으로 바뀌어 안 보이는 문제가 생긴다.
    // 이 테이블에만 로컬 스타일시트로 실제 배경/글자색을 입혀서 선택 표시가 보이게 한다.
    tableView_->setStyleSheet(
        QStringLiteral("QTableView::item:selected { background: rgba(14, 147, 132, 0.14); "
                        "color: #16202B; }"));
    tableView_->setShowGrid(false);
    tableView_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);  // "이벤트" 열만 늘어남
    // 그리드 카드 클릭과 동일하게, 선택 상태를 남기지 않고 클릭 즉시 재생 모달을 연다.
    connect(tableView_, &QTableWidget::cellClicked, this, [this](int row, int) {
        const int clipIndex = tableView_->item(row, 0)->data(Qt::UserRole).toInt();
        openVideoPlayer(clipIndex);
    });

    rebuildCardGrid();  // rebuildTableView()도 안에서 같이 호출해 두 뷰를 한 번에 채운다

    auto* viewStack = new QStackedWidget(this);
    viewStack->addWidget(gridPage_);   // index 0
    viewStack->addWidget(tableView_);  // index 1
    layout->addWidget(viewStack, /*stretch=*/1);

    connect(gridToggle, &QPushButton::toggled, this, [viewStack](bool checked) {
        if (checked) viewStack->setCurrentIndex(0);
    });
    connect(tableToggle, &QPushButton::toggled, this, [viewStack](bool checked) {
        if (checked) viewStack->setCurrentIndex(1);
    });

    fetchVideoClips();
}

void P4VideoPage::fetchVideoClips() {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/video-events")));
    QNetworkReply* reply = netManager_->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleVideoClipsReply(reply);
        reply->deleteLater();
    });
}

void P4VideoPage::handleVideoClipsReply(QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        // 네트워크 에러(statusCode == 0)나 서버 에러 — clips_는 비운 채로 두고, 카드 그리드는
        // makeEmptySlot()의 "이벤트 발생 시 자동으로 추가됩니다" 안내만 보여준다.
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
    // deleteLater() 필수: 열람 후 재생 모달을 닫으면 openVideoPlayer()가 그 클릭을 처리 중이던
    // ClickableCard::mousePressEvent 콜스택 안에서 이 함수를 호출한다. 여기서 delete를 바로
    // 쓰면 지금 이벤트를 처리 중인 카드 자신을 지우게 되어, mousePressEvent가 리턴하면서
    // 이미 죽은 this로 QFrame::mousePressEvent를 호출해 use-after-free가 난다.
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

    // 필터에 걸린 것만 그린다. 그리드 위치(shown)와 clips_ 원본 인덱스(i)를 분리해야
    // 필터링 중에도 openVideoPlayer(i)가 항상 올바른 clip을 가리킨다.
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
    // 카드 행(row)엔 stretch를 안 걸어야 원래 크기를 유지한다. 대신 마지막에 stretch=1짜리
    // 빈 행을 하나 더 둬서, 창이 카드 총합보다 클 때 남는 세로 공간을 그 행이 흡수하게 한다.
    const int rowCount = (shown + 1 + columns_ - 1) / columns_;
    cardGrid_->setRowStretch(rowCount, 1);

    rebuildTableView();
}

// rebuildCardGrid()와 같은 clips_/matchesFilters() 결과를 표로 그린다 (컬럼: 유형/로봇ID/대상자/
// 이벤트/촬영일시/길이/열람횟수/만료). §4-3 원안(모델+델리게이트로 행 배경·좌측 바까지)의
// 간소화 버전 — 색 강조는 셀 글자색 정도로만 표현한다.
void P4VideoPage::rebuildTableView() {
    tableView_->setRowCount(0);

    for (int i = 0; i < clips_.size(); ++i) {
        const VideoClip& clip = clips_[i];
        if (!matchesFilters(clip)) continue;

        const int row = tableView_->rowCount();
        tableView_->insertRow(row);

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
        // cellClicked 핸들러가 클릭된 행의 실제 clips_ 인덱스를 알아야 하므로 0번 칸에 심어둔다
        // (그리드 카드의 [this, i] 캡처와 같은 역할).
        typeItem->setData(Qt::UserRole, i);
        tableView_->setItem(row, 0, typeItem);

        tableView_->setItem(row, 1, new QTableWidgetItem(clip.robotId));
        tableView_->setItem(row, 2, new QTableWidgetItem(clip.recipientName));
        tableView_->setItem(row, 3, new QTableWidgetItem(clip.eventLabel));
        tableView_->setItem(row, 4, new QTableWidgetItem(clip.capturedAt));
        tableView_->setItem(row, 5, new QTableWidgetItem(clip.durationLabel));
        tableView_->setItem(row, 6,
                             new QTableWidgetItem(clip.viewCount > 0
                                                       ? QStringLiteral("열람 %1회").arg(clip.viewCount)
                                                       : QStringLiteral("열람 없음")));

        auto* expireItem = new QTableWidgetItem(QStringLiteral("D-%1").arg(clip.expireDays));
        if (clip.expireDays <= 7) {
            expireItem->setForeground(QBrush(QColor("#C27500")));  // QLabel[role="warning"]과 동일 색
        }
        tableView_->setItem(row, 7, expireItem);
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

bool P4VideoPage::matchesFilters(const VideoClip& clip) const {
    if (robotFilter_->currentIndex() > 0 && clip.robotId != robotFilter_->currentText()) {
        return false;
    }
    if (recipientFilter_->currentIndex() > 0 && clip.recipientName != recipientFilter_->currentText()) {
        return false;
    }
    if (typeFilter_->currentIndex() > 0 && clip.tagText != typeFilter_->currentText()) {
        return false;
    }
    if (periodFilter_->currentIndex() > 0) {
        // capturedAtRaw가 없는 클립(예: mock 데이터, 파싱 실패)은 기간을 알 수 없으니 필터가
        // 걸려있을 땐 안전하게 제외한다 — "전체"(index 0) 선택 시엔 이 블록 자체가 안 돈다.
        if (!clip.capturedAtRaw.isValid()) return false;

        // 자정 기준 날짜 차이로 계산 — "오늘"이 자정 넘기자마자 어제로 밀리지 않게 시간(HH:mm)은 무시.
        const qint64 daysAgo = clip.capturedAtRaw.date().daysTo(QDate::currentDate());
        switch (periodFilter_->currentIndex()) {
            case 1:  // 오늘
                if (daysAgo != 0) return false;
                break;
            case 2:  // 최근 7일
                if (daysAgo < 0 || daysAgo > 7) return false;
                break;
            case 3:  // 최근 30일
                if (daysAgo < 0 || daysAgo > 30) return false;
                break;
        }
    }
    return true;
}

void P4VideoPage::openVideoPlayer(int clipIndex) {
    // §4-4: 배경 흐림은 부모 위 반투명 오버레이 QWidget을 띄우고 그 위에 다이얼로그를 얹는다.
    QWidget* topLevel = window();
    auto* overlay = new QWidget(topLevel);
    overlay->setObjectName("modalOverlay");
    overlay->setGeometry(topLevel->rect());
    overlay->show();
    overlay->raise();

    VideoPlayerDialog dialog(clips_[clipIndex], topLevel);
    dialog.exec();

    overlay->deleteLater();

    if (dialog.wasViewed()) {
        recordVideoView(clipIndex, dialog.enteredReason());
    }
}

// 서버 POST /video-events/{id}/view를 호출해 열람 사실을 감사 로그에 남긴다. 응답으로
// 다시 계산된 view_count/view_history를 받아 그대로 clips_에 반영한다 — 로컬에서 임의로
// +1 하면 새로고침·다른 PC에서 보는 값과 어긋나므로, 항상 서버가 다시 센 값을 신뢰한다.
void P4VideoPage::recordVideoView(int clipIndex, const QString& reason) {
    if (clipIndex < 0 || clipIndex >= clips_.size()) return;
    const int cameraRecordingId = clips_[clipIndex].cameraRecordingId;
    if (cameraRecordingId <= 0) return;  // mock 데이터 등 서버에서 안 온 클립은 기록 대상이 아님

    QNetworkRequest request(
        QUrl(QStringLiteral("%1/video-events/%2/view").arg(serverUrl_).arg(cameraRecordingId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

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
