#pragma once
#include <QDialog>
#include <QString>
#include <QVector>

class QLineEdit;
class QComboBox;
class QFrame;
class QPushButton;
class LiveSearchLineEdit;
class CheckableListWidget;
class QKeyEvent;

// P7 로봇 등록·수정 모달 — p7_robot_account_modal_wireframe_rev2.html §1~2 대응.
class RobotDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Register, Edit };

    // 담당 대상자 선택지 한 명 — AccountDialog::RecipientOption과 동일 출처(recipients_,
    // fetchRecipients() 결과)를 그대로 받아 쓴다. 로봇 한 대엔 대상자 한 명만 배정되므로
    // 초기 체크 상태는 data.recipientLabel과 비교해서 정한다(assigned 필드는 없다).
    struct RecipientOption {
        int id = -1;
        QString label;  // "김순자 (C-001)" 또는 이미 다른 로봇 담당 중이면 "김순자 (C-001) · R-01"
        int currentGuardianId = -1;
        QString currentGuardianName;
    };

    struct RobotData {
        QString code;                                    // 로봇 코드 — Edit에서는 잠금
        QString name;                                     // 표시 이름
        QString model = QStringLiteral("limo");
        int rosDomainId = 0;
        QString ip;                                         // 로봇에 고정 할당된 IP, 등록 시에만 저장됨
        QString recipientLabel = QStringLiteral("미배정");  // "이말순 (C-003)" 또는 "미배정"
        int recipientId = -1;                               // 체크된 대상자의 recipient.id, 없으면 -1
        bool connected = false;                            // Edit 전용 — 연결 상태 미리보기에 사용
        QString wsHost = QStringLiteral("<서버>");          // ws://<wsHost>:8080/ws/robot/<code>
        QString connectionDetail;                          // "최근 ping 응답 0.4s 전 · 세션 3d 2h" (연결 시)
    };

    RobotDialog(Mode mode, const RobotData& data, const QVector<RecipientOption>& recipients = {},
                QWidget* parent = nullptr);

signals:
    void robotSaved(const RobotData& data);
    void robotDeregisterRequested(const QString& code);

private:
    QFrame* buildHeader(const QString& title);
    QFrame* buildFooter(const QString& primaryLabel);
    void handleSaveClicked();
    void applyRecipientFilter(const QString& query);
    // Enter/Return으로는 저장·닫힘이 안 되게 막는다(AccountDialog와 동일 이유).
    // Esc는 QDialog 기본 동작(reject)이 그대로 처리하도록 둔다.
    void keyPressEvent(QKeyEvent* event) override;

    Mode mode_;
    QString originalCode_;
    QLineEdit* codeEdit_;
    QLineEdit* nameEdit_;
    QComboBox* modelCombo_;
    QLineEdit* rosDomainEdit_;
    QLineEdit* ipEdit_;
    LiveSearchLineEdit* recipientSearchEdit_;
    CheckableListWidget* recipientListWidget_;
    QPushButton* saveBtn_;
};
