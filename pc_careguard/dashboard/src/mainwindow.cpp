#include "mainwindow.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
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
    pageStack_->addWidget(new P1OverviewPage(pageStack_));   // idx 0
    pageStack_->addWidget(new P2EventPage(pageStack_));      // idx 1
    pageStack_->addWidget(new P3RecipientPage(pageStack_));  // idx 2
    videoPage_ = new P4VideoPage(pageStack_);
    pageStack_->addWidget(videoPage_);                       // idx 3
    auto* mapPage = new P5MapPage(pageStack_);
    pageStack_->addWidget(mapPage);                          // idx 4
    auto* patrolPage = new P6PatrolPage(pageStack_);
    pageStack_->addWidget(patrolPage);                       // idx 5
    pageStack_->addWidget(new P7SettingsPage(pageStack_));   // idx 6
    body->addWidget(pageStack_, /*stretch=*/1);

    // P5에서 파일로 맵을 등록하면 P6의 맵 선택지에도 같은 이름·대상자·이미지로 반영한다.
    connect(mapPage, &P5MapPage::mapRegistered, patrolPage, &P6PatrolPage::upsertMap);

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

    auto* navGroup = new QButtonGroup(this);
    navGroup->setExclusive(true);
    connect(navGroup, &QButtonGroup::idClicked, this,
            [this](int id) { pageStack_->setCurrentIndex(id); });

    auto addGroupLabel = [&](const QString& text) {
        auto* label = new QLabel(text, sidebar);
        label->setObjectName("navGroup");
        layout->addWidget(label);
    };

    addGroupLabel(QStringLiteral("관제"));
    addNavButton(layout, SimpleIcons::dashboard(Tokens::TEXT_SECONDARY, 14), QStringLiteral("통합 관제"),
                 0, navGroup);
    addNavButton(layout, SimpleIcons::bell(Tokens::TEXT_SECONDARY, 14), QStringLiteral("이벤트"), 1,
                 navGroup);
    addNavButton(layout, SimpleIcons::heart(Tokens::TEXT_SECONDARY, 14), QStringLiteral("대상자"), 2,
                 navGroup);

    addGroupLabel(QStringLiteral("기록"));
    addNavButton(layout, SimpleIcons::video(Tokens::TEXT_SECONDARY, 14), QStringLiteral("영상"), 3,
                 navGroup);

    addGroupLabel(QStringLiteral("설정"));
    addNavButton(layout, SimpleIcons::mapPin(Tokens::TEXT_SECONDARY, 14), QStringLiteral("맵·로봇"), 4,
                 navGroup);
    addNavButton(layout, SimpleIcons::route(Tokens::TEXT_SECONDARY, 14), QStringLiteral("순찰·경로"), 5,
                 navGroup);
    addNavButton(layout, SimpleIcons::settings(Tokens::TEXT_SECONDARY, 14), QStringLiteral("시스템"), 6,
                 navGroup);

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

void MainWindow::setUserName(const QString& name) {
    userLabel_->setText(name);
    videoPage_->setUserName(name);  // §7 열람 이력에 로그인 사용자 이름을 기록하기 위해 전달
}

void MainWindow::setEmergencyCount(int count) {
    emergencyBadge_->setVisible(count > 0);
    emergencyBadge_->setText(QStringLiteral("미처리 긴급 %1").arg(count));
}

void MainWindow::setAttentionCount(int count) {
    attentionBadge_->setVisible(count > 0);
    attentionBadge_->setText(QStringLiteral("주의 %1").arg(count));
}
