#pragma once
#include <QDialog>
#include <QString>

class QLineEdit;
class QComboBox;
class QFrame;
class QPushButton;

// P7 로봇 등록·수정 모달 — p7_robot_account_modal_wireframe_rev2.html §1~2 대응.
class RobotDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Register, Edit };

    struct RobotData {
        QString code;                                    // 로봇 코드 — Edit에서는 잠금
        QString name;                                     // 표시 이름
        QString model = QStringLiteral("limo");
        int rosDomainId = 0;
        QString recipientLabel = QStringLiteral("미배정");  // "이말순 (C-003)" 또는 "미배정"
        bool connected = false;                            // Edit 전용 — 연결 상태 미리보기에 사용
        QString wsHost = QStringLiteral("<서버>");          // ws://<wsHost>:8080/ws/robot/<code>
        QString connectionDetail;                          // "최근 ping 응답 0.4s 전 · 세션 3d 2h" (연결 시)
        QString warnNode;                                  // 비어있으면 경고 배너 없음, 아니면 노드 이름
        QString warnDetail;                                // "응답 지연 2.1s"
    };

    RobotDialog(Mode mode, const RobotData& data, QWidget* parent = nullptr);

signals:
    void robotSaved(const RobotData& data);
    void robotDeregisterRequested(const QString& code);

private:
    QFrame* buildHeader(const QString& title);
    QFrame* buildFooter(const QString& primaryLabel);
    void handleSaveClicked();

    Mode mode_;
    QString originalCode_;
    QLineEdit* codeEdit_;
    QLineEdit* nameEdit_;
    QComboBox* modelCombo_;
    QLineEdit* rosDomainEdit_;
    QComboBox* recipientCombo_;
    QPushButton* saveBtn_;
};
