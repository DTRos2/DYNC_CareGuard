#pragma once
#include <QMainWindow>

class QLabel;
class QStackedWidget;
class QButtonGroup;
class QTimer;
class QIcon;
class P4VideoPage;

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

    QStackedWidget* pageStack_;
    P4VideoPage* videoPage_ = nullptr;
    QLabel* clockLabel_;
    QLabel* userLabel_;
    QLabel* emergencyBadge_;
    QLabel* attentionBadge_;
};
