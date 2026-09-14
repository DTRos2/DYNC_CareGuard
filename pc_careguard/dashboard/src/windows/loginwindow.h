#pragma once
#include <QWidget>

class IconLineEdit;
class QLabel;
class QCheckBox;
class QPushButton;
class QFrame;
class QTimer;
class QIcon;
class QNetworkAccessManager;
class QNetworkReply;

// 로그인 화면. gui/wireframe/v1/p0_login_final_wireframe.html 기준.
// POST {서버주소}/auth/login (shared/api_contract/auth.yaml 참고)로 인증한다.
// 서버 주소 결정 방식은 net/server_config.h::resolveServerUrl() 참고.
// 실패 잠금(5회)은 서버가 아닌 클라이언트 로컬 상태로만 관리한다.
class LoginWindow : public QWidget {
    Q_OBJECT
public:
    explicit LoginWindow(QWidget* parent = nullptr);

signals:
    void loginSucceeded(const QString& username, const QString& role, const QString& token);

private slots:
    void onSubmit();
    void togglePasswordVisibility();
    void onLockTick();

private:
    enum class State { Idle, Authenticating, AuthFailed, Locked, ServerError };

    void resolveLogin(const QString& id, const QString& password);
    void handleLoginReply(QNetworkReply* reply, const QString& id);
    void setState(State state);
    void enterLocked();
    void applyBanner(const QString& title, const QString& subtitleHtml, const QString& level,
                      const QIcon& icon);
    void hideBanner();
    void clearPasswordError();
    void setInlineErrorText(const QString& text);

    IconLineEdit* idInput_;
    IconLineEdit* passwordInput_;
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

    QNetworkAccessManager* netManager_;
    QString serverUrl_;
};
