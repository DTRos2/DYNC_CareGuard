#pragma once
#include <QWidget>

class QLineEdit;
class QLabel;
class QCheckBox;
class QPushButton;
class QFrame;
class QAction;
class QTimer;
class QIcon;

// 로그인 화면. gui/wireframe/v1/p0_login_final_wireframe.html 기준.
// 인증은 아직 서버 연동 전이므로 매직 비밀번호로 각 상태를 시연하는 스텁이다.
// 실제 인증 API 연동은 pc_careguard/server 작업 후 resolveLogin()에서 교체한다
// (shared/api_contract 참고). 매직 값: "wrong" → 인증 실패(5회 누적 시 잠금),
// "offline" → 서버 연결 실패, 그 외 비어있지 않은 값 → 성공.
class LoginWindow : public QWidget {
    Q_OBJECT
public:
    explicit LoginWindow(QWidget* parent = nullptr);

signals:
    void loginSucceeded(const QString& username);

private slots:
    void onSubmit();
    void togglePasswordVisibility();
    void onLockTick();

private:
    enum class State { Idle, Authenticating, AuthFailed, Locked, ServerError };

    void resolveLogin(const QString& id, const QString& password);
    void setState(State state);
    void enterLocked();
    void applyBanner(const QString& title, const QString& subtitleHtml, const QString& level,
                      const QIcon& icon);
    void hideBanner();
    void clearPasswordError();

    QLineEdit* idInput_;
    QLineEdit* passwordInput_;
    QAction* eyeAction_;
    bool passwordVisible_ = false;
    QCheckBox* rememberCheckbox_;

    QWidget* inlineErrorRow_;
    QLabel* inlineErrorIcon_;
    QLabel* inlineErrorText_;

    QFrame* banner_;
    QLabel* bannerIcon_;
    QLabel* bannerTitle_;
    QLabel* bannerSubtitle_;

    QPushButton* loginBtn_;

    State state_ = State::Idle;
    int failCount_ = 0;
    int lockRemainingSec_ = 0;
    QTimer* lockTimer_;
    bool submitting_ = false;
};
