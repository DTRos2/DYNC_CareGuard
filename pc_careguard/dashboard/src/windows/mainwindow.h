#pragma once
#include <QMainWindow>
#include <QMap>

class QLabel;
class QStackedWidget;
class QButtonGroup;
class QTimer;
class QIcon;
class P2EventPage;
class P3RecipientPage;
class P5MapPage;
class P6PatrolPage;
class P4VideoPage;
class P7SettingsPage;
class P8LogsPage;
class QPushButton;

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
    void setUserRole(const QString& role);
    void setAuthToken(const QString& token);    

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
    P2EventPage* eventPage_ = nullptr;
    P3RecipientPage* recipientPage_ = nullptr;
    P5MapPage* mapPage_ = nullptr;
    P6PatrolPage* patrolPage_ = nullptr;
    P4VideoPage* videoPage_ = nullptr;
    P7SettingsPage* settingsPage_ = nullptr;
    P8LogsPage* logsPage_ = nullptr;
    QLabel* clockLabel_;
    QLabel* userLabel_;
    QLabel* emergencyBadge_;
    QLabel* attentionBadge_;
    QMap<int, QPushButton*> navButtons_;
    QLabel* recordGroupLabel_ = nullptr;
    QLabel* settingsGroupLabel_ = nullptr;
    QLabel* logGroupLabel_ = nullptr;
    QString role_;
};
