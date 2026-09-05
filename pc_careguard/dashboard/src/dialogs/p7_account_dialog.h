#pragma once
#include <QDialog>
#include <QString>
#include <QStringList>

class QLineEdit;
class QPushButton;
class QCheckBox;
class QFrame;
class QLabel;
class QVBoxLayout;

// P7 계정 등록·수정 모달 — p7_robot_account_modal_wireframe_rev2.html §3~4 대응.
class AccountDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Register, Edit };
    enum class Role { Admin, Operator, Guardian };

    struct AccountData {
        QString loginId;
        QString displayName;
        Role role = Role::Operator;
        bool active = true;
        QString lastLogin;              // Edit 모드 표시용
        bool isSelf = false;             // 현재 로그인한 본인 계정인지
        bool isLastActiveAdmin = false;  // 활성 관리자가 이 계정 하나뿐인지
    };

    AccountDialog(Mode mode, const AccountData& data, const QStringList& existingLoginIds,
                  QWidget* parent = nullptr);

signals:
    void accountSaved(const AccountData& data);
    void passwordResetRequested(const QString& loginId);

private:
    QFrame* buildHeader(const QString& title);
    QFrame* buildFooter(const QString& primaryLabel);
    void buildRoleSelector(QVBoxLayout* body, Role initial);
    void handleActiveToggled(bool checked);
    void handleSaveClicked();
    Role selectedRole() const;
    void updateAdminBanners();

    Mode mode_;
    AccountData data_;
    QStringList existingLoginIds_;

    QLineEdit* loginIdEdit_;
    QLabel* loginIdErrorHint_;
    QLineEdit* nameEdit_;
    QPushButton* roleAdminBtn_;
    QPushButton* roleOperatorBtn_;
    QPushButton* roleGuardianBtn_;
    QLineEdit* passwordEdit_;
    QLineEdit* passwordConfirmEdit_;
    QCheckBox* activeCheck_;
    QFrame* adminWarnBanner_;
    QFrame* selfBlockBanner_;
    QFrame* lastAdminBlockBanner_;
};
