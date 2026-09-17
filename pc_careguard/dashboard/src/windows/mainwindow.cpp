#include "mainwindow.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "pages/p1_overview_page.h"
#include "pages/p2_event_page.h"
#include "pages/p3_recipient_page.h"
#include "pages/p4_video_page.h"
#include "pages/p5_map_page.h"
#include "pages/p6_patrol_page.h"
#include "pages/p7_settings_page.h"
#include "pages/p8_logs_page.h"
#include "net/ws_client.h"
#include "theme/tokens.h"
#include "widgets/simple_icons.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setMinimumSize(Tokens::Layout::MIN_W, Tokens::Layout::MIN_H);
    setWindowTitle(QStringLiteral("CareGuard"));

    auto* central = new QWidget(this);
    central->setObjectName("centralWidget");
    setCentralWidget(central);

    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    rootLayout->addWidget(buildTopBar());

    auto* body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    body->addWidget(buildSidebar());

    pageStack_ = new QStackedWidget(central);
    auto* overviewPage = new P1OverviewPage(pageStack_);
    pageStack_->addWidget(overviewPage);                     // idx 0
    eventPage_ = new P2EventPage(pageStack_);
    pageStack_->addWidget(eventPage_);                       // idx 1
    recipientPage_ = new P3RecipientPage(pageStack_);
    pageStack_->addWidget(recipientPage_);                   // idx 2
    videoPage_ = new P4VideoPage(pageStack_);
    pageStack_->addWidget(videoPage_);                       // idx 3
    auto* mapPage = new P5MapPage(pageStack_);
    pageStack_->addWidget(mapPage);                          // idx 4
    auto* patrolPage = new P6PatrolPage(pageStack_);
    pageStack_->addWidget(patrolPage);                       // idx 5
    settingsPage_ = new P7SettingsPage(pageStack_);
    pageStack_->addWidget(settingsPage_);                    // idx 6
    auto* logsPage = new P8LogsPage(pageStack_);
    pageStack_->addWidget(logsPage);                          // idx 7
    body->addWidget(pageStack_, /*stretch=*/1);

    // /ws/dashboard 연결 하나를 앱 전체가 공유한다 — 페이지마다 만들면 소켓이 페이지
    // 수만큼 중복으로 열린다(§wsClient_ 주석 참고). 모든 페이지가 구독을 마친 뒤에
    // connectToServer()를 불러야, 첫 연결이 이미 끝난 뒤에 뒤늦게 구독해서
    // connectionStateChanged(true)를 못 받는 페이지(P5/P6/P7)가 생기지 않는다.
    wsClient_ = new WsClient(this);
    overviewPage->setWsClient(wsClient_);
    recipientPage_->setWsClient(wsClient_);
    videoPage_->setWsClient(wsClient_);
    mapPage->setWsClient(wsClient_);
    patrolPage->setWsClient(wsClient_);
    settingsPage_->setWsClient(wsClient_);
    logsPage->setWsClient(wsClient_);
    wsClient_->connectToServer();

    // P5에서 파일로 맵을 등록하면 P6의 맵 선택지에도 반영한다. mapRegistered 시그널엔
    // 서버 map.id/resolution/origin이 안 실려 있어(이미지·이름만) upsertMap을 직접 못
    // 부른다 — refreshMapForRobot(robotId)로 서버에서 이 로봇의 최신 맵을 통째로 다시
    // 조회해(그 안에서 정확한 id/resolution/origin까지 받아) upsertMap에 반영한다.
    // 이렇게 해야 P6이 지금 이 맵을 보고 있을 때 로봇에 새 map_id로 nav_prepare가
    // 나가서 Nav2(그 안의 rviz2)도 새 지도로 다시 뜬다 — 예전엔 이미지만 바뀌고
    // 로봇은 계속 이전 맵으로 알고 있었다.
    connect(mapPage, &P5MapPage::mapRegistered, patrolPage,
            [patrolPage](const QImage&, const QString&, const QString& robotId, const QString&) {
                patrolPage->refreshMapForRobot(robotId);
            });

    // P2의 미처리 이벤트 집계를 TopBar 뱃지에 반영한다.
    connect(eventPage_, &P2EventPage::unresolvedCountsChanged, this,
            [this](int emergencyCount, int attentionCount) {
                setEmergencyCount(emergencyCount);
                setAttentionCount(attentionCount);
            });

    // P3 긴급 카드의 "이벤트 확인" → P2로 전환하고 그 대상자의 이벤트만 보이게 필터링.
    connect(recipientPage_, &P3RecipientPage::eventCheckRequested, this,
            [this](const QString& recipientName) {
                switchToPage(1);
                eventPage_->filterToRecipient(recipientName);
            });

    // P4 재생 모달의 "이벤트 상세로 이동" → P2로 전환하고 그 이벤트의 상세 모달을 바로 연다.
    connect(videoPage_, &P4VideoPage::eventDetailRequested, this,
            [this](const QString& eventUid) {
                switchToPage(1);
                eventPage_->openEventByUid(eventUid);
            });

    // P1 로봇 상세 모달의 "이벤트 상세 열기" → P2로 전환하고 검색창에 event_id를 넣어
    // 그 이벤트가 목록에 걸리게 한다(2026-09-14 변경 — 예전엔 eventUid를 무시하고
    // 페이지 전환만 했다). P4의 openEventByUid와 달리 상세 모달까지 자동으로 열지는
    // 않는다 — 로봇 상세 모달이 닫히자마자 곧바로 또 다른 모달이 뜨는 게 어색하다는
    // 리포트로, filterToEventUid로 검색까지만 하고 열람은 사용자가 직접 누르게 했다.
    connect(overviewPage, &P1OverviewPage::eventNavigationRequested, this,
            [this](const QString& eventUid) {
                switchToPage(1);
                eventPage_->filterToEventUid(eventUid);
            });

    // 반대 방향 — P2 이벤트 상세 모달의 "사유 입력 후 열람" → 페이지 전환 없이 그 자리에
    // P4의 재생 모달을 그대로 띄운다(연결된 클립을 찾아서). eventDetailRequested와 달리
    // 여기선 switchToPage를 안 부른다 — 이미 P2를 보고 있는 채로 재생 모달이 그 위에 뜬다.
    connect(eventPage_, &P2EventPage::videoViewRequested, this,
            [this](const QString& eventUid) { videoPage_->openVideoPlayerByEventUid(eventUid); });

    // role=guardian 전용 — P3가 자신의 대상자 목록을 받아오면(setUserRole()이 트리거) 같은
    // 이름 목록으로 P2 이벤트도 같이 필터링한다. admin/operator는 빈 목록이 와서 필터 없음.
    connect(recipientPage_, &P3RecipientPage::guardianRecipientNamesChanged, eventPage_,
            &P2EventPage::setRecipientNameFilter);

    // P3에서 대상자 이름·배정 로봇을 저장하면 P2 이벤트 목록의 "대상자"/"로봇" 칸이
    // 옛 값으로 남아있지 않도록 바로 다시 받아온다.
    connect(recipientPage_, &P3RecipientPage::recipientsUpdated, eventPage_,
            &P2EventPage::refreshFromServer);

    rootLayout->addLayout(body, /*stretch=*/1);
}

