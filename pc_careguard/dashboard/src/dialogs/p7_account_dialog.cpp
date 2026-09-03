#include "p7_account_dialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {
QString roleLabel(AccountDialog::Role role) {
    switch (role) {
        case AccountDialog::Role::Admin: return QStringLiteral("관리자");
        case AccountDialog::Role::Operator: return QStringLiteral("관제 담당자");
        case AccountDialog::Role::Guardian: return QStringLiteral("보호자");
    }
    return QString();
}
}  // namespace

AccountDialog::AccountDialog(Mode mode, const AccountData& data, const QStringList& existingLoginIds,
                              QWidget* parent)
    : QDialog(parent), mode_(mode), data_(data), existingLoginIds_(existingLoginIds) {
    setModal(true);
    setFixedWidth(Tokens::Layout::MODAL_W_S);
    const bool isEdit = (mode_ == Mode::Edit);
    const QString title =
        isEdit ? QStringLiteral("계정 수정 · %1").arg(data.loginId) : QStringLiteral("계정 등록");
    setWindowTitle(title);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildHeader(title));

    auto* body = new QVBoxLayout();
    body->setContentsMargins(16, 14, 16, 14);
    body->setSpacing(12);

    // 로그인 ID / 표시 이름
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(8);
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* label = new QLabel(QStringLiteral("로그인 ID*"), this);
        label->setProperty("role", "label");
        col->addWidget(label);
        loginIdEdit_ = new QLineEdit(this);
        loginIdEdit_->setFont(QFont(Tokens::Font::MONO));
        if (isEdit) {
            loginIdEdit_->setReadOnly(true);
            loginIdEdit_->setText(data.loginId + QStringLiteral(" 🔒"));
            loginIdEdit_->setStyleSheet(QStringLiteral("background:%1; color:%2;")
                                             .arg(Tokens::BG_SUNKEN.name(), Tokens::TEXT_MUTED.name()));
        } else {
            loginIdEdit_->setPlaceholderText(QStringLiteral("예) op_kang"));
            connect(loginIdEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
                const bool dup = existingLoginIds_.contains(text.trimmed());
                loginIdEdit_->setProperty("state", dup ? "error" : QVariant());
                loginIdEdit_->style()->unpolish(loginIdEdit_);
                loginIdEdit_->style()->polish(loginIdEdit_);
                loginIdErrorHint_->setVisible(dup);
            });
        }
        col->addWidget(loginIdEdit_);
        loginIdErrorHint_ = new QLabel(QStringLiteral("이미 사용 중인 ID입니다"), this);
        loginIdErrorHint_->setStyleSheet(
            QStringLiteral("font-size:11px; color:%1;").arg(Tokens::EMERGENCY.main.name()));
        loginIdErrorHint_->setVisible(false);
        col->addWidget(loginIdErrorHint_);
        row1->addLayout(col, 1);
    }
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* labelRow = new QHBoxLayout();
        labelRow->setSpacing(5);
        auto* label = new QLabel(QStringLiteral("표시 이름*"), this);
        label->setProperty("role", "label");
        labelRow->addWidget(label);
        auto* tag = new QLabel(QStringLiteral("컬럼 필요"), this);
        tag->setStyleSheet(QStringLiteral("font-size:10px; font-weight:500; padding:1px 5px; "
                                            "border-radius:4px; background:%1; color:%2; border:1px solid %3;")
                                .arg(Tokens::ATTENTION.soft.name(), Tokens::ATTENTION.main.name(),
                                     Tokens::ATTENTION.border.name()));
        labelRow->addWidget(tag);
        labelRow->addStretch();
        col->addLayout(labelRow);
        nameEdit_ = new QLineEdit(data.displayName, this);
        col->addWidget(nameEdit_);
        auto* hint = new QLabel(
            QStringLiteral("v2에서 <span style=\"font-family:'JetBrains Mono'\">display_name</span> 이 "
                            "빠졌습니다 — 스키마 갭 확인 필요"),
            this);
        hint->setTextFormat(Qt::RichText);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
        col->addWidget(hint);
        row1->addLayout(col, 1);
    }
    body->addLayout(row1);

    buildRoleSelector(body, data.role);

    // 비밀번호
    if (!isEdit) {
        auto* row2 = new QHBoxLayout();
        row2->setSpacing(8);
        {
            auto* col = new QVBoxLayout();
            col->setSpacing(5);
            auto* label = new QLabel(QStringLiteral("비밀번호*"), this);
            label->setProperty("role", "label");
            col->addWidget(label);
            passwordEdit_ = new QLineEdit(this);
            passwordEdit_->setEchoMode(QLineEdit::Password);
            col->addWidget(passwordEdit_);
            row2->addLayout(col, 1);
        }
        {
            auto* col = new QVBoxLayout();
            col->setSpacing(5);
            auto* label = new QLabel(QStringLiteral("비밀번호 확인*"), this);
            label->setProperty("role", "label");
            col->addWidget(label);
            passwordConfirmEdit_ = new QLineEdit(this);
            passwordConfirmEdit_->setEchoMode(QLineEdit::Password);
            col->addWidget(passwordConfirmEdit_);
            row2->addLayout(col, 1);
        }
        body->addLayout(row2);
        auto* pwHint = new QLabel(QStringLiteral("8자 이상 · 영문/숫자 조합"), this);
        pwHint->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
        body->addWidget(pwHint);
    } else {
        passwordEdit_ = nullptr;
        passwordConfirmEdit_ = nullptr;
        auto* label = new QLabel(QStringLiteral("비밀번호"), this);
        label->setProperty("role", "label");
        body->addWidget(label);
        auto* pwRow = new QHBoxLayout();
        pwRow->setSpacing(8);
        auto* maskedField = new QLineEdit(QStringLiteral("••••••••"), this);
        maskedField->setReadOnly(true);
        maskedField->setEnabled(false);
        pwRow->addWidget(maskedField, 1);
        auto* resetBtn = new QPushButton(QStringLiteral("초기화"), this);
        resetBtn->setProperty("variant", "secondary");
        resetBtn->setProperty("size", "s");
        connect(resetBtn, &QPushButton::clicked, this,
                [this] { emit passwordResetRequested(data_.loginId); });
        pwRow->addWidget(resetBtn);
        body->addLayout(pwRow);
        auto* pwHint = new QLabel(QStringLiteral("임시 비밀번호가 발급되고 다음 로그인 시 변경을 요구합니다"), this);
        pwHint->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
        body->addWidget(pwHint);
    }

    // 계정 상태
    auto* statusLabel = new QLabel(QStringLiteral("계정 상태"), this);
    statusLabel->setStyleSheet(QStringLiteral("font-size:12px; font-weight:600; color:%1; "
                                                "border-top:1px solid %2; padding-top:12px;")
                                    .arg(Tokens::TEXT_PRIMARY.name(), Tokens::BORDER.name()));
    body->addWidget(statusLabel);

    auto* statusRow = new QHBoxLayout();
    statusRow->setSpacing(7);
    activeCheck_ = new QCheckBox(isEdit ? QStringLiteral("활성") : QStringLiteral("등록 즉시 활성화"), this);
    activeCheck_->setChecked(data.active);
    statusRow->addWidget(activeCheck_);
    if (isEdit && !data.lastLogin.isEmpty()) {
        statusRow->addStretch();
        auto* lastLoginLabel = new QLabel(QStringLiteral("최종 로그인 %1").arg(data.lastLogin), this);
        lastLoginLabel->setFont(QFont(Tokens::Font::MONO));
        lastLoginLabel->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
        statusRow->addWidget(lastLoginLabel);
    } else {
        statusRow->addStretch();
    }
    body->addLayout(statusRow);

    // 관리자 관련 배너 3종 (기본 숨김, updateAdminBanners에서 상태에 맞춰 하나만 표시)
    adminWarnBanner_ = new QFrame(this);
    adminWarnBanner_->setObjectName("banner");
    adminWarnBanner_->setProperty("level", Tokens::Prop::ATTENTION);
    {
        auto* l = new QHBoxLayout(adminWarnBanner_);
        l->setContentsMargins(9, 7, 9, 7);
        l->setSpacing(8);
        auto* icon = new QLabel(adminWarnBanner_);
        icon->setPixmap(SimpleIcons::alertTriangle(Tokens::ATTENTION.main, 14).pixmap(14, 14));
        l->addWidget(icon);
        auto* text = new QLabel(
            QStringLiteral("<b>관리자 계정</b>입니다. 비활성화하면 즉시 로그아웃되고 "
                            "시스템 설정에 접근할 수 없게 됩니다."),
            adminWarnBanner_);
        text->setTextFormat(Qt::RichText);
        text->setWordWrap(true);
        l->addWidget(text, 1);
    }
    body->addWidget(adminWarnBanner_);

    selfBlockBanner_ = new QFrame(this);
    selfBlockBanner_->setObjectName("banner");
    selfBlockBanner_->setProperty("level", Tokens::Prop::INFO);
    {
        auto* l = new QHBoxLayout(selfBlockBanner_);
        l->setContentsMargins(9, 7, 9, 7);
        l->setSpacing(8);
        auto* icon = new QLabel(selfBlockBanner_);
        icon->setPixmap(SimpleIcons::infoCircle(Tokens::INFO.main, 14).pixmap(14, 14));
        l->addWidget(icon);
        auto* text = new QLabel(QStringLiteral("현재 로그인한 본인 계정은 비활성화할 수 없습니다."),
                                 selfBlockBanner_);
        text->setWordWrap(true);
        l->addWidget(text, 1);
    }
    body->addWidget(selfBlockBanner_);

    lastAdminBlockBanner_ = new QFrame(this);
    lastAdminBlockBanner_->setObjectName("banner");
    lastAdminBlockBanner_->setProperty("level", Tokens::Prop::ATTENTION);
    {
        auto* l = new QHBoxLayout(lastAdminBlockBanner_);
        l->setContentsMargins(9, 7, 9, 7);
        l->setSpacing(8);
        auto* icon = new QLabel(lastAdminBlockBanner_);
        icon->setPixmap(SimpleIcons::alertTriangle(Tokens::ATTENTION.main, 14).pixmap(14, 14));
        l->addWidget(icon);
        auto* text = new QLabel(
            QStringLiteral("활성 관리자가 이 계정 하나뿐입니다. 먼저 다른 계정에 관리자 역할을 부여하세요."),
            lastAdminBlockBanner_);
        text->setWordWrap(true);
        l->addWidget(text, 1);
    }
    body->addWidget(lastAdminBlockBanner_);

    root->addLayout(body);
    root->addWidget(buildFooter(isEdit ? QStringLiteral("저장") : QStringLiteral("등록")));

    connect(activeCheck_, &QCheckBox::toggled, this, &AccountDialog::handleActiveToggled);
    updateAdminBanners();
}

