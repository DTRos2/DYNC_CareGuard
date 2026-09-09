#include "p7_account_dialog.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "theme/tokens.h"
#include "widgets/icon_line_edit.h"
#include "widgets/korean_search.h"
#include "widgets/live_search_line_edit.h"
#include "widgets/simple_icons.h"

namespace {
// 8자 이상 · 영문/숫자 조합 — 계정등록·비밀번호 변경 양쪽에서 공유하는 정책.
bool isPasswordStrongEnough(const QString& password) {
    if (password.size() < 8) return false;
    const bool hasLetter = std::any_of(password.begin(), password.end(), [](QChar c) { return c.isLetter(); });
    const bool hasDigit = std::any_of(password.begin(), password.end(), [](QChar c) { return c.isDigit(); });
    return hasLetter && hasDigit;
}

QString roleLabel(AccountDialog::Role role) {
    switch (role) {
        case AccountDialog::Role::Admin: return QStringLiteral("관리자");
        case AccountDialog::Role::Operator: return QStringLiteral("관제 담당자");
        case AccountDialog::Role::Guardian: return QStringLiteral("보호자");
    }
    return QString();
}

// 체크박스를 셀 배경(rowBg)과 함께 가운데 정렬해 테이블 셀에 심는다 — p7_settings_page.cpp의
// wrapCellWidget()과 같은 목적이나, 번역 단위가 달라 여기서 별도로 둔다.
QWidget* wrapCheckboxCell(QCheckBox* checkbox, const QColor& rowBg) {
    auto* wrap = new QWidget();
    wrap->setAutoFillBackground(true);
    QPalette pal = wrap->palette();
    pal.setColor(QPalette::Window, rowBg);
    wrap->setPalette(pal);

    auto* layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(checkbox);
    return wrap;
}
}  // namespace

