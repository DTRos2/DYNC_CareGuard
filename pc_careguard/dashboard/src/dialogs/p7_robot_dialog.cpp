#include "p7_robot_dialog.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/checkable_list_widget.h"
#include "widgets/korean_search.h"
#include "widgets/live_search_line_edit.h"
#include "widgets/simple_icons.h"

namespace {
constexpr int kRosDomainMin = 0;
constexpr int kRosDomainMax = 232;

// Qt::FramelessWindowHint 다이얼로그 안에서는 QComboBox가 팝업 위치를 잘못 계산해서
// 콤보 박스 바로 아래가 아니라 다이얼로그 중간쯤에 뜬다. 팝업을 띄운 직후 콤보 박스
// 바로 아래로 직접 옮겨서 고정한다.
class DropDownFixComboBox : public QComboBox {
public:
    using QComboBox::QComboBox;
    void showPopup() override {
        QComboBox::showPopup();
        view()->window()->move(mapToGlobal(QPoint(0, height())));
    }
};
}  // namespace

RobotDialog::RobotDialog(Mode mode, const RobotData& data, const QVector<RecipientOption>& recipients,
                          QWidget* parent)
    : QDialog(parent), mode_(mode), originalCode_(data.code) {
    setModal(true);
    // OS 창관리자 타이틀바를 없애 커스텀 헤더(제목·X)만 남긴다 — 안 그러면 X가 두 개로 보임
    // (AccountDialog와 동일 이유 — p7_account_dialog.cpp 참고).
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setFixedWidth(Tokens::Layout::MODAL_W_S);
    const bool isEdit = (mode_ == Mode::Edit);
    setWindowTitle(isEdit ? QStringLiteral("로봇 수정 · %1").arg(data.code)
                           : QStringLiteral("로봇 등록"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildHeader(isEdit ? QStringLiteral("로봇 수정 · %1").arg(data.code)
                                        : QStringLiteral("로봇 등록")));

    auto* body = new QVBoxLayout();
    body->setContentsMargins(16, 14, 16, 14);
    body->setSpacing(12);

    // 노드 경고 배너 (Edit 모드 + warnNode가 있을 때만)
    if (isEdit && !data.warnNode.isEmpty()) {
        auto* banner = new QFrame(this);
        banner->setObjectName("banner");
        banner->setProperty("level", Tokens::Prop::ATTENTION);
        auto* bl = new QHBoxLayout(banner);
        bl->setContentsMargins(9, 7, 9, 7);
        bl->setSpacing(8);
        auto* icon = new QLabel(banner);
        icon->setPixmap(SimpleIcons::alertTriangle(Tokens::ATTENTION.main, 14).pixmap(14, 14));
        bl->addWidget(icon);
        auto* text = new QLabel(
            QStringLiteral("노드 <b style=\"font-family:'JetBrains Mono'\">%1</b> %2 · "
                            "설정 변경 후 재시작이 필요할 수 있습니다")
                .arg(data.warnNode, data.warnDetail),
            banner);
        text->setTextFormat(Qt::RichText);
        text->setWordWrap(true);
        bl->addWidget(text, 1);
        body->addWidget(banner);
    }

    // 로봇 코드 · 표시 이름 · 모델 · ROS_DOMAIN_ID — 한 줄에 하나씩 세로로 나열.
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* label = new QLabel(QStringLiteral("로봇 코드*"), this);
        label->setProperty("role", "label");
        col->addWidget(label);
        codeEdit_ = new QLineEdit(data.code, this);
        codeEdit_->setFont(QFont(Tokens::Font::MONO));
        if (isEdit) {
            codeEdit_->setReadOnly(true);
            codeEdit_->setText(data.code + QStringLiteral(" 🔒"));
            codeEdit_->setStyleSheet(
                QStringLiteral("background:%1; color:%2;").arg(Tokens::BG_SUNKEN.name(), Tokens::TEXT_MUTED.name()));
        }
        col->addWidget(codeEdit_);
        auto* hint = new QLabel(isEdit ? QStringLiteral("변경하려면 등록 해제 후 재등록")
                                        : QStringLiteral("연결 식별자 · 등록 후 변경 불가"),
                                 this);
        hint->setProperty("role", "muted");
        hint->setStyleSheet(QStringLiteral("font-size:11px;"));
        col->addWidget(hint);
        body->addLayout(col);
    }
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* label = new QLabel(QStringLiteral("표시 이름*"), this);
        label->setProperty("role", "label");
        col->addWidget(label);
        nameEdit_ = new QLineEdit(data.name, this);
        if (!isEdit) nameEdit_->setPlaceholderText(QStringLiteral("예) 5호기"));
        col->addWidget(nameEdit_);
        body->addLayout(col);
    }
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* label = new QLabel(QStringLiteral("모델"), this);
        label->setProperty("role", "label");
        col->addWidget(label);
        modelCombo_ = new DropDownFixComboBox(this);
        modelCombo_->addItems({QStringLiteral("limo"), QStringLiteral("turtlebot")});
        modelCombo_->setCurrentText(data.model);
        col->addWidget(modelCombo_);
        body->addLayout(col);
    }
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* label = new QLabel(QStringLiteral("ROS_DOMAIN_ID"), this);
        label->setProperty("role", "label");
        col->addWidget(label);
        rosDomainEdit_ = new QLineEdit(QString::number(data.rosDomainId), this);
        rosDomainEdit_->setFont(QFont(Tokens::Font::MONO));
        col->addWidget(rosDomainEdit_);
        if (!isEdit) {
            auto* hint = new QLabel(QStringLiteral("0–232 · 로봇마다 다르게"), this);
            hint->setProperty("role", "muted");
            hint->setStyleSheet(QStringLiteral("font-size:11px;"));
            col->addWidget(hint);
        }
        body->addLayout(col);
    }

    // 담당 대상자 — AccountDialog와 동일하게 검색으로 좁혀서 목록에서 고른다(단, 여기는
    // AccountDialog와 동일한 체크박스 목록이되, 로봇 한 대에 대상자 한 명만 배정되므로
    // 체크박스 하나를 켜면 나머지는 자동으로 꺼지는 단일 선택으로 동작한다.
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* label = new QLabel(QStringLiteral("담당 대상자"), this);
        label->setProperty("role", "label");
        col->addWidget(label);

        recipientSearchEdit_ = new LiveSearchLineEdit(this);
        recipientSearchEdit_->setPlaceholderText(QStringLiteral("이름 또는 코드로 검색"));
        col->addWidget(recipientSearchEdit_);

        // "미배정"은 목록에 넣지 않는다 — 체크된 항목이 하나도 없으면 그게 곧 미배정이다.
        recipientListWidget_ = new CheckableListWidget(this);
        recipientListWidget_->setFixedHeight(120);
        recipientListWidget_->setSelectionMode(QAbstractItemView::NoSelection);
        recipientListWidget_->setFocusPolicy(Qt::NoFocus);

        for (const RecipientOption& option : recipients) {
            auto* item = new QListWidgetItem(option.label, recipientListWidget_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(option.label == data.recipientLabel ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, option.id);
        }
        // 체크박스 하나를 켜면 나머지를 자동으로 끈다 — 라디오처럼 항상 최대 하나만 체크된다.
        connect(recipientListWidget_, &QListWidget::itemChanged, this, [this](QListWidgetItem* changed) {
            if (changed->checkState() != Qt::Checked) return;
            for (int i = 0; i < recipientListWidget_->count(); ++i) {
                QListWidgetItem* other = recipientListWidget_->item(i);
                if (other != changed && other->checkState() == Qt::Checked) other->setCheckState(Qt::Unchecked);
            }
        });
        col->addWidget(recipientListWidget_);

        // LiveSearchLineEdit이 한글 조합 중(커밋 전) preedit 문자열까지 합쳐서 liveTextChanged를 낸다.
        connect(recipientSearchEdit_, &LiveSearchLineEdit::liveTextChanged, this,
                &RobotDialog::applyRecipientFilter);

        auto* hint = new QLabel(QStringLiteral("배정하면 대상자 정보의 담당 로봇이 함께 바뀝니다"), this);
        hint->setProperty("role", "muted");
        hint->setStyleSheet(QStringLiteral("font-size:11px;"));
        col->addWidget(hint);
        body->addLayout(col);
    }

    // 연결 정보 / 연결 상태
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* label = new QLabel(isEdit ? QStringLiteral("연결 상태") : QStringLiteral("연결 정보"), this);
        label->setProperty("role", "label");
        col->addWidget(label);

        auto* preview = new QFrame(this);
        preview->setStyleSheet(
            QStringLiteral("background:%1; border-radius:6px;").arg(Tokens::BG_SUNKEN.name()));
        auto* previewLayout = new QVBoxLayout(preview);
        previewLayout->setContentsMargins(9, 8, 9, 8);
        previewLayout->setSpacing(2);

        auto* urlLine = new QLabel(
            QStringLiteral("ws://%1:8080/ws/robot/%2").arg(data.wsHost, data.code),
            preview);
        urlLine->setFont(QFont(Tokens::Font::MONO));
        urlLine->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_SECONDARY.name()));
        previewLayout->addWidget(urlLine);

        if (isEdit) {
            auto* statusLine = new QLabel(preview);
            statusLine->setFont(QFont(Tokens::Font::MONO));
            const QString statusText = data.connected
                ? QStringLiteral("● 연결됨 · %1").arg(data.connectionDetail)
                : QStringLiteral("● 연결 끊김");
            statusLine->setText(statusText);
            statusLine->setStyleSheet(QStringLiteral("font-size:11px; color:%1;")
                                           .arg((data.connected ? Tokens::OK.main : Tokens::TEXT_MUTED).name()));
            previewLayout->addWidget(statusLine);
        } else {
            auto* noteLine = new QLabel(QStringLiteral("로봇에서 위 주소로 접속 · 서버는 대기만 함"), preview);
            noteLine->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
            previewLayout->addWidget(noteLine);
        }
        col->addWidget(preview);

        if (!isEdit) {
            auto* hint = new QLabel(
                QStringLiteral("등록만으로는 연결되지 않습니다. 로봇 측 설정에 코드와 주소를 동일하게 넣으세요."),
                this);
            hint->setProperty("role", "muted");
            hint->setWordWrap(true);
            hint->setStyleSheet(QStringLiteral("font-size:11px;"));
            col->addWidget(hint);
        }
        body->addLayout(col);
    }

    // 등록 해제 (Edit 전용)
    if (isEdit) {
        auto* sectionLabel = new QLabel(QStringLiteral("등록 해제"), this);
        sectionLabel->setStyleSheet(QStringLiteral("font-size:12px; font-weight:600; color:%1; "
                                                     "border-top:1px solid %2; padding-top:12px;")
                                         .arg(Tokens::TEXT_PRIMARY.name(), Tokens::BORDER.name()));
        body->addWidget(sectionLabel);

        auto* dangerBanner = new QFrame(this);
        dangerBanner->setObjectName("banner");
        dangerBanner->setProperty("level", Tokens::Prop::EMERGENCY);
        auto* dl = new QHBoxLayout(dangerBanner);
        dl->setContentsMargins(9, 7, 9, 7);
        dl->setSpacing(8);
        auto* dIcon = new QLabel(dangerBanner);
        dIcon->setPixmap(SimpleIcons::alertTriangle(Tokens::EMERGENCY.main, 14).pixmap(14, 14));
        dl->addWidget(dIcon);
        auto* dText = new QLabel(
            QStringLiteral("해제해도 과거 이벤트·영상 기록은 남습니다. 담당 대상자는 미배정으로 바뀌고, "
                            "해당 코드로 들어오는 WebSocket 접속은 거부됩니다."),
            dangerBanner);
        dText->setWordWrap(true);
        dl->addWidget(dText, 1);
        body->addWidget(dangerBanner);

        auto* deregisterBtn = new QPushButton(QStringLiteral("이 로봇 등록 해제"), this);
        deregisterBtn->setProperty("variant", "danger");
        deregisterBtn->setProperty("size", "s");
        deregisterBtn->setAutoDefault(false);
        connect(deregisterBtn, &QPushButton::clicked, this, [this] {
            emit robotDeregisterRequested(originalCode_);
            reject();
        });
        body->addWidget(deregisterBtn, 0, Qt::AlignLeft);
    }

    root->addLayout(body);
    root->addWidget(buildFooter(isEdit ? QStringLiteral("저장") : QStringLiteral("등록")));
}

