#pragma once
#include <QMainWindow>
#include <QPair>
#include <QVector>

class QLabel;
class QStackedWidget;
class QButtonGroup;
class QPushButton;
class QTimer;
class QIcon;
class P2EventPage;
class P3RecipientPage;
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
    // role: careguard_user.role.code(admin/operator/guardian). 사이드바 메뉴·기본 진입 페이지를
    // role에 맞게 다시 구성하고, P3/P2에 guardian 대상자 필터를, P7에 operator 읽기 전용
    // 탭 구성을 내려준다. 로그아웃 후 다른 계정으로 재로그인해도 안전하게 다시 호출 가능.
    void setUserRole(const QString& role, int userId);
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
    // setUserRole()가 호출. 페이지 인덱스 -> role별 노출 여부를 계산해 사이드바 버튼/그룹
    // 라벨을 보이거나 숨기고, 지금 보고 있는 페이지가 숨겨질 페이지면 허용된 첫 페이지로 옮긴다.
    void applyRoleVisibility(const QString& role);

    QStackedWidget* pageStack_;
    QButtonGroup* navGroup_ = nullptr;
    P2EventPage* eventPage_ = nullptr;
    P3RecipientPage* recipientPage_ = nullptr;
    P4VideoPage* videoPage_ = nullptr;
    P7SettingsPage* settingsPage_ = nullptr;

    // idx 0~7 = p1~p8, buildSidebar()가 채운다. applyRoleVisibility()가 role에 따라 보이거나
    // 숨긴다.
    QVector<QPushButton*> navButtons_;
    // (그룹 라벨, 그 아래 속한 페이지 인덱스들) — 그룹 내 버튼이 전부 숨겨지면 라벨도 같이 숨긴다.
    QVector<QPair<QLabel*, QVector<int>>> navGroupLabels_;
    // 앱 전체가 공유하는 /ws/dashboard 연결 하나. 예전엔 페이지(P1/P3/P4/P5/P6/P7/P8)마다
    // 자기 WsClient를 따로 만들어서 서버에 동일 엔드포인트로 소켓이 7개씩 열려 있었다
    // (`lsof -i :8000`으로 확인, 2026-09-14). 여기서 하나만 만들어 각 페이지로 내려준다.
    WsClient* wsClient_ = nullptr;
    QLabel* clockLabel_;
    QLabel* userLabel_;
    QLabel* emergencyBadge_;
    QLabel* attentionBadge_;
};
