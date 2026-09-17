#pragma once
#include <QDialog>

class QLineEdit;
class QTextEdit;
class QPushButton;
class ApiClient;

// P2 이벤트 상세 모달 "보호자 메일" 버튼이 여는 모달. 받는사람 이메일(1순위 보호자로
// 기본 채워지지만 수정 가능)과 메시지 본문을 받아 POST /recipients/{id}/guardian-mail로
// 즉시 발송한다. exec()이 Accepted로 끝나면 실제로 발송에 성공한 것이다(실패 시에는
// 모달이 안 닫히고 그대로 남는다 — 다시 시도하거나 주소를 고칠 수 있게).
class GuardianMailDialog : public QDialog {
    Q_OBJECT
public:
    // recipientId: 발송 로그(notification_log)를 남길 대상자 id.
    // defaultEmail: 1순위 활성 보호자 이메일 — 없으면 빈 문자열(직접 입력 안내 표시).
    explicit GuardianMailDialog(int recipientId, const QString& recipientName, const QString& defaultEmail,
                                 QWidget* parent = nullptr);

private:
    void handleSendClicked();

    int recipientId_ = 0;
    QString recipientName_;
    ApiClient* apiClient_ = nullptr;

    QLineEdit* toEmailEdit_ = nullptr;
    QTextEdit* messageEdit_ = nullptr;
    QPushButton* sendBtn_ = nullptr;
};