QWidget* MainWindow::buildTopBar() {
    auto* topBar = new QFrame(this);
    topBar->setObjectName("topBar");

    auto* layout = new QHBoxLayout(topBar);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(10);

    auto* brand = new QLabel(QStringLiteral("CareGuard"), topBar);
    brand->setObjectName("brand");
    layout->addWidget(brand);

    emergencyBadge_ = new QLabel(topBar);
    emergencyBadge_->setObjectName("badge");
    emergencyBadge_->setProperty("level", "emergencyFill");
    emergencyBadge_->setVisible(false);
    layout->addWidget(emergencyBadge_);

    attentionBadge_ = new QLabel(topBar);
    attentionBadge_->setObjectName("badge");
    attentionBadge_->setProperty("level", "attention");
    attentionBadge_->setVisible(false);
    layout->addWidget(attentionBadge_);

    layout->addStretch();

    clockLabel_ = new QLabel(topBar);
    clockLabel_->setProperty("role", "mono");
    layout->addWidget(clockLabel_);

    userLabel_ = new QLabel(topBar);
    userLabel_->setProperty("role", "secondary");
    layout->addWidget(userLabel_);

    auto* clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, [this] {
        clockLabel_->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));
    });
    clockTimer->start(1000);
    clockLabel_->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));

    return topBar;
}

