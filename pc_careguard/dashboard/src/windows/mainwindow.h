#pragma once
#include <QMainWindow>

class QLabel;
class QStackedWidget;
class QButtonGroup;
class QTimer;
class QIcon;
class P4VideoPage;
class P7SettingsPage;
class WsClient;

// 공통 셸: TopBar + Sidebar + QStackedWidget.
// CareGuard_DESIGN_SYSTEM.md §3(공통 셸 구조), §4-2(objectName), §6(페이지 인벤토리) 참고.
// 각 페이지의 메인 뷰 구현은 담당 팀원이 pages/*.cpp에서 채운다 — 이 클래스는 셸(틀)만 담당한다.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void setUserName(const QString& name);
    void setEmergencyCount(int count);
    void setAttentionCount(int count);

signals:
    void logoutRequested();

private:
    QWidget* buildTopBar();
    QWidget* buildSidebar();
    void addNavButton(class QVBoxLayout* sidebarLayout, const QIcon& icon, const QString& text,
                       int pageIndex, QButtonGroup* group);
    // 사이드바 클릭이 아닌 코드에서 페이지를 전환할 때(P3 "이벤트 확인" 등) 사이드바 체크
    // 상태도 같이 맞춰준다.
    void switchToPage(int pageIndex);

    QStackedWidget* pageStack_;
    QButtonGroup* navGroup_ = nullptr;
    P4VideoPage* videoPage_ = nullptr;
    P7SettingsPage* settingsPage_ = nullptr;
    // 앱 전체가 공유하는 /ws/dashboard 연결 하나. 예전엔 페이지(P1/P3/P4/P5/P6/P7/P8)마다
    // 자기 WsClient를 따로 만들어서 서버에 동일 엔드포인트로 소켓이 7개씩 열려 있었다
    // (`lsof -i :8000`으로 확인, 2026-09-14). 여기서 하나만 만들어 각 페이지로 내려준다.
    WsClient* wsClient_ = nullptr;
    QLabel* clockLabel_;
    QLabel* userLabel_;
    QLabel* emergencyBadge_;
    QLabel* attentionBadge_;
};