QFrame* RobotDialog::buildHeader(const QString& title) {
    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    auto* titleLabel = new QLabel(title, header);
    titleLabel->setStyleSheet(QStringLiteral("font-size:15px; font-weight:600; color:%1;")
                                   .arg(Tokens::TEXT_PRIMARY.name()));
    layout->addWidget(titleLabel);
    layout->addStretch();

    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_MUTED, 15), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    closeBtn->setAutoDefault(false);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(closeBtn);
    return header;
}

QFrame* RobotDialog::buildFooter(const QString& primaryLabel) {
    auto* footer = new QFrame(this);
    footer->setObjectName("modalFooter");
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);
    layout->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("취소"), footer);
    cancelBtn->setProperty("variant", "secondary");
    // autoDefault를 꺼야 Enter가 이 버튼(취소/저장)을 "기본 버튼"으로 여겨 자동으로
    // 눌러버리지 않는다 — 그냥 두면 입력창에서 Enter만 쳐도 다이얼로그가 닫혀버린다.
    cancelBtn->setAutoDefault(false);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancelBtn);

    saveBtn_ = new QPushButton(primaryLabel, footer);
    saveBtn_->setProperty("variant", "primary");
    saveBtn_->setAutoDefault(false);
    connect(saveBtn_, &QPushButton::clicked, this, &RobotDialog::handleSaveClicked);
    layout->addWidget(saveBtn_);
    return footer;
}