void MainWindow::addNavButton(QVBoxLayout* sidebarLayout, const QIcon& icon, const QString& text,
                               int pageIndex, QButtonGroup* group) {
    auto* btn = new QPushButton(icon, text, this);
    btn->setObjectName("navBtn");
    btn->setIconSize(QSize(14, 14));
    btn->setCheckable(true);
    sidebarLayout->addWidget(btn);
    group->addButton(btn, pageIndex);
    if (pageIndex == 0) btn->setChecked(true);
}

QWidget* MainWindow::buildSidebar() {
    auto* sidebar = new QFrame(this);
    sidebar->setObjectName("sidebar");

    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 12, 0, 12);
    layout->setSpacing(2);

    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);
    connect(navGroup_, &QButtonGroup::idClicked, this,
            [this](int id) { pageStack_->setCurrentIndex(id); });

    navButtons_.clear();
    navButtons_.resize(8);
    navGroupLabels_.clear();

    auto addGroupLabel = [&](const QString& text, const QVector<int>& coveredIndexes) {
        auto* label = new QLabel(text, sidebar);
        label->setObjectName("navGroup");
        layout->addWidget(label);
        navGroupLabels_.append({label, coveredIndexes});
    };

    addGroupLabel(QStringLiteral("관제"), {0, 1, 2});
    addNavButton(layout, SimpleIcons::dashboard(Tokens::TEXT_SECONDARY, 14), QStringLiteral("통합 관제"),
                 0, navGroup_);
    addNavButton(layout, SimpleIcons::bell(Tokens::TEXT_SECONDARY, 14), QStringLiteral("이벤트"), 1,
                 navGroup_);
    addNavButton(layout, SimpleIcons::heart(Tokens::TEXT_SECONDARY, 14), QStringLiteral("대상자"), 2,
                 navGroup_);

    addGroupLabel(QStringLiteral("기록"), {3});
    addNavButton(layout, SimpleIcons::video(Tokens::TEXT_SECONDARY, 14), QStringLiteral("영상"), 3,
                 navGroup_);

    addGroupLabel(QStringLiteral("설정"), {4, 5, 6});
    addNavButton(layout, SimpleIcons::mapPin(Tokens::TEXT_SECONDARY, 14), QStringLiteral("맵·로봇"), 4,
                 navGroup_);
    addNavButton(layout, SimpleIcons::route(Tokens::TEXT_SECONDARY, 14), QStringLiteral("순찰·경로"), 5,
                 navGroup_);
    addNavButton(layout, SimpleIcons::settings(Tokens::TEXT_SECONDARY, 14), QStringLiteral("시스템"), 6,
                 navGroup_);

    addGroupLabel(QStringLiteral("로그"), {7});
    addNavButton(layout, SimpleIcons::list(Tokens::TEXT_SECONDARY, 14), QStringLiteral("통합 로그"), 7,
                 navGroup_);

    for (int i = 0; i < 8; ++i) navButtons_[i] = qobject_cast<QPushButton*>(navGroup_->button(i));

    layout->addStretch();

    auto* divider = new QFrame(sidebar);
    divider->setObjectName("sidebarDivider");
    // qss #sidebarDivider는 max-height:1px + margin:6px만 정의하고 min-height가 없어서
    // 레이아웃이 0 높이로 찌그러뜨린다. margin(6+6) + 선(1px)만큼 명시적으로 확보한다.
    divider->setFixedHeight(13);
    layout->addWidget(divider);

    auto* logoutBtn =
        new QPushButton(SimpleIcons::logout(Tokens::TEXT_SECONDARY, 14), QStringLiteral("로그아웃"), sidebar);
    logoutBtn->setObjectName("navBtn");
    logoutBtn->setIconSize(QSize(14, 14));
    layout->addWidget(logoutBtn);
    connect(logoutBtn, &QPushButton::clicked, this, &MainWindow::logoutRequested);

    auto* quitBtn =
        new QPushButton(SimpleIcons::power(Tokens::EMERGENCY.main, 14), QStringLiteral("종료"), sidebar);
    quitBtn->setObjectName("navBtnDanger");
    quitBtn->setIconSize(QSize(14, 14));
    layout->addWidget(quitBtn);
    connect(quitBtn, &QPushButton::clicked, this, [this] {
        const auto reply = QMessageBox::question(
            this, QStringLiteral("종료 확인"), QStringLiteral("CareGuard를 종료하시겠습니까?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::Yes) qApp->quit();
    });

    return sidebar;
}

void MainWindow::switchToPage(int pageIndex) {
    pageStack_->setCurrentIndex(pageIndex);
    if (auto* btn = navGroup_->button(pageIndex)) btn->setChecked(true);
}

void MainWindow::setUserName(const QString& name) {
    userLabel_->setText(name);
    videoPage_->setUserName(name);  // §7 열람 이력에 로그인 사용자 이름을 기록하기 위해 전달
    // name은 로그인 폼에 입력한 login_id 그대로다(LoginWindow::loginSucceeded 참고).
    // 계정 관리 탭의 삭제 권한(관리자만) 판단에 실제 로그인 ID가 필요해 여기서 전달한다.
    settingsPage_->setCurrentUserLoginId(name);
}

void MainWindow::setUserRole(const QString& role, int userId) {
    applyRoleVisibility(role);

    // P7: operator는 로봇관리·계정관리 탭을 숨기고 임계값·알림 탭만 조회 전용으로 연다.
    settingsPage_->setCurrentUserRole(role);

    // P3/P2: guardian만 자신이 연결된 대상자로 필터링. admin/operator 재로그인 시(같은
    // MainWindow를 재사용) 이전 guardian 필터가 남지 않도록 0으로 명시적으로 되돌린다.
    recipientPage_->setGuardianUserId(role == QStringLiteral("guardian") ? userId : 0);
}

void MainWindow::applyRoleVisibility(const QString& role) {
    QSet<int> allowed;
    if (role == QStringLiteral("guardian")) {
        allowed = {1, 2};                             // 이벤트, 대상자만
    } else if (role == QStringLiteral("operator")) {
        allowed = {0, 1, 2, 3, 5, 6, 7};               // 맵·로봇(4) 제외
    } else {
        // admin, 혹은 알 수 없는 role — 안전하게 전체 노출
        for (int i = 0; i < navButtons_.size(); ++i) allowed.insert(i);
    }

    for (int i = 0; i < navButtons_.size(); ++i) {
        if (navButtons_[i]) navButtons_[i]->setVisible(allowed.contains(i));
    }
    for (const auto& group : navGroupLabels_) {
        bool anyVisible = false;
        for (int idx : group.second) {
            if (allowed.contains(idx)) {
                anyVisible = true;
                break;
            }
        }
        if (group.first) group.first->setVisible(anyVisible);
    }

    // 지금 보던 페이지가 이번 role에서 숨겨지면(예: operator로 재로그인했는데 맵·로봇을
    // 보고 있던 상태) 허용된 첫 페이지로 옮긴다.
    if (!allowed.contains(pageStack_->currentIndex())) {
        for (int i = 0; i < navButtons_.size(); ++i) {
            if (allowed.contains(i)) {
                switchToPage(i);
                break;
            }
        }
    }
}

void MainWindow::setEmergencyCount(int count) {
    emergencyBadge_->setVisible(count > 0);
    emergencyBadge_->setText(QStringLiteral("미처리 긴급 %1").arg(count));
}

void MainWindow::setAttentionCount(int count) {
    attentionBadge_->setVisible(count > 0);
    attentionBadge_->setText(QStringLiteral("주의 %1").arg(count));
}