AccountDialog::AccountDialog(Mode mode, const AccountData& data, const QStringList& existingLoginIds,
                              const QVector<RecipientOption>& recipients, QWidget* parent)
    : QDialog(parent),
      mode_(mode),
      data_(data),
      existingLoginIds_(existingLoginIds),
      recipients_(recipients) {
    setModal(true);
    // OS 창관리자 타이틀바를 없애 커스텀 헤더(제목·X)만 남긴다 — 안 그러면 X가 두 개로 보임
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
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

    // 로그인 ID
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
        body->addLayout(col);
    }

    // 표시 이름
    {
        auto* col = new QVBoxLayout();
        col->setSpacing(5);
        auto* label = new QLabel(QStringLiteral("표시 이름*"), this);
        label->setProperty("role", "label");
        col->addWidget(label);
        nameEdit_ = new QLineEdit(data.displayName, this);
        nameEdit_->setPlaceholderText(QStringLiteral("예) 홍길동"));
        col->addWidget(nameEdit_);
        body->addLayout(col);
    }

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
            passwordEdit_ = new IconLineEdit(this);
            passwordEdit_->setEchoMode(QLineEdit::Password);
            passwordEdit_->setTrailingIcon(SimpleIcons::eye(Tokens::TEXT_MUTED, 14));
            connect(passwordEdit_, &IconLineEdit::trailingIconClicked, this,
                    &AccountDialog::togglePasswordVisibility);
            col->addWidget(passwordEdit_);
            row2->addLayout(col, 1);
        }
        {
            auto* col = new QVBoxLayout();
            col->setSpacing(5);
            auto* label = new QLabel(QStringLiteral("비밀번호 확인*"), this);
            label->setProperty("role", "label");
            col->addWidget(label);
            passwordConfirmEdit_ = new IconLineEdit(this);
            passwordConfirmEdit_->setEchoMode(QLineEdit::Password);
            passwordConfirmEdit_->setTrailingIcon(SimpleIcons::eye(Tokens::TEXT_MUTED, 14));
            connect(passwordConfirmEdit_, &IconLineEdit::trailingIconClicked, this,
                    &AccountDialog::togglePasswordConfirmVisibility);
            col->addWidget(passwordConfirmEdit_);
            row2->addLayout(col, 1);
        }
        body->addLayout(row2);
        auto* pwHint = new QLabel(QStringLiteral("8자 이상 · 영문/숫자 조합"), this);
        pwHint->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
        body->addWidget(pwHint);

        auto updatePasswordMatchHint = [this, pwHint] {
            const QString pw = passwordEdit_->text();
            const QString confirm = passwordConfirmEdit_->text();
            if (confirm.isEmpty()) {
                pwHint->setText(QStringLiteral("8자 이상 · 영문/숫자 조합"));
                pwHint->setStyleSheet(
                    QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
            } else if (pw == confirm) {
                pwHint->setText(QStringLiteral("비밀번호가 일치합니다"));
                pwHint->setStyleSheet(
                    QStringLiteral("font-size:11px; color:%1;").arg(Tokens::OK.main.name()));
            } else {
                pwHint->setText(QStringLiteral("비밀번호가 일치하지 않습니다"));
                pwHint->setStyleSheet(
                    QStringLiteral("font-size:11px; color:%1;").arg(Tokens::EMERGENCY.main.name()));
            }
        };
        connect(passwordEdit_, &QLineEdit::textChanged, this, updatePasswordMatchHint);
        connect(passwordConfirmEdit_, &QLineEdit::textChanged, this, updatePasswordMatchHint);
    } else {
        auto* changeBtn = new QPushButton(QStringLiteral("변경"), this);
        changeBtn->setProperty("variant", "secondary");
        changeBtn->setProperty("size", "s");
        changeBtn->setAutoDefault(false);

        // 1번째 줄 입력창 폭을 2번째 줄(입력창+버튼)의 입력창 폭과 맞추기 위해,
        // 실제 버튼과 동일한 크기의 투명 자리표시자를 같은 위치에 둔다.
        auto* btnSpacer = new QPushButton(QStringLiteral("변경"), this);
        btnSpacer->setProperty("variant", "secondary");
        btnSpacer->setProperty("size", "s");
        QSizePolicy spacerPolicy = btnSpacer->sizePolicy();
        spacerPolicy.setRetainSizeWhenHidden(true);
        btnSpacer->setSizePolicy(spacerPolicy);
        btnSpacer->hide();

        auto* pwLabel = new QLabel(QStringLiteral("비밀번호"), this);
        pwLabel->setProperty("role", "label");
        body->addWidget(pwLabel);

        auto* newPwRow = new QHBoxLayout();
        newPwRow->setSpacing(8);
        passwordEdit_ = new IconLineEdit(this);
        passwordEdit_->setEchoMode(QLineEdit::Password);
        passwordEdit_->setTrailingIcon(SimpleIcons::eye(Tokens::TEXT_MUTED, 14));
        connect(passwordEdit_, &IconLineEdit::trailingIconClicked, this,
                &AccountDialog::togglePasswordVisibility);
        newPwRow->addWidget(passwordEdit_, 1);
        newPwRow->addWidget(btnSpacer);
        body->addLayout(newPwRow);

        auto* pwConfirmLabel = new QLabel(QStringLiteral("비밀번호 확인"), this);
        pwConfirmLabel->setProperty("role", "label");
        body->addWidget(pwConfirmLabel);

        auto* confirmRow = new QHBoxLayout();
        confirmRow->setSpacing(8);
        passwordConfirmEdit_ = new IconLineEdit(this);
        passwordConfirmEdit_->setEchoMode(QLineEdit::Password);
        passwordConfirmEdit_->setTrailingIcon(SimpleIcons::eye(Tokens::TEXT_MUTED, 14));
        connect(passwordConfirmEdit_, &IconLineEdit::trailingIconClicked, this,
                &AccountDialog::togglePasswordConfirmVisibility);
        confirmRow->addWidget(passwordConfirmEdit_, 1);
        confirmRow->addWidget(changeBtn);
        body->addLayout(confirmRow);

        auto* pwHint = new QLabel(QStringLiteral("8자 이상 · 영문/숫자 조합"), this);
        pwHint->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
        body->addWidget(pwHint);

        auto updateEditPasswordMatchHint = [this, pwHint] {
            const QString pw = passwordEdit_->text();
            const QString confirm = passwordConfirmEdit_->text();
            if (confirm.isEmpty()) {
                pwHint->setText(QStringLiteral("8자 이상 · 영문/숫자 조합"));
                pwHint->setStyleSheet(
                    QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
            } else if (pw == confirm) {
                pwHint->setText(QStringLiteral("비밀번호가 일치합니다"));
                pwHint->setStyleSheet(
                    QStringLiteral("font-size:11px; color:%1;").arg(Tokens::OK.main.name()));
            } else {
                pwHint->setText(QStringLiteral("비밀번호가 일치하지 않습니다"));
                pwHint->setStyleSheet(
                    QStringLiteral("font-size:11px; color:%1;").arg(Tokens::EMERGENCY.main.name()));
            }
        };
        connect(passwordEdit_, &QLineEdit::textChanged, this, updateEditPasswordMatchHint);
        connect(passwordConfirmEdit_, &QLineEdit::textChanged, this, updateEditPasswordMatchHint);

        connect(changeBtn, &QPushButton::clicked, this, [this] {
            if (passwordEdit_->text().isEmpty() || passwordEdit_->text() != passwordConfirmEdit_->text()) {
                passwordConfirmEdit_->setProperty("state", "error");
                passwordConfirmEdit_->style()->unpolish(passwordConfirmEdit_);
                passwordConfirmEdit_->style()->polish(passwordConfirmEdit_);
                return;
            }
            passwordConfirmEdit_->setProperty("state", QVariant());
            passwordConfirmEdit_->style()->unpolish(passwordConfirmEdit_);
            passwordConfirmEdit_->style()->polish(passwordConfirmEdit_);

            if (!isPasswordStrongEnough(passwordEdit_->text())) {
                QMessageBox::warning(this, QStringLiteral("비밀번호 변경 불가"),
                                      QStringLiteral("비밀번호는 8자 이상이며 영문과 숫자를 모두 포함해야 합니다."));
                return;
            }

            // 실제 변경 성공/실패 알림은 서버 응답을 아는 P7SettingsPage 쪽에서 띄운다.
            emit passwordChangeRequested(data_.loginId, passwordEdit_->text());
            passwordEdit_->clear();
            passwordConfirmEdit_->clear();
        });
    }

    buildRoleSelector(body, data.role);

    // 계정 상태 — 등록 모드는 항상 활성으로 등록되므로 이 섹션을 노출하지 않는다.
    activeCheck_ = new QCheckBox(QStringLiteral("활성"), this);
    if (isEdit) {
        auto* statusLabel = new QLabel(QStringLiteral("계정 상태"), this);
        statusLabel->setStyleSheet(QStringLiteral("font-size:12px; font-weight:600; color:%1; "
                                                    "border-top:1px solid %2; padding-top:12px;")
                                        .arg(Tokens::TEXT_PRIMARY.name(), Tokens::BORDER.name()));
        body->addWidget(statusLabel);

        auto* statusRow = new QHBoxLayout();
        statusRow->setSpacing(7);
        activeCheck_->setChecked(data.active);
        statusRow->addWidget(activeCheck_);
        if (!data.lastLogin.isEmpty()) {
            statusRow->addStretch();
            auto* lastLoginLabel = new QLabel(QStringLiteral("최종 로그인 %1").arg(data.lastLogin), this);
            lastLoginLabel->setFont(QFont(Tokens::Font::MONO));
            lastLoginLabel->setStyleSheet(
                QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
            statusRow->addWidget(lastLoginLabel);
        } else {
            statusRow->addStretch();
        }
        body->addLayout(statusRow);
    } else {
        activeCheck_->setChecked(true);
        activeCheck_->hide();
    }

    // 담당 대상자 — 검색으로 좁혀서 목록에서 체크로 고른다. 하나도 안 고르면 미배정으로 저장된다.
    {
        auto* chargeLabel = new QLabel(QStringLiteral("담당 대상자"), this);
        chargeLabel->setStyleSheet(QStringLiteral("font-size:12px; font-weight:600; color:%1; "
                                                    "border-top:1px solid %2; padding-top:12px;")
                                        .arg(Tokens::TEXT_PRIMARY.name(), Tokens::BORDER.name()));
        body->addWidget(chargeLabel);

        auto* chargeHint = new QLabel(QStringLiteral("대상자 1명당 담당자 1명 — 다른 담당자에게 배정된 "
                                                       "대상자를 선택하면 그 담당자에게서 해제됩니다"),
                                       this);
        chargeHint->setWordWrap(true);
        chargeHint->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
        body->addWidget(chargeHint);

        recipientSearchEdit_ = new LiveSearchLineEdit(this);
        recipientSearchEdit_->setPlaceholderText(QStringLiteral("이름 또는 코드로 검색"));
        body->addWidget(recipientSearchEdit_);

        // 대상자 / 현재 담당 2열 표 — 로봇·계정 관리 탭의 QTableWidget과 같은 방식(테두리 없는
        // 헤더, 행별 배경색 지정)을 그대로 따른다. 체크박스는 0열 셀에 위젯으로 심는다.
        recipientTable_ = new QTableWidget(this);
        recipientTable_->setObjectName("recipientTable");
        recipientTable_->setColumnCount(3);
        recipientTable_->setHorizontalHeaderLabels({QString(), QStringLiteral("대상자"), QStringLiteral("현재 담당")});
        recipientTable_->verticalHeader()->setVisible(false);
        recipientTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        recipientTable_->setSelectionMode(QAbstractItemView::NoSelection);
        recipientTable_->setFocusPolicy(Qt::NoFocus);
        recipientTable_->setWordWrap(false);
        recipientTable_->setFixedHeight(140);
        // 대상자·현재 담당 칸 사이(와 체크박스 칸 사이)를 세로선으로 구분한다. 가로선은
        // QTableView::item의 기본 border-bottom과 겹쳐 두 줄로 보이므로 그 규칙만 꺼서
        // 세로 그리드선만 남긴다.
        recipientTable_->setShowGrid(true);
        recipientTable_->setStyleSheet(
            QStringLiteral("QTableWidget#recipientTable { gridline-color: %1; } "
                            "QTableWidget#recipientTable::item { border: none; }")
                .arg(Tokens::BORDER.name()));
        auto* recipientHeader = recipientTable_->horizontalHeader();
        recipientHeader->setSectionResizeMode(QHeaderView::Interactive);
        recipientHeader->setSectionResizeMode(1, QHeaderView::Stretch);
        recipientTable_->setColumnWidth(0, 48);
        recipientTable_->setColumnWidth(2, 90);

        recipientTable_->setRowCount(recipients.size());
        for (int r = 0; r < recipients.size(); ++r) {
            const RecipientOption& option = recipients.at(r);

            // 현재 담당 상태를 "현재 담당" 칸에 보여준다 — 미지정은 주의색, 본인 담당은
            // 강조(행 배경까지), 다른 계정 담당은 그 계정 이름을 보여줘 재배정 여부를
            // 판단할 수 있게 한다.
            QString statusText;
            QColor statusColor = Tokens::TEXT_SECONDARY;
            bool highlightSelf = false;
            if (option.currentOperatorId == -1) {
                statusText = QStringLiteral("미지정");
                statusColor = Tokens::ATTENTION.main;
            } else if (option.currentOperatorId == data_.dbId) {
                statusText = QStringLiteral("본인");
                statusColor = Tokens::OK.main;
                highlightSelf = true;
            } else {
                statusText = option.currentOperatorName;
            }
            const QColor rowBg = highlightSelf ? Tokens::OK.soft : Tokens::BG_SURFACE;

            auto* nameItem = new QTableWidgetItem(option.label);
            nameItem->setBackground(rowBg);
            nameItem->setForeground(Tokens::TEXT_PRIMARY);
            recipientTable_->setItem(r, 1, nameItem);

            auto* statusItem = new QTableWidgetItem(statusText);
            statusItem->setBackground(rowBg);
            statusItem->setForeground(statusColor);
            if (highlightSelf) {
                QFont statusFont = statusItem->font();
                statusFont.setWeight(QFont::DemiBold);
                statusItem->setFont(statusFont);
            }
            recipientTable_->setItem(r, 2, statusItem);

            auto* checkbox = new QCheckBox();
            checkbox->setChecked(option.assigned);
            recipientTable_->setCellWidget(r, 0, wrapCheckboxCell(checkbox, rowBg));

            // id는 "대상자" 칸의 아이템에 실어둔다 — 검색 필터·저장 시 이 아이템의 텍스트/데이터로 조회한다.
            nameItem->setData(Qt::UserRole, option.id);

            // 이미 다른 계정이 담당 중인 대상자를 체크하면 재배정 확인 팝업을 띄운다.
            const int recipientId = option.id;
            connect(checkbox, &QCheckBox::toggled, this, [this, checkbox, recipientId](bool checked) {
                handleRecipientReassignCheck(checkbox, recipientId, checked);
            });
        }
        // "대상자" 칸(1열)은 어디를 클릭해도 그 행 체크박스가 토글되게 한다 — 체크박스
        // 자체는 좁아서 정확히 맞히기 번거롭다. 체크박스 칸(0열)은 이미 위젯이 클릭을
        // 직접 받아 처리하므로 여기서는 1열만 다룬다.
        connect(recipientTable_, &QTableWidget::cellClicked, this, [this](int row, int column) {
            if (column != 1) return;
            QWidget* cell = recipientTable_->cellWidget(row, 0);
            auto* checkbox = cell != nullptr ? cell->findChild<QCheckBox*>() : nullptr;
            if (checkbox != nullptr) checkbox->toggle();
        });
        body->addWidget(recipientTable_);

        // LiveSearchLineEdit이 한글 조합 중(커밋 전) preedit 문자열까지 합쳐서 liveTextChanged를 낸다.
        connect(recipientSearchEdit_, &LiveSearchLineEdit::liveTextChanged, this,
                &AccountDialog::applyRecipientFilter);

        if (recipients.isEmpty()) {
            auto* emptyHint = new QLabel(QStringLiteral("등록된 대상자가 없습니다"), this);
            emptyHint->setStyleSheet(QStringLiteral("font-size:11px; color:%1;").arg(Tokens::TEXT_MUTED.name()));
            body->addWidget(emptyHint);
        }
    }

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
        btn->setAutoDefault(false);
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
    const bool isEdit = (mode_ == Mode::Edit);
    const bool admin = (selectedRole() == Role::Admin);
    const bool showSelfBlock = admin && data_.isSelf;
    const bool showLastAdminBlock = admin && !data_.isSelf && data_.isLastActiveAdmin && activeCheck_->isChecked();
    const bool showAdminWarn = admin && !showSelfBlock && !showLastAdminBlock && isEdit;

    adminWarnBanner_->setVisible(showAdminWarn);
    selfBlockBanner_->setVisible(showSelfBlock);
    lastAdminBlockBanner_->setVisible(showLastAdminBlock);
    // 체크박스의 활성/비활성은 현재 선택된 역할과 무관해야 한다 — 관리자에서 다른
    // 역할로 바꿨다가 다시 관리자로 돌아오는 우회로 본인/마지막 관리자 계정을
    // 비활성화할 수 있으면 안 된다(리뷰 지적 #4).
    activeCheck_->setEnabled(!(data_.isSelf || data_.isLastActiveAdmin));
}

void AccountDialog::togglePasswordVisibility() {
    passwordVisible_ = !passwordVisible_;
    passwordEdit_->setEchoMode(passwordVisible_ ? QLineEdit::Normal : QLineEdit::Password);
    passwordEdit_->setTrailingIcon(passwordVisible_ ? SimpleIcons::eyeOff(Tokens::TEXT_MUTED, 14)
                                                     : SimpleIcons::eye(Tokens::TEXT_MUTED, 14));
}

void AccountDialog::togglePasswordConfirmVisibility() {
    passwordConfirmVisible_ = !passwordConfirmVisible_;
    passwordConfirmEdit_->setEchoMode(passwordConfirmVisible_ ? QLineEdit::Normal : QLineEdit::Password);
    passwordConfirmEdit_->setTrailingIcon(passwordConfirmVisible_
                                               ? SimpleIcons::eyeOff(Tokens::TEXT_MUTED, 14)
                                               : SimpleIcons::eye(Tokens::TEXT_MUTED, 14));
}

void AccountDialog::applyRecipientFilter(const QString& query) {
    for (int r = 0; r < recipientTable_->rowCount(); ++r) {
        const QTableWidgetItem* nameItem = recipientTable_->item(r, 1);
        recipientTable_->setRowHidden(r, !KoreanSearch::matches(nameItem->text(), query));
    }
}

void AccountDialog::handleRecipientReassignCheck(QCheckBox* checkbox, int recipientId, bool checked) {
    if (recipientReassignGuard_) return;
    if (!checked) return;  // 해제는 확인 없이 허용

    const RecipientOption* option = nullptr;
    for (const RecipientOption& opt : recipients_) {
        if (opt.id == recipientId) {
            option = &opt;
            break;
        }
    }
    if (option == nullptr) return;
    // 미배정이었거나 이미 본인 담당이던 대상자는 재배정이 아니므로 확인이 필요 없다.
    if (option->currentOperatorId == -1 || option->currentOperatorId == data_.dbId) return;

    int otherCount = 0;
    for (const RecipientOption& opt : recipients_) {
        if (opt.currentOperatorId == option->currentOperatorId) ++otherCount;
    }

    const QString typedName = nameEdit_->text().trimmed();
    const QString newOwnerName = typedName.isEmpty() ? data_.displayName : typedName;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("담당자 재배정"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("%1(%2)는 현재 %3 담당입니다.\n%4에게 재배정할까요?")
                    .arg(option->name, option->code, option->currentOperatorName, newOwnerName));
    box.setInformativeText(QStringLiteral("%1의 담당은 %2명 → %3명이 됩니다")
                                .arg(option->currentOperatorName)
                                .arg(otherCount)
                                .arg(otherCount - 1));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("재배정"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("취소"));
    if (box.exec() != QMessageBox::Ok) {
        recipientReassignGuard_ = true;
        checkbox->setChecked(false);
        recipientReassignGuard_ = false;
    }
}

void AccountDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
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
    closeBtn->setAutoDefault(false);
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
    // autoDefault를 꺼야 Enter가 이 버튼(취소/저장)을 "기본 버튼"으로 여겨 자동으로
    // 눌러버리지 않는다 — 그냥 두면 입력창에서 Enter만 쳐도 다이얼로그가 닫혀버린다.
    cancelBtn->setAutoDefault(false);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancelBtn);
    auto* saveBtn = new QPushButton(primaryLabel, footer);
    saveBtn->setProperty("variant", "primary");
    saveBtn->setAutoDefault(false);
    connect(saveBtn, &QPushButton::clicked, this, &AccountDialog::handleSaveClicked);
    layout->addWidget(saveBtn);
    return footer;
}

