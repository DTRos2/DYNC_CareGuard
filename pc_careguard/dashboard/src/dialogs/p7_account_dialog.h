#pragma once
#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

class QLineEdit;
class QPushButton;
class QCheckBox;
class QFrame;
class QLabel;
class QVBoxLayout;
class IconLineEdit;
class LiveSearchLineEdit;
class CheckableListWidget;
class QKeyEvent;

// P7 계정 등록·수정 모달 — p7_robot_account_modal_wireframe_rev2.html §3~4 대응.
class AccountDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Register, Edit };
    enum class Role { Admin, Operator, Guardian };

    // 담당 배정 후보 한 명. recipient.operator_id로 저장되는 대상자.
    struct RecipientOption {
        int id = -1;
        QString code;   // "C-001"
        QString label;  // "김순자 (C-001) · R-01" 형태. 선택 목록의 항목 표시 텍스트로 쓴다
        bool assigned = false;  // 이 계정이 현재 담당 중인지 (목록 초기 체크 상태에 반영)
    };

    struct AccountData {
        QString loginId;
        QString displayName;
        QString password;  // Register 모드에서만 채워짐. 서버 전송용 평문 — 해시는 서버가 한다
        Role role = Role::Operator;
        bool active = true;
        QString lastLogin;              // Edit 모드 표시용
        bool isSelf = false;             // 현재 로그인한 본인 계정인지
        bool isLastActiveAdmin = false;  // 활성 관리자가 이 계정 하나뿐인지
        QVector<int> assignedRecipientIds;  // 저장 결과 — 체크된 대상자 id 목록. 비었으면 미배정
    };

    // recipients: 등록·수정 모두 "담당 대상자" 선택 목록의 원본이다. 하나도 체크하지
    // 않으면 미배정으로 저장된다.
    AccountDialog(Mode mode, const AccountData& data, const QStringList& existingLoginIds,
                  const QVector<RecipientOption>& recipients = {}, QWidget* parent = nullptr);

signals:
    void accountSaved(const AccountData& data);
    void passwordChangeRequested(const QString& loginId, const QString& newPassword);

private:
    QFrame* buildHeader(const QString& title);
    QFrame* buildFooter(const QString& primaryLabel);
    void buildRoleSelector(QVBoxLayout* body, Role initial);
    void handleActiveToggled(bool checked);
    void handleSaveClicked();
    Role selectedRole() const;
    void updateAdminBanners();
    void togglePasswordVisibility();
    void togglePasswordConfirmVisibility();
    void applyRecipientFilter(const QString& query);
    // Enter/Return으로는 저장·닫힘이 안 되게 막는다(오타 입력 중 실수로 저장되는 것 방지).
    // Esc는 QDialog 기본 동작(reject)이 그대로 처리하도록 둔다.
    void keyPressEvent(QKeyEvent* event) override;

    Mode mode_;
    AccountData data_;
    QStringList existingLoginIds_;
    QVector<RecipientOption> recipients_;

    QLineEdit* loginIdEdit_;
    QLabel* loginIdErrorHint_;
    QLineEdit* nameEdit_;
    QPushButton* roleAdminBtn_;
    QPushButton* roleOperatorBtn_;
    QPushButton* roleGuardianBtn_;
    IconLineEdit* passwordEdit_;
    IconLineEdit* passwordConfirmEdit_;
    bool passwordVisible_ = false;
    bool passwordConfirmVisible_ = false;
    QCheckBox* activeCheck_;
    QFrame* adminWarnBanner_;
    QFrame* selfBlockBanner_;
    QFrame* lastAdminBlockBanner_;
    LiveSearchLineEdit* recipientSearchEdit_ = nullptr;
    CheckableListWidget* recipientListWidget_ = nullptr;
};