void AccountDialog::buildRoleSelector(QVBoxLayout* body, Role initial) {
    auto* col = new QVBoxLayout();
    col->setSpacing(5);
    auto* label = new QLabel(QStringLiteral("역할*"), this);
    label->setProperty("role", "label");
    col->addWidget(label);

    auto* row = new QHBoxLayout();
    row->setSpacing(0);
    roleAdminBtn_ = new QPushButton(QStringLiteral("관리자"), this);
    roleOperatorBtn_ = new QPushButton(QStringLiteral("관제 담당자"), this);
    roleGuardianBtn_ = new QPushButton(QStringLiteral("보호자"), this);

    auto* group = new QButtonGroup(this);
    int idx = 0;
    for (auto* btn : {roleAdminBtn_, roleOperatorBtn_, roleGuardianBtn_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        group->addButton(btn, idx++);
        row->addWidget(btn);
    }
    roleAdminBtn_->setProperty("pos", "first");
    roleGuardianBtn_->setProperty("pos", "last");
    roleGuardianBtn_->setEnabled(false);  // role.is_active = FALSE — 선택 불가

    switch (initial) {
        case Role::Admin: roleAdminBtn_->setChecked(true); break;
        case Role::Operator: roleOperatorBtn_->setChecked(true); break;
        case Role::Guardian: roleGuardianBtn_->setChecked(true); break;
    }
    connect(group, &QButtonGroup::idClicked, this, [this](int) { updateAdminBanners(); });
    col->addLayout(row);

    auto* hint = new QLabel(
        QStringLiteral("보호자 역할은 <span style=\"font-family:'JetBrains Mono'\">role.is_active = "
                        "FALSE</span> — 선택 불가"),
        this);
    hint->setTextFormat(Qt::RichText);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
    col->addWidget(hint);
    body->addLayout(col);
}

AccountDialog::Role AccountDialog::selectedRole() const {
    if (roleAdminBtn_->isChecked()) return Role::Admin;
    if (roleGuardianBtn_->isChecked()) return Role::Guardian;
    return Role::Operator;
}

void AccountDialog::updateAdminBanners() {
    const bool admin = (selectedRole() == Role::Admin);
    const bool showSelfBlock = admin && data_.isSelf;
    const bool showLastAdminBlock = admin && !data_.isSelf && data_.isLastActiveAdmin && activeCheck_->isChecked();
    const bool showAdminWarn = admin && !showSelfBlock && !showLastAdminBlock;

    adminWarnBanner_->setVisible(showAdminWarn);
    selfBlockBanner_->setVisible(showSelfBlock);
    lastAdminBlockBanner_->setVisible(showLastAdminBlock);
    activeCheck_->setEnabled(!(showSelfBlock || showLastAdminBlock));
}

void AccountDialog::handleActiveToggled(bool) {
    // 관리자 계정 비활성화에 별도 확인 팝업은 없다(범위 제외 — 사용자 지시).
    // 본인 계정/마지막 활성 관리자는 updateAdminBanners()가 체크박스 자체를
    // disable해서 여기 도달하지 않는다. 그 외 관리자는 그냥 평범한 체크박스처럼
    // 바로 꺼지고, 저장 버튼을 눌러야 실제로 반영된다(handleSaveClicked).
    updateAdminBanners();
}

QFrame* AccountDialog::buildHeader(const QString& title) {
    auto* header = new QFrame(this);
    header->setObjectName("modalHeader");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);
    auto* titleLabel = new QLabel(title, header);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:15px; font-weight:600; color:%1;").arg(Tokens::TEXT_PRIMARY.name()));
    layout->addWidget(titleLabel);
    layout->addStretch();
    auto* closeBtn = new QPushButton(SimpleIcons::x(Tokens::TEXT_MUTED, 15), QString(), header);
    closeBtn->setProperty("variant", "ghost");
    closeBtn->setProperty("size", "s");
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(closeBtn);
    return header;
}