void AccountDialog::handleSaveClicked() {
    const bool isEdit = (mode_ == Mode::Edit);
    const QString loginId = isEdit ? data_.loginId : loginIdEdit_->text().trimmed();
    const QString name = nameEdit_->text().trimmed();

    // 로그인 ID 필드 상태 갱신 (Register 모드에서만 — Edit 모드는 read-only/잠금)
    if (!isEdit) {
        loginIdEdit_->setProperty("state", loginId.isEmpty() ? "error" : QVariant());
        loginIdEdit_->style()->unpolish(loginIdEdit_);
        loginIdEdit_->style()->polish(loginIdEdit_);
    }

    nameEdit_->setProperty("state", name.isEmpty() ? "error" : QVariant());
    nameEdit_->style()->unpolish(nameEdit_);
    nameEdit_->style()->polish(nameEdit_);

    // 담당 대상자 — 각 행 0열 셀에 심어둔 체크박스 상태를 모은다.
    QVector<int> matchedRecipientIds;
    for (int r = 0; r < recipientTable_->rowCount(); ++r) {
        const QWidget* cell = recipientTable_->cellWidget(r, 0);
        const auto* checkbox = (cell != nullptr) ? cell->findChild<QCheckBox*>() : nullptr;
        if (checkbox != nullptr && checkbox->isChecked()) {
            matchedRecipientIds.append(recipientTable_->item(r, 1)->data(Qt::UserRole).toInt());
        }
    }

    if (loginId.isEmpty() || name.isEmpty()) return;
    if (!isEdit && existingLoginIds_.contains(loginId)) return;
    if (!isEdit) {
        if (passwordEdit_->text().isEmpty() || passwordEdit_->text() != passwordConfirmEdit_->text()) {
            passwordConfirmEdit_->setProperty("state", "error");
            passwordConfirmEdit_->style()->unpolish(passwordConfirmEdit_);
            passwordConfirmEdit_->style()->polish(passwordConfirmEdit_);
            return;
        }
        passwordConfirmEdit_->setProperty("state", QVariant());
        passwordConfirmEdit_->style()->unpolish(passwordConfirmEdit_);
        passwordConfirmEdit_->style()->polish(passwordConfirmEdit_);

        if (!isPasswordStrongEnough(passwordEdit_->text())) {
            QMessageBox::warning(this, QStringLiteral("계정 등록 불가"),
                                  QStringLiteral("비밀번호는 8자 이상이며 영문과 숫자를 모두 포함해야 합니다."));
            return;
        }
    }

    // 활성 계정을 비활성화하려는데 본인 담당 대상자가 남아 있으면, 그 대상자들이
    // 미지정 상태가 된다는 것을 알리고 확인을 받는다(취소하면 저장 자체를 취소).
    if (isEdit && data_.active && !activeCheck_->isChecked()) {
        QStringList affected;
        for (const RecipientOption& opt : recipients_) {
            if (opt.currentOperatorId == data_.dbId) affected << QStringLiteral("%1 %2").arg(opt.name, opt.code);
        }
        if (!affected.isEmpty()) {
            QMessageBox box(this);
            box.setWindowTitle(QStringLiteral("계정 비활성화"));
            box.setIcon(QMessageBox::Warning);
            box.setText(QStringLiteral("%1 계정을 비활성화하면\n담당 대상자 %2명이 미지정 상태가 됩니다.")
                            .arg(name)
                            .arg(affected.size()));
            box.setInformativeText(affected.join(QStringLiteral(" · ")) +
                                    QStringLiteral("\n재배정 후 비활성화하는 것을 권합니다"));
            box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
            box.setDefaultButton(QMessageBox::Cancel);
            box.button(QMessageBox::Ok)->setText(QStringLiteral("비활성화"));
            box.button(QMessageBox::Cancel)->setText(QStringLiteral("취소"));
            if (box.exec() != QMessageBox::Ok) return;
        }
    }

    AccountData result = data_;
    result.loginId = loginId;
    result.displayName = name;
    if (!isEdit) result.password = passwordEdit_->text();
    result.role = selectedRole();
    result.active = activeCheck_->isChecked();
    result.assignedRecipientIds = matchedRecipientIds;
    emit accountSaved(result);
    accept();
}