void RobotDialog::handleSaveClicked() {
    // In Register mode, get from codeEdit; in Edit mode, use originalCode
    const QString code = (mode_ == Mode::Edit) ? originalCode_ : codeEdit_->text().trimmed();
    const QString name = nameEdit_->text().trimmed();

    // Determine which fields are empty (only check codeEdit_ in Register mode)
    const bool codeIsEmpty = (mode_ == Mode::Register) && codeEdit_->text().trimmed().isEmpty();
    const bool nameIsEmpty = name.isEmpty();

    // Update codeEdit_ styling (only in Register mode)
    if (mode_ == Mode::Register) {
        if (codeIsEmpty) {
            codeEdit_->setProperty("state", "error");
        } else {
            codeEdit_->setProperty("state", QVariant());
        }
        codeEdit_->style()->unpolish(codeEdit_);
        codeEdit_->style()->polish(codeEdit_);
    }

    // Update nameEdit_ styling
    if (nameIsEmpty) {
        nameEdit_->setProperty("state", "error");
    } else {
        nameEdit_->setProperty("state", QVariant());
    }
    nameEdit_->style()->unpolish(nameEdit_);
    nameEdit_->style()->polish(nameEdit_);

    // Don't proceed if any required field is empty
    if (codeIsEmpty || nameIsEmpty) {
        return;
    }

    RobotData result;
    result.code = code;
    result.name = name;
    result.model = modelCombo_->currentText();
    result.rosDomainId = rosDomainEdit_->text().trimmed().toInt();
    result.recipientLabel = QStringLiteral("미배정");
    for (int i = 0; i < recipientListWidget_->count(); ++i) {
        QListWidgetItem* item = recipientListWidget_->item(i);
        if (item->checkState() == Qt::Checked) {
            result.recipientLabel = item->text();
            break;
        }
    }
    emit robotSaved(result);
    accept();
}

void RobotDialog::applyRecipientFilter(const QString& query) {
    for (int i = 0; i < recipientListWidget_->count(); ++i) {
        auto* item = recipientListWidget_->item(i);
        item->setHidden(!KoreanSearch::matches(item->text(), query));
    }
}

void RobotDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}
