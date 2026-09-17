#include "guardian_mail_dialog.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "net/api_client.h"
#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {
QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", "label");
    return label;
}
}  // namespace

GuardianMailDialog::GuardianMailDialog(int recipientId, const QString& recipientName, const QString& defaultEmail,
                                         QWidget* parent)
    : QDialog(parent), recipientId_(recipientId), recipientName_(recipientName) {
    setModal(true);
    // EventDetailModal·P3RecipientCreateDialog와 같은 패턴 — OS 타이틀바를 없애고
    // 커스텀 헤더(제목·X)만 남긴다.
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setFixedWidth(420);
    setWindowTitle(QStringLiteral("보호자 메일"));

    apiClient_ = new ApiClient(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 0, 16, 0);
    headerLayout->setAlignment(Qt::AlignVCenter);
    header->setFixedHeight(46);

    auto* titleLabel = new QLabel(QStringLiteral("보호자 메일 · %1").arg(recipientName_), header);
    QFont titleFont = titleLabel->font();
    titleFont.setPixelSize(14);
    titleFont.setWeight(QFont::Medium);
    titleLabel->setFont(titleFont);
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();

    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_SECONDARY, 16), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    headerLayout->addWidget(closeBtn);
    root->addWidget(header);

    auto* body = new QWidget(this);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(6);

    layout->addWidget(makeSectionLabel(QStringLiteral("받는사람"), body));
    toEmailEdit_ = new QLineEdit(body);
    toEmailEdit_->setPlaceholderText(QStringLiteral("guardian@example.com"));
    toEmailEdit_->setText(defaultEmail);
    layout->addWidget(toEmailEdit_);
    if (defaultEmail.isEmpty()) {
        // 보호자 연락처에 이메일이 없는 대상자 — 자동으로 못 채웠다는 걸 알려주고
        // 직접 입력하게 한다(막지는 않는다 — 담당자가 임의 주소로 보낼 수도 있음).
        auto* hint = new QLabel(QStringLiteral("등록된 보호자 이메일이 없습니다 — 직접 입력해주세요."), body);
        hint->setProperty("role", "secondary");
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }
    layout->addSpacing(6);

    layout->addWidget(makeSectionLabel(QStringLiteral("메시지"), body));
    messageEdit_ = new QTextEdit(body);
    messageEdit_->setPlaceholderText(QStringLiteral("보호자에게 전달할 내용을 입력하세요"));
    messageEdit_->setFixedHeight(120);
    layout->addWidget(messageEdit_);
    layout->addSpacing(4);

    sendBtn_ = new QPushButton(QStringLiteral("메일 보내기"), body);
    sendBtn_->setProperty("variant", "primary");
    connect(sendBtn_, &QPushButton::clicked, this, &GuardianMailDialog::handleSendClicked);
    layout->addWidget(sendBtn_);

    root->addWidget(body);
}

void GuardianMailDialog::handleSendClicked() {
    const QString toEmail = toEmailEdit_->text().trimmed();
    if (toEmail.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("보호자 메일"), QStringLiteral("받는사람 이메일을 입력해주세요."));
        return;
    }
    const QString message = messageEdit_->toPlainText().trimmed();
    if (message.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("보호자 메일"), QStringLiteral("메시지를 입력해주세요."));
        return;
    }

    sendBtn_->setEnabled(false);
    sendBtn_->setText(QStringLiteral("보내는 중…"));
    apiClient_->sendGuardianMail(recipientId_, toEmail, message, [this](bool ok, const QString& err) {
        if (!ok) {
            sendBtn_->setEnabled(true);
            sendBtn_->setText(QStringLiteral("메일 보내기"));
            QMessageBox::warning(this, QStringLiteral("발송 실패"),
                                  err.isEmpty() ? QStringLiteral("서버 연결을 확인해주세요.") : err);
            return;
        }
        QMessageBox::information(this, QStringLiteral("보호자 메일"), QStringLiteral("메일을 보냈습니다."));
        accept();
    });
}