QFrame* AccountDialog::buildFooter(const QString& primaryLabel) {
    auto* footer = new QFrame(this);
    footer->setObjectName("modalFooter");
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);
    layout->addStretch();
    auto* cancelBtn = new QPushButton(QStringLiteral("취소"), footer);
    cancelBtn->setProperty("variant", "secondary");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancelBtn);
    auto* saveBtn = new QPushButton(primaryLabel, footer);
    saveBtn->setProperty("variant", "primary");
    connect(saveBtn, &QPushButton::clicked, this, &AccountDialog::handleSaveClicked);
    layout->addWidget(saveBtn);
    return footer;
}

void AccountDialog::handleSaveClicked() {
    const bool isEdit = (mode_ == Mode::Edit);
    const QString loginId = isEdit ? data_.loginId : loginIdEdit_->text().trimmed();
    const QString name = nameEdit_->text().trimmed();

    if (loginId.isEmpty() || name.isEmpty()) return;
    if (!isEdit && existingLoginIds_.contains(loginId)) return;
    if (!isEdit && (passwordEdit_->text().isEmpty() || passwordEdit_->text() != passwordConfirmEdit_->text())) {
        passwordConfirmEdit_->setProperty("state", "error");
        passwordConfirmEdit_->style()->unpolish(passwordConfirmEdit_);
        passwordConfirmEdit_->style()->polish(passwordConfirmEdit_);
        return;
    }

    AccountData result = data_;
    result.loginId = loginId;
    result.displayName = name;
    result.role = selectedRole();
    result.active = activeCheck_->isChecked();
    emit accountSaved(result);
    accept();
}
