#include "p7_settings_page.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "dialogs/p7_account_dialog.h"
#include "dialogs/p7_robot_dialog.h"
#include "net/ws_client.h"
#include "theme/tokens.h"
#include "widgets/dropdown_fix_combo_box.h"
#include "widgets/simple_icons.h"

namespace {

QLabel* makeBadge(const char* level, const QString& text) {
    auto* badge = new QLabel(text);
    badge->setObjectName("badge");
    badge->setProperty("level", level);
    badge->setAlignment(Qt::AlignCenter);
    return badge;
}

QWidget* wrapCellWidget(QWidget* inner, const QColor& rowBg = Tokens::BG_SURFACE) {
    auto* wrap = new QWidget();
    wrap->setAutoFillBackground(true);
    QPalette pal = wrap->palette();
    pal.setColor(QPalette::Window, rowBg);
    wrap->setPalette(pal);

    auto* layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(inner);
    return wrap;
}

QFrame* card(QWidget* parent = nullptr) {
    auto* frame = new QFrame(parent);
    frame->setObjectName("card");
    return frame;
}

QLabel* sectionLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setProperty("role", "h3");
    return label;
}

// 로봇 관리 / 계정 관리 / 감사 로그 3탭에서 공통으로 쓰는 목록 헤더행(제목 + 등록/여백 버튼).
QWidget* buildListHeader(const QString& title, const QString& actionText = QString()) {
    auto* row = new QWidget();
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);

    auto* label = new QLabel(title, row);
    label->setProperty("role", "label");
    rowLayout->addWidget(label);
    rowLayout->addStretch();

    if (!actionText.isEmpty()) {
        auto* btn = new QPushButton(actionText, row);
        btn->setProperty("variant", "primary");
        rowLayout->addWidget(btn);
    }
    return row;
}

QLineEdit* thresholdField(const QString& value, int width = 44) {
    auto* edit = new QLineEdit(value);
    edit->setFixedWidth(width);
    edit->setAlignment(Qt::AlignRight);
    return edit;
}

QWidget* thresholdRow(const QString& label, QLineEdit* field, const QString& unit,
                       QLineEdit* field2 = nullptr) {
    auto* row = new QWidget();
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* labelWidget = new QLabel(label, row);
    labelWidget->setProperty("role", "secondary");
    layout->addWidget(labelWidget, /*stretch=*/1);
    layout->addWidget(field);
    if (field2 != nullptr) {
        auto* slash = new QLabel(QStringLiteral("/"), row);
        slash->setProperty("role", "muted");
        layout->addWidget(slash);
        layout->addWidget(field2);
    }
    auto* unitLabel = new QLabel(unit, row);
    unitLabel->setProperty("role", "muted");
    layout->addWidget(unitLabel);
    return row;
}

// 임계값 탭 로봇 선택 팝업 전용 — 카드 위에 "얹힌" 느낌이 나도록 은은한 그림자를 더한다
// (Tokens SHADOW_MD와 동일한 값: blur 24, dy 4, rgba(22,32,43,0.10) — CareGuard_DESIGN_
// SYSTEM.md §3 "드롭다운" 용도로 이미 정의된 토큰). 다른 콤보 박스(로봇 등록 모달 등)엔
// 영향 없게 이 파일 안에서만 쓰는 별도 서브클래스로 뒀다.
class ShadowedDropDownComboBox : public DropDownFixComboBox {
public:
    using DropDownFixComboBox::DropDownFixComboBox;
    void showPopup() override {
        QWidget* popup = view()->window();
        // WA_TranslucentBackground는 네이티브 창이 만들어지기 전에 설정해야 효과가
        // 있다 — showPopup() 호출로 창이 실제로 뜨기 전에 먼저 켠다. 그림자가 팝업
        // 테두리 바깥으로 번지려면 이게 필요하다(기본은 불투명 창이라 바깥이 잘림,
        // GNOME Shell/Mutter가 컴포지터로 떠 있어 알파 블렌딩은 지원됨).
        popup->setAttribute(Qt::WA_TranslucentBackground);
        DropDownFixComboBox::showPopup();
        auto* shadow = new QGraphicsDropShadowEffect(popup);
        shadow->setBlurRadius(24);
        shadow->setOffset(0, 4);
        shadow->setColor(QColor(22, 32, 43, 26));
        popup->setGraphicsEffect(shadow);
    }
};

}  // namespace

void P7SettingsPage::setCurrentUserLoginId(const QString& loginId) {
    currentUserLoginId_ = loginId;
    if (accountTable_ != nullptr) refreshAccountTable();
}

P7SettingsPage::P7SettingsPage(QWidget* parent) : QWidget(parent) {
    // 로봇 목록은 하드코딩하지 않고 서버 DB에서 불러온다 (fetchRobots(), 생성자 마지막에서 호출).

    // 계정 목록은 하드코딩하지 않고 DB에서 불러온다 (fetchAccounts(), 생성자 마지막에서 호출).

    // 감사 로그도 하드코딩된 목업을 걷어냈다 — 아직 실제 조회 API가 없어서 당장은
    // 빈 목록으로 시작한다. user_action_log(+ care_action의 119/112 신고) 조회 API가
    // 생기면 여기서 fetchAuditLog() 등으로 채우면 된다.

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                                Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("시스템 설정"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildRobotTab(), QStringLiteral("로봇 관리"));
    tabs->addTab(buildAccountTab(), QStringLiteral("계정 관리"));
    tabs->addTab(buildThresholdTab(), QStringLiteral("임계값 · 알림"));
    layout->addWidget(tabs);

    serverUrl_ = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("CAREGUARD_SERVER_URL"), QStringLiteral("http://127.0.0.1:8000"));
    netManager_ = new QNetworkAccessManager(this);
    fetchAccounts();
    fetchRecipients();
    fetchRobots();

    wsClient_ = new WsClient(this);
    connect(wsClient_, &WsClient::messageReceived, this, &P7SettingsPage::handleWsMessage);
    // 서버가 재시작됐다 돌아오는 경우 등, (재)연결될 때마다 목록을 다시 불러온다 —
    // 그 사이에 놓친 robot_status push가 있어도 이걸로 따라잡는다. 최초 연결 때도 한 번
    // 더 불리지만 무해하다.
    connect(wsClient_, &WsClient::connectionStateChanged, this, [this](bool connected) {
        if (connected) fetchRobots();
    });
    wsClient_->connectToServer();
}

QWidget* P7SettingsPage::buildRobotTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto* headerRow = buildListHeader(QStringLiteral("등록 로봇 %1").arg(robots_.size()),
                                       QStringLiteral("로봇 등록"));
    robotCountLabel_ = headerRow->findChild<QLabel*>();
    auto* headerBtn = headerRow->findChild<QPushButton*>();
    if (headerBtn != nullptr) {
        connect(headerBtn, &QPushButton::clicked, this, [this] { openRobotDialog(-1); });
    }
    layout->addWidget(headerRow);

    robotTable_ = new QTableWidget(tab);
    const QStringList headers = {QStringLiteral("로봇"),  QStringLiteral("IP"), QStringLiteral("도메인"),
                                  QStringLiteral("맵 · 대상자"), QStringLiteral("상태"),
                                  QStringLiteral("액션")};
    robotTable_->setColumnCount(headers.size());
    robotTable_->setHorizontalHeaderLabels(headers);
    robotTable_->verticalHeader()->setVisible(false);
    robotTable_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H_COMPACT);
    robotTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    robotTable_->setSelectionMode(QAbstractItemView::NoSelection);
    robotTable_->setFocusPolicy(Qt::NoFocus);
    robotTable_->setShowGrid(false);
    robotTable_->setWordWrap(false);

    auto* header = robotTable_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Stretch);  // 맵 · 대상자 — 남는 폭 흡수
    robotTable_->setColumnWidth(0, 110);  // 로봇
    robotTable_->setColumnWidth(1, 175);  // IP
    robotTable_->setColumnWidth(2, 160);  // 도메인
    robotTable_->setColumnWidth(4, 130);  // 상태
    robotTable_->setColumnWidth(5, 110);  // 액션

    robotTable_->setRowCount(static_cast<int>(robots_.size()));
    for (int r = 0; r < static_cast<int>(robots_.size()); ++r) {
        populateRobotRow(r);
    }
    layout->addWidget(robotTable_, /*stretch=*/1);

    auto* bottomRow = new QWidget(tab);
    auto* bottomLayout = new QHBoxLayout(bottomRow);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(Tokens::Layout::CARD_GAP);

    // 통신 단절 관리
    auto* discCard = card(bottomRow);
    auto* discLayout = new QVBoxLayout(discCard);
    discLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                    Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    discLayout->setSpacing(8);

    auto* discTitle = new QLabel(QStringLiteral("통신 단절 관리"), discCard);
    discTitle->setProperty("role", "label");
    discLayout->addWidget(discTitle);

    auto* banner = new QFrame(discCard);
    banner->setObjectName("banner");
    banner->setProperty("level", Tokens::Prop::ATTENTION);
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(9, 7, 9, 7);
    bannerLayout->setSpacing(8);

    auto* bannerIcon = new QLabel(banner);
    bannerIcon->setPixmap(SimpleIcons::plugOff(Tokens::ATTENTION.main, 14).pixmap(14, 14));
    bannerLayout->addWidget(bannerIcon);

    auto* bannerTextCol = new QWidget(banner);
    auto* bannerTextLayout = new QVBoxLayout(bannerTextCol);
    bannerTextLayout->setContentsMargins(0, 0, 0, 0);
    bannerTextLayout->setSpacing(2);
    auto* bannerLine1 = new QLabel(QStringLiteral("R-04 · 마지막 동기화 13:58"), bannerTextCol);
    QFont line1Font = bannerLine1->font();
    line1Font.setWeight(QFont::DemiBold);
    bannerLine1->setFont(line1Font);
    bannerTextLayout->addWidget(bannerLine1);
    auto* bannerLine2 = new QLabel(QStringLiteral("로컬 큐 이벤트 7건 대기 · 30초 주기 재시도 중"), bannerTextCol);
    bannerLine2->setProperty("role", "secondary");
    bannerTextLayout->addWidget(bannerLine2);
    bannerLayout->addWidget(bannerTextCol, /*stretch=*/1);

    discLayout->addWidget(banner);

    auto* syncBtn = new QPushButton(SimpleIcons::refresh(Tokens::TEXT_PRIMARY, 12),
                                     QStringLiteral("재연결 후 일괄 동기화"), discCard);
    syncBtn->setProperty("variant", "secondary");
    syncBtn->setIconSize(QSize(12, 12));
    discLayout->addWidget(syncBtn);
    discLayout->addStretch();

    bottomLayout->addWidget(discCard, /*stretch=*/1);

    layout->addWidget(bottomRow, /*stretch=*/1);
    return tab;
}

QWidget* P7SettingsPage::buildAccountTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    const int activeCount = static_cast<int>(
        std::count_if(accounts_.begin(), accounts_.end(), [](const AccountRow& a) { return a.active; }));
    auto* headerRow = buildListHeader(
        QStringLiteral("계정 %1 · 활성 %2").arg(accounts_.size()).arg(activeCount),
        QStringLiteral("계정 등록"));
    accountCountLabel_ = headerRow->findChild<QLabel*>();
    accountRegisterBtn_ = headerRow->findChild<QPushButton*>();
    if (accountRegisterBtn_ != nullptr) {
        // 등록도 관리자 권한 계정만 가능 — 삭제와 동일하게 계정 목록을 불러온 뒤
        // refreshAccountTable()에서 isCurrentUserAdmin()으로 판단해 갱신한다. 그 전까지는
        // 기본으로 막아둔다.
        accountRegisterBtn_->setEnabled(false);
        connect(accountRegisterBtn_, &QPushButton::clicked, this, [this] { openAccountDialog(-1); });
    }
    // 삭제는 관리자 권한 계정만 가능 — 실제 표시 여부는 계정 목록을 불러온 뒤
    // refreshAccountTable()에서 isCurrentUserAdmin()으로 판단해 갱신한다.
    deleteAccountBtn_ = new QPushButton(QStringLiteral("삭제"), headerRow);
    deleteAccountBtn_->setProperty("variant", "secondary");
    deleteAccountBtn_->setVisible(false);
    connect(deleteAccountBtn_, &QPushButton::clicked, this, &P7SettingsPage::handleDeleteAccountsClicked);
    headerRow->layout()->addWidget(deleteAccountBtn_);
    layout->addWidget(headerRow);

    accountTable_ = new QTableWidget(tab);
    const QStringList headers = {QStringLiteral("ID"),   QStringLiteral("이름"),
                                  QStringLiteral("역할"), QStringLiteral("담당 (대상자·로봇)"),
                                  QStringLiteral("최종 로그인"), QStringLiteral("상태"),
                                  QStringLiteral("액션"), QStringLiteral("선택")};
    accountTable_->setColumnCount(headers.size());
    accountTable_->setHorizontalHeaderLabels(headers);
    accountTable_->verticalHeader()->setVisible(false);
    accountTable_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    accountTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    accountTable_->setSelectionMode(QAbstractItemView::NoSelection);
    accountTable_->setFocusPolicy(Qt::NoFocus);
    accountTable_->setShowGrid(false);

    auto* header = accountTable_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    accountTable_->setColumnWidth(0, 110);  // ID
    accountTable_->setColumnWidth(1, 100);  // 이름
    accountTable_->setColumnWidth(2, 100);  // 역할
    accountTable_->setColumnWidth(4, 130);  // 최종 로그인
    accountTable_->setColumnWidth(5, 90);   // 상태
    accountTable_->setColumnWidth(6, 150);  // 액션
    accountTable_->setColumnWidth(7, 60);   // 선택(삭제용 체크박스)
    accountTable_->setColumnHidden(7, true);  // 관리자가 아니면 refreshAccountTable()에서도 숨김 유지

    accountTable_->setRowCount(static_cast<int>(accounts_.size()));
    for (int r = 0; r < static_cast<int>(accounts_.size()); ++r) {
        populateAccountRow(r);
    }
    layout->addWidget(accountTable_, /*stretch=*/1);

    return tab;
}

QWidget* P7SettingsPage::buildThresholdTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    // 로봇 선택 — 임계값은 로봇별로 저장되므로 어떤 로봇을 보고/고칠지 먼저 고른다.
    auto* robotSelectColumn = new QWidget(tab);
    auto* robotSelectLayout = new QVBoxLayout(robotSelectColumn);
    robotSelectLayout->setContentsMargins(0, 0, 0, 0);
    robotSelectLayout->setSpacing(4);
    robotSelectLayout->addWidget(sectionLabel(QStringLiteral("로봇 선택")));
    thresholdRobotCombo_ = new ShadowedDropDownComboBox(robotSelectColumn);
    thresholdRobotCombo_->setObjectName(QStringLiteral("thresholdRobotCombo"));
    thresholdRobotCombo_->setMinimumWidth(220);
    connect(thresholdRobotCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0) return;
                fetchRobotThresholds(thresholdRobotCombo_->itemData(index).toInt());
            });
    robotSelectLayout->addWidget(thresholdRobotCombo_);
    layout->addWidget(robotSelectColumn);

    auto* cardsRow = new QWidget(tab);
    auto* cardsLayout = new QHBoxLayout(cardsRow);
    cardsLayout->setContentsMargins(0, 0, 0, 0);
    cardsLayout->setSpacing(Tokens::Layout::CARD_GAP);

    // 로봇 공통
    auto* robotCard = card(cardsRow);
    auto* robotLayout = new QVBoxLayout(robotCard);
    robotLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                     Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    robotLayout->setSpacing(8);
    robotLayout->addWidget(sectionLabel(QStringLiteral("로봇 공통")));
    batteryAttentionEdit_ = thresholdField("30");
    batteryReturnEdit_ = thresholdField("28");
    robotLayout->addWidget(thresholdRow(QStringLiteral("배터리 주의 / 복귀"), batteryAttentionEdit_,
                                         QStringLiteral("%"), batteryReturnEdit_));
    voiceWaitEdit_ = thresholdField("8");
    robotLayout->addWidget(
        thresholdRow(QStringLiteral("음성 응답 대기"), voiceWaitEdit_, QStringLiteral("초")));
    reconnectRetryEdit_ = thresholdField("30");
    robotLayout->addWidget(
        thresholdRow(QStringLiteral("재연결 재시도 주기"), reconnectRetryEdit_, QStringLiteral("초")));
    cardsLayout->addWidget(robotCard, 1);

    // 복약 공통
    auto* medCard = card(cardsRow);
    auto* medLayout = new QVBoxLayout(medCard);
    medLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                                   Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    medLayout->setSpacing(8);
    medLayout->addWidget(sectionLabel(QStringLiteral("복약 공통")));
    medMissedWaitEdit_ = thresholdField("10");
    medLayout->addWidget(
        thresholdRow(QStringLiteral("미수령 판정 시간"), medMissedWaitEdit_, QStringLiteral("분")));
    medReannounceEdit_ = thresholdField("2");
    medLayout->addWidget(
        thresholdRow(QStringLiteral("재안내 횟수"), medReannounceEdit_, QStringLiteral("회")));
    cardsLayout->addWidget(medCard, 1);

    layout->addWidget(cardsRow);

    auto* buttonRow = new QWidget(tab);
    auto* buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(10);
    buttonLayout->addStretch();

    auto* saveBtn = new QPushButton(QStringLiteral("이 로봇에 저장"), buttonRow);
    saveBtn->setProperty("variant", "primary");
    connect(saveBtn, &QPushButton::clicked, this, &P7SettingsPage::saveThresholdsForCurrentRobot);
    buttonLayout->addWidget(saveBtn);

    auto* applyAllBtn = new QPushButton(QStringLiteral("전체 로봇에 적용"), buttonRow);
    applyAllBtn->setProperty("variant", "secondary");
    connect(applyAllBtn, &QPushButton::clicked, this, &P7SettingsPage::applyThresholdsToAllRobots);
    buttonLayout->addWidget(applyAllBtn);

    layout->addWidget(buttonRow);
    layout->addStretch();

    refreshThresholdRobotCombo();

    return tab;
}

void P7SettingsPage::populateRobotRow(int r) {
    const RobotRow& row = robots_[static_cast<size_t>(r)];
    const bool offline = row.level == Tokens::Level::Offline;
    const QColor rowBg = offline ? Tokens::BG_SUNKEN : Tokens::BG_SURFACE;
    const QColor textColor = offline ? Tokens::TEXT_MUTED : Tokens::TEXT_PRIMARY;

    auto* idItem = new QTableWidgetItem(row.id);
    idItem->setFont(QFont(Tokens::Font::MONO));
    idItem->setTextAlignment(Qt::AlignCenter);
    idItem->setBackground(rowBg);
    idItem->setForeground(textColor);
    robotTable_->setItem(r, 0, idItem);

    auto* ipItem = new QTableWidgetItem(row.ip);
    ipItem->setFont(QFont(Tokens::Font::MONO));
    ipItem->setTextAlignment(Qt::AlignCenter);
    ipItem->setBackground(rowBg);
    ipItem->setForeground(offline ? Tokens::TEXT_MUTED : Tokens::TEXT_SECONDARY);
    robotTable_->setItem(r, 1, ipItem);

    auto* nsItem = new QTableWidgetItem(row.ns);
    nsItem->setFont(QFont(Tokens::Font::MONO));
    nsItem->setTextAlignment(Qt::AlignCenter);
    nsItem->setBackground(rowBg);
    nsItem->setForeground(offline ? Tokens::TEXT_MUTED : Tokens::TEXT_SECONDARY);
    robotTable_->setItem(r, 2, nsItem);

    auto* mapItem = new QTableWidgetItem(row.mapRecipient);
    mapItem->setTextAlignment(Qt::AlignCenter);
    mapItem->setBackground(rowBg);
    mapItem->setForeground(textColor);
    robotTable_->setItem(r, 3, mapItem);

    QColor statusColor = Tokens::OK.main;
    if (row.level == Tokens::Level::Offline) statusColor = Tokens::TEXT_MUTED;
    auto* statusItem = new QTableWidgetItem(row.statusDetail);
    statusItem->setTextAlignment(Qt::AlignCenter);
    statusItem->setBackground(rowBg);
    statusItem->setForeground(statusColor);
    robotTable_->setItem(r, 4, statusItem);

    // 오프라인(방금 등록해서 아직 안 붙었거나 연결이 끊긴) 로봇도 수정·등록 해제
    // 다이얼로그를 열 수 있어야 한다 — 예전엔 이 칸이 클릭 안 되는 "재연결" 라벨만
    // 보여줘서 오프라인 로봇은 등록 해제할 방법이 아예 없었다.
    auto* editBtn = new QPushButton(offline ? QStringLiteral("수정 · 삭제") : QStringLiteral("수정 · 재시작"));
    editBtn->setObjectName("linkBtn");
    editBtn->setCursor(Qt::PointingHandCursor);
    connect(editBtn, &QPushButton::clicked, this, [this, r] { openRobotDialog(r); });
    robotTable_->setCellWidget(r, 5, wrapCellWidget(editBtn, rowBg));
}

void P7SettingsPage::refreshRobotTable() {
    if (robotCountLabel_ != nullptr) {
        robotCountLabel_->setText(QStringLiteral("등록 로봇 %1").arg(robots_.size()));
    }
    robotTable_->setRowCount(static_cast<int>(robots_.size()));
    for (int r = 0; r < static_cast<int>(robots_.size()); ++r) {
        populateRobotRow(r);
    }
}

void P7SettingsPage::fetchRobots() {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/robots")));
    QNetworkReply* reply = netManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleRobotsReply(reply);
        reply->deleteLater();
    });
}

void P7SettingsPage::handleRobotsReply(QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        robots_.clear();
        if (robotTable_ != nullptr) refreshRobotTable();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    std::vector<RobotRow> rows;
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject obj = value.toObject();
        RobotRow row;
        row.dbId = obj.value("id").toInt(-1);
        row.id = obj.value("code").toString();
        row.name = obj.value("name").toString();
        row.model = obj.value("model").toString();
        row.rosDomainId = obj.value("ros_domain_id").toInt(0);
        row.ip = obj.value("ip").toString(QStringLiteral("-"));
        row.ns = row.rosDomainId > 0 ? QString::number(row.rosDomainId) : QStringLiteral("-");
        row.mapRecipient = obj.value("map_recipient").toString(QStringLiteral("-"));
        const bool online = obj.value("is_online").toBool();
        row.level = online ? Tokens::Level::Normal : Tokens::Level::Offline;
        row.statusDetail = online ? QStringLiteral("● 온라인") : QStringLiteral("오프라인");
        rows.push_back(row);
    }
    robots_ = rows;
    if (robotTable_ != nullptr) refreshRobotTable();
    if (thresholdRobotCombo_ != nullptr) refreshThresholdRobotCombo();
}

void P7SettingsPage::registerRobot(const RobotDialog::RobotData& data) {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/robots")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{
        {"code", data.code},
        {"name", data.name},
        {"model", data.model},
        {"ros_domain_id", data.rosDomainId},
        {"ip", data.ip.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(data.ip)},
    };
    const int recipientId = data.recipientId;
    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, recipientId] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200 && statusCode != 201) {
            const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
            const QString detail = err.value("detail").toString();
            reply->deleteLater();
            QMessageBox::warning(this, QStringLiteral("로봇 등록 실패"),
                                  detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
            return;
        }
        const int newRobotId = QJsonDocument::fromJson(reply->readAll()).object().value("id").toInt(-1);
        reply->deleteLater();
        if (recipientId > 0 && newRobotId > 0) {
            // 등록 자체는 성공했으니, 담당 대상자 배정은 별도 API로 이어서 저장한다
            // (실패해도 로봇 등록은 이미 끝난 상태라 목록은 새로고침한다).
            assignRobotRecipient(newRobotId, recipientId);
            return;
        }
        fetchRobots();
    });
}

void P7SettingsPage::assignRobotRecipient(int robotDbId, int recipientId) {
    QNetworkRequest request(QUrl(QStringLiteral("%1/robots/%2/recipient").arg(serverUrl_).arg(robotDbId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject body{{"recipient_id", recipientId}};
    QNetworkReply* reply = netManager_->put(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 204) {
            const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
            const QString detail = err.value("detail").toString();
            QMessageBox::warning(this, QStringLiteral("담당 대상자 배정 실패"),
                                  detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
        }
        reply->deleteLater();
        fetchRobots();
    });
}

void P7SettingsPage::deregisterRobot(int dbId) {
    QNetworkRequest request(QUrl(QStringLiteral("%1/robots/%2").arg(serverUrl_).arg(dbId)));
    QNetworkReply* reply = netManager_->deleteResource(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 204) {
            const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
            const QString detail = err.value("detail").toString();
            reply->deleteLater();
            QMessageBox::warning(this, QStringLiteral("로봇 등록 해제 실패"),
                                  detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
            return;
        }
        reply->deleteLater();
        fetchRobots();
    });
}

void P7SettingsPage::handleWsMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("robot_recipient_changed") || type == QStringLiteral("robot_deregistered") ||
        type == QStringLiteral("map_changed")) {
        // 담당 대상자 재배정·로봇 등록 해제·새 맵 등록 — 로봇 관리 탭의 "맵/대상자"
        // 열이 이 값들을 보여주므로(robots.py::LIST_QUERY), GET /robots를 다시 받는다.
        fetchRobots();
        return;
    }

    if (type != QStringLiteral("robot_status")) return;

    const QString code = message.value(QStringLiteral("robot_code")).toString();
    const bool online = message.value(QStringLiteral("online")).toBool();
    for (int r = 0; r < static_cast<int>(robots_.size()); ++r) {
        if (robots_[static_cast<size_t>(r)].id == code) {
            robots_[static_cast<size_t>(r)].level = online ? Tokens::Level::Normal : Tokens::Level::Offline;
            robots_[static_cast<size_t>(r)].statusDetail =
                online ? QStringLiteral("● 온라인") : QStringLiteral("오프라인");
            if (robotTable_ != nullptr) populateRobotRow(r);
            break;
        }
    }
}

void P7SettingsPage::openRobotDialog(int rowIndex) {
    const bool isNew = (rowIndex < 0);
    RobotDialog::RobotData data;
    if (isNew) {
        int maxNum = 0;
        for (const auto& r : robots_) {
            const int num = r.id.mid(2).toInt();
            maxNum = std::max(maxNum, num);
        }
        data.code = QStringLiteral("R-%1").arg(maxNum + 1, 2, 10, QLatin1Char('0'));
    } else {
        const RobotRow& row = robots_[static_cast<size_t>(rowIndex)];
        data.code = row.id;
        data.name = row.name;
        data.model = row.model;
        data.rosDomainId = row.rosDomainId;
        data.ip = row.ip == QStringLiteral("-") ? QString() : row.ip;
        data.recipientLabel = row.recipientLabel;
        data.connected = (row.level != Tokens::Level::Offline);
        data.wsHost = row.wsHost;
        data.connectionDetail = row.connectionDetail;
    }

    const QString previousRecipient = isNew ? QString() : robots_[static_cast<size_t>(rowIndex)].recipientLabel;

    QVector<RobotDialog::RecipientOption> recipientOptions;
    for (const RecipientRow& rc : recipients_) {
        RobotDialog::RecipientOption option;
        option.id = rc.id;
        option.label = rc.label;
        recipientOptions.push_back(option);
    }

    auto* dialog = new RobotDialog(isNew ? RobotDialog::Mode::Register : RobotDialog::Mode::Edit, data,
                                    recipientOptions, this);
    connect(dialog, &RobotDialog::robotSaved, this,
            [this, rowIndex, isNew, previousRecipient](const RobotDialog::RobotData& saved) {
        if (isNew) {
            // 신규 등록은 서버(DB)에 저장하고, 응답 후 목록을 다시 불러와 반영한다.
            registerRobot(saved);
            return;
        }
        // 기존 로봇 수정은 이번 범위 밖 — 지금까지처럼 로컬 표시만 갱신한다.
        RobotRow row = robots_[static_cast<size_t>(rowIndex)];
        row.name = saved.name;
        row.model = saved.model;
        row.rosDomainId = saved.rosDomainId;
        row.ip = saved.ip;
        row.recipientLabel = saved.recipientLabel;
        if (saved.recipientLabel != previousRecipient) {
            row.mapRecipient = saved.recipientLabel;
        }
        robots_[static_cast<size_t>(rowIndex)] = row;
        refreshRobotTable();
    });
    connect(dialog, &RobotDialog::robotDeregisterRequested, this, [this, rowIndex](const QString&) {
        if (rowIndex >= 0) {
            // 서버(DB)에서도 실제로 등록 해제하고, 응답 후 목록을 다시 불러와 반영한다.
            deregisterRobot(robots_[static_cast<size_t>(rowIndex)].dbId);
        }
    });
    dialog->exec();
    dialog->deleteLater();
}

namespace {
AccountDialog::Role koreanRoleToEnum(const QString& role) {
    if (role == QStringLiteral("관리자")) return AccountDialog::Role::Admin;
    if (role == QStringLiteral("보호자")) return AccountDialog::Role::Guardian;
    return AccountDialog::Role::Operator;
}

QString roleEnumToCode(AccountDialog::Role role) {
    switch (role) {
        case AccountDialog::Role::Admin: return QStringLiteral("admin");
        case AccountDialog::Role::Guardian: return QStringLiteral("guardian");
        case AccountDialog::Role::Operator: return QStringLiteral("operator");
    }
    return QString();
}

// role.name(예: "관제 담당자")이 아니라 화면 표에서 그대로 쓰는 짧은 표기로 맞춘다.
QString roleCodeToKorean(const QString& code) {
    if (code == QStringLiteral("admin")) return QStringLiteral("관리자");
    if (code == QStringLiteral("guardian")) return QStringLiteral("보호자");
    return QStringLiteral("관제");
}

QString formatServerTimestamp(const QString& iso) {
    if (iso.isEmpty()) return QString();
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!dt.isValid()) return QString();
    return dt.toString(QStringLiteral("MM.dd HH:mm"));
}
}  // namespace

bool P7SettingsPage::isLastActiveAdmin(int rowIndex) const {
    const AccountRow& target = accounts_[static_cast<size_t>(rowIndex)];
    if (target.role != QStringLiteral("관리자") || !target.active) return false;
    int activeAdmins = 0;
    for (const auto& acc : accounts_) {
        if (acc.role == QStringLiteral("관리자") && acc.active) ++activeAdmins;
    }
    return activeAdmins <= 1;
}

void P7SettingsPage::populateAccountRow(int r) {
    const AccountRow& acc = accounts_[static_cast<size_t>(r)];
    const QColor rowBg = acc.active ? Tokens::BG_SURFACE : Tokens::BG_SUNKEN;
    const QColor textColor = acc.active ? Tokens::TEXT_PRIMARY : Tokens::TEXT_MUTED;

    auto* idItem = new QTableWidgetItem(acc.id);
    idItem->setFont(QFont(Tokens::Font::MONO));
    idItem->setTextAlignment(Qt::AlignCenter);
    idItem->setBackground(rowBg);
    idItem->setForeground(textColor);
    accountTable_->setItem(r, 0, idItem);

    auto* nameItem = new QTableWidgetItem(acc.name);
    nameItem->setTextAlignment(Qt::AlignCenter);
    nameItem->setBackground(rowBg);
    nameItem->setForeground(textColor);
    accountTable_->setItem(r, 1, nameItem);

    auto* roleItem = new QTableWidgetItem(acc.role);
    roleItem->setTextAlignment(Qt::AlignCenter);
    roleItem->setBackground(rowBg);
    roleItem->setForeground(acc.role == QStringLiteral("관리자") ? Tokens::ACCENT_HOVER : textColor);
    QFont roleFont = roleItem->font();
    roleFont.setWeight(acc.role == QStringLiteral("관리자") ? QFont::DemiBold : QFont::Normal);
    roleItem->setFont(roleFont);
    accountTable_->setItem(r, 2, roleItem);

    auto* chargeItem = new QTableWidgetItem(acc.charge);
    chargeItem->setTextAlignment(Qt::AlignCenter);
    chargeItem->setBackground(rowBg);
    chargeItem->setForeground(textColor);
    accountTable_->setItem(r, 3, chargeItem);

    auto* loginItem = new QTableWidgetItem(acc.lastLogin);
    loginItem->setFont(QFont(Tokens::Font::MONO));
    loginItem->setTextAlignment(Qt::AlignCenter);
    loginItem->setBackground(rowBg);
    loginItem->setForeground(acc.active ? Tokens::TEXT_SECONDARY : Tokens::TEXT_MUTED);
    accountTable_->setItem(r, 4, loginItem);

    accountTable_->setCellWidget(
        r, 5,
        wrapCellWidget(makeBadge(acc.active ? Tokens::Prop::OK : Tokens::Prop::OFFLINE,
                                  acc.active ? QStringLiteral("활성") : QStringLiteral("비활성")),
                        rowBg));

    if (acc.active) {
        const bool canEdit = isCurrentUserAdmin();
        auto* editBtn = new QPushButton(QStringLiteral("수정 · 비밀번호 변경"));
        editBtn->setObjectName("linkBtn");
        editBtn->setEnabled(canEdit);
        editBtn->setCursor(canEdit ? Qt::PointingHandCursor : Qt::ArrowCursor);
        connect(editBtn, &QPushButton::clicked, this, [this, r] { openAccountDialog(r); });
        accountTable_->setCellWidget(r, 6, wrapCellWidget(editBtn, rowBg));
    } else {
        auto* activateBtn = new QPushButton(QStringLiteral("활성화"));
        activateBtn->setObjectName("linkBtn");
        activateBtn->setCursor(Qt::PointingHandCursor);
        connect(activateBtn, &QPushButton::clicked, this, [this, r] {
            const AccountRow& target = accounts_[static_cast<size_t>(r)];
            QMessageBox box(this);
            box.setWindowTitle(QStringLiteral("계정 활성화"));
            box.setText(QStringLiteral("%1(%2) 계정을 다시 활성화하시겠습니까?").arg(target.name, target.id));
            box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
            box.setDefaultButton(QMessageBox::Cancel);
            box.button(QMessageBox::Ok)->setText(QStringLiteral("확인"));
            box.button(QMessageBox::Cancel)->setText(QStringLiteral("취소"));
            if (box.exec() == QMessageBox::Ok) activateAccount(r);
        });
        accountTable_->setCellWidget(r, 6, wrapCellWidget(activateBtn, rowBg));
    }

    // 삭제용 체크박스 — 관리자 권한이 아니면 컬럼 자체가 숨겨져 있어 보이지 않는다.
    auto* selectCheck = new QCheckBox();
    accountTable_->setCellWidget(r, 7, wrapCellWidget(selectCheck, rowBg));
}

void P7SettingsPage::refreshAccountTable() {
    if (accountCountLabel_ != nullptr) {
        const int activeCount = static_cast<int>(
            std::count_if(accounts_.begin(), accounts_.end(), [](const AccountRow& a) { return a.active; }));
        accountCountLabel_->setText(QStringLiteral("계정 %1 · 활성 %2").arg(accounts_.size()).arg(activeCount));
    }
    accountTable_->setRowCount(static_cast<int>(accounts_.size()));
    for (int r = 0; r < static_cast<int>(accounts_.size()); ++r) {
        populateAccountRow(r);
    }

    // 계정 등록·삭제는 관리자 권한 계정만 — 로그인 세션 연동 전이라 currentUserLoginId_로
    // 본인 계정을 찾아 그 역할로 판단한다.
    const bool isAdmin = isCurrentUserAdmin();
    if (accountRegisterBtn_ != nullptr) accountRegisterBtn_->setEnabled(isAdmin);
    accountTable_->setColumnHidden(7, !isAdmin);
    if (deleteAccountBtn_ != nullptr) deleteAccountBtn_->setVisible(isAdmin);
}

void P7SettingsPage::fetchAccounts() {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/accounts")));
    QNetworkReply* reply = netManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleAccountsReply(reply);
        reply->deleteLater();
    });
}

void P7SettingsPage::handleAccountsReply(QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        // 서버 연결 실패 시 목록을 비운 채로 둔다 — P4 영상 목록과 동일한 방침.
        accounts_.clear();
        if (accountTable_ != nullptr) refreshAccountTable();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    std::vector<AccountRow> rows;
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject obj = value.toObject();
        AccountRow row;
        row.dbId = obj.value("id").toInt();
        row.id = obj.value("login_id").toString();
        row.name = obj.value("display_name").toString();
        row.role = roleCodeToKorean(obj.value("role_code").toString());
        const QString chargeSummary = obj.value("charge_summary").toString();
        row.charge = chargeSummary.isEmpty() ? QStringLiteral("미배정") : chargeSummary;
        row.lastLogin = formatServerTimestamp(obj.value("last_login_at").toString());
        row.active = obj.value("is_active").toBool();
        rows.push_back(row);
    }
    accounts_ = rows;
    if (accountTable_ != nullptr) refreshAccountTable();
}

void P7SettingsPage::registerAccount(const AccountDialog::AccountData& data) {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/accounts")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{
        {"login_id", data.loginId},
        {"password", data.password},
        {"display_name", data.displayName},
        {"role_code", roleEnumToCode(data.role)},
        {"is_active", data.active},
    };
    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, assignedRecipientIds = data.assignedRecipientIds] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200 && statusCode != 201) {
            const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
            const QString detail = err.value("detail").toString();
            reply->deleteLater();
            QMessageBox::warning(this, QStringLiteral("계정 등록 실패"),
                                  detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
            return;
        }
        const QJsonObject created = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (assignedRecipientIds.isEmpty()) {
            fetchAccounts();  // 등록 결과를 DB 기준으로 다시 불러와 반영한다.
        } else {
            // 등록과 동시에 담당 대상자를 골랐으면 이어서 배정한다. assignRecipients()가
            // 성공 시 fetchAccounts()/fetchRecipients()까지 처리한다.
            assignRecipients(created.value("id").toInt(), assignedRecipientIds);
        }
    });
}

void P7SettingsPage::updateAccount(int rowIndex, const AccountDialog::AccountData& data) {
    const int dbId = accounts_[static_cast<size_t>(rowIndex)].dbId;
    QNetworkRequest request(QUrl(QStringLiteral("%1/accounts/%2").arg(serverUrl_).arg(dbId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{
        {"display_name", data.displayName},
        {"role_code", roleEnumToCode(data.role)},
        {"is_active", data.active},
    };
    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PATCH", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, dbId, isActive = data.active, assignedRecipientIds = data.assignedRecipientIds] {
                const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (statusCode != 200) {
                    const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
                    const QString detail = err.value("detail").toString();
                    reply->deleteLater();
                    QMessageBox::warning(this, QStringLiteral("계정 수정 실패"),
                                          detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
                    return;
                }
                reply->deleteLater();
                if (!isActive) {
                    // 비활성화하면 서버가 이 계정의 담당 배정을 전부 해제한다(update_account 참고).
                    // 여기서 다시 PUT을 보내면 방금 해제된 배정을 비활성 계정에 되살리게 되므로 생략한다.
                    fetchAccounts();
                    fetchRecipients();
                    return;
                }
                // assignRecipients()가 성공 시 fetchAccounts()까지 다시 불러와 반영한다.
                assignRecipients(dbId, assignedRecipientIds);
            });
}

void P7SettingsPage::changePassword(int userId, const QString& newPassword) {
    QNetworkRequest request(QUrl(QStringLiteral("%1/accounts/%2/password").arg(serverUrl_).arg(userId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{{"new_password", newPassword}};
    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PATCH", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (statusCode != 204) {
            QMessageBox::warning(this, QStringLiteral("비밀번호 변경 실패"),
                                  QStringLiteral("서버에 연결할 수 없습니다"));
            return;
        }
        QMessageBox::information(this, QStringLiteral("비밀번호 변경"), QStringLiteral("비밀번호가 변경되었습니다."));
    });
}

void P7SettingsPage::activateAccount(int rowIndex) {
    const AccountRow& acc = accounts_[static_cast<size_t>(rowIndex)];
    QNetworkRequest request(QUrl(QStringLiteral("%1/accounts/%2").arg(serverUrl_).arg(acc.dbId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QJsonObject body{
        {"display_name", acc.name},
        {"role_code", roleEnumToCode(koreanRoleToEnum(acc.role))},
        {"is_active", true},
    };
    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PATCH", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) {
            const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object();
            const QString detail = err.value("detail").toString();
            reply->deleteLater();
            QMessageBox::warning(this, QStringLiteral("계정 활성화 실패"),
                                  detail.isEmpty() ? QStringLiteral("서버에 연결할 수 없습니다") : detail);
            return;
        }
        reply->deleteLater();
        fetchAccounts();
    });
}

bool P7SettingsPage::isCurrentUserAdmin() const {
    for (const auto& acc : accounts_) {
        if (acc.id == currentUserLoginId_) return acc.role == QStringLiteral("관리자");
    }
    return false;
}

void P7SettingsPage::handleDeleteAccountsClicked() {
    QVector<int> ids;
    QStringList labels;
    for (int r = 0; r < accountTable_->rowCount(); ++r) {
        auto* wrap = accountTable_->cellWidget(r, 7);
        auto* checkbox = (wrap != nullptr) ? wrap->findChild<QCheckBox*>() : nullptr;
        if (checkbox != nullptr && checkbox->isChecked()) {
            const AccountRow& acc = accounts_[static_cast<size_t>(r)];
            ids.append(acc.dbId);
            labels.append(QStringLiteral("%1(%2)").arg(acc.name, acc.id));
        }
    }
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("계정 삭제"), QStringLiteral("삭제할 계정을 먼저 선택하세요"));
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("계정 삭제"));
    box.setText(QStringLiteral("%1 계정을 삭제하시겠습니까?\n이 작업은 되돌릴 수 없습니다.").arg(labels.join(QStringLiteral(", "))));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("확인"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("취소"));
    if (box.exec() != QMessageBox::Ok) return;

    deleteAccounts(ids);
}

void P7SettingsPage::deleteAccounts(const QVector<int>& userIds) {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/accounts/delete")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonArray idsArray;
    for (int id : userIds) idsArray.append(id);
    const QJsonObject body{{"user_ids", idsArray}};

    QNetworkReply* reply = netManager_->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (statusCode != 204) {
            QMessageBox::warning(this, QStringLiteral("계정 삭제 실패"), QStringLiteral("서버에 연결할 수 없습니다"));
            return;
        }
        fetchAccounts();
    });
}

void P7SettingsPage::fetchRecipients() {
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/accounts/recipients")));
    QNetworkReply* reply = netManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleRecipientsReply(reply);
        reply->deleteLater();
    });
}

void P7SettingsPage::handleRecipientsReply(QNetworkReply* reply) {
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        recipients_.clear();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    std::vector<RecipientRow> rows;
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject obj = value.toObject();
        RecipientRow row;
        row.id = obj.value("id").toInt();
        row.code = obj.value("code").toString();
        row.name = obj.value("name").toString();
        const QString robotCode = obj.value("robot_code").toString();
        row.label = robotCode.isEmpty()
                        ? QStringLiteral("%1 (%2)").arg(obj.value("name").toString(), obj.value("code").toString())
                        : QStringLiteral("%1 (%2) · %3")
                              .arg(obj.value("name").toString(), obj.value("code").toString(), robotCode);
        row.operatorId = obj.value("operator_id").isNull() ? -1 : obj.value("operator_id").toInt();
        rows.push_back(row);
    }
    recipients_ = rows;
}

void P7SettingsPage::assignRecipients(int userId, const QVector<int>& recipientIds) {
    QNetworkRequest request(QUrl(QStringLiteral("%1/accounts/%2/recipients").arg(serverUrl_).arg(userId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonArray idsArray;
    for (int id : recipientIds) idsArray.append(id);
    const QJsonObject body{{"recipient_ids", idsArray}};

    QNetworkReply* reply = netManager_->sendCustomRequest(request, "PUT", QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (statusCode != 204) {
            QMessageBox::warning(this, QStringLiteral("담당 배정 실패"),
                                  QStringLiteral("계정 정보는 저장됐지만 담당 대상자 배정에는 실패했습니다"));
            return;
        }
        fetchAccounts();
        fetchRecipients();
    });
}

void P7SettingsPage::openAccountDialog(int rowIndex) {
    const bool isNew = (rowIndex < 0);
    AccountDialog::AccountData data;
    QStringList existingIds;
    for (const auto& acc : accounts_) existingIds << acc.id;

    // 담당 계정 id로 표시 이름을 찾는다 — "현재 담당" 표기·재배정 확인 메시지 조립용.
    auto operatorName = [this](int operatorId) -> QString {
        for (const auto& acc : accounts_) {
            if (acc.dbId == operatorId) return acc.name;
        }
        return QString();
    };

    // 신규 등록은 아직 dbId가 없으니 전부 미체크로 보여준다 — 등록과 동시에 담당을
    // 고를 수 있게 하되(공백으로 두면 미배정), 기존 배정 여부는 수정 모드에서만 따진다.
    QVector<AccountDialog::RecipientOption> recipientOptions;
    for (const RecipientRow& rc : recipients_) {
        AccountDialog::RecipientOption option;
        option.id = rc.id;
        option.code = rc.code;
        option.name = rc.name;
        option.label = rc.label;
        option.assigned = !isNew && rc.operatorId == accounts_[static_cast<size_t>(rowIndex)].dbId;
        option.currentOperatorId = rc.operatorId;
        option.currentOperatorName = rc.operatorId == -1 ? QString() : operatorName(rc.operatorId);
        recipientOptions.push_back(option);
    }

    if (!isNew) {
        const AccountRow& acc = accounts_[static_cast<size_t>(rowIndex)];
        data.dbId = acc.dbId;
        data.loginId = acc.id;
        data.displayName = acc.name;
        data.role = koreanRoleToEnum(acc.role);
        data.active = acc.active;
        data.lastLogin = acc.lastLogin;
        data.isSelf = (acc.id == currentUserLoginId_);
        data.isLastActiveAdmin = isLastActiveAdmin(rowIndex);
    }

    auto* dialog = new AccountDialog(isNew ? AccountDialog::Mode::Register : AccountDialog::Mode::Edit, data,
                                      existingIds, recipientOptions, this);
    connect(dialog, &AccountDialog::accountSaved, this,
            [this, rowIndex, isNew](const AccountDialog::AccountData& saved) {
                if (isNew) {
                    // 신규 등록은 서버(DB)에 저장하고, 응답 후 목록을 다시 불러와 반영한다.
                    registerAccount(saved);
                    return;
                }
                // 기존 계정 수정도 서버(DB)에 반영한다. 성공 시 updateAccount()가
                // assignRecipients()를 거쳐 fetchAccounts()까지 다시 불러와 반영한다.
                updateAccount(rowIndex, saved);
            });
    connect(dialog, &AccountDialog::passwordChangeRequested, this,
            [this, rowIndex](const QString&, const QString& newPassword) {
                const int dbId = accounts_[static_cast<size_t>(rowIndex)].dbId;
                changePassword(dbId, newPassword);
            });
    dialog->exec();
    dialog->deleteLater();
}

void P7SettingsPage::refreshThresholdRobotCombo() {
    const int previousDbId = thresholdRobotCombo_->count() > 0
                                  ? thresholdRobotCombo_->currentData().toInt()
                                  : -1;
    thresholdRobotCombo_->blockSignals(true);
    thresholdRobotCombo_->clear();
    int restoreIndex = -1;
    for (const RobotRow& row : robots_) {
        thresholdRobotCombo_->addItem(row.id, row.dbId);
        if (row.dbId == previousDbId) restoreIndex = thresholdRobotCombo_->count() - 1;
    }
    thresholdRobotCombo_->blockSignals(false);
    if (thresholdRobotCombo_->count() == 0) return;
    const int indexToSelect = restoreIndex >= 0 ? restoreIndex : 0;
    thresholdRobotCombo_->setCurrentIndex(indexToSelect);
    fetchRobotThresholds(thresholdRobotCombo_->itemData(indexToSelect).toInt());
}

void P7SettingsPage::fetchRobotThresholds(int robotDbId) {
    QNetworkRequest request(QUrl(QStringLiteral("%1/robots/%2/thresholds").arg(serverUrl_).arg(robotDbId)));
    QNetworkReply* reply = netManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode == 200) {
            const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
            batteryAttentionEdit_->setText(QString::number(body.value("battery_attention_pct").toInt(30)));
            batteryReturnEdit_->setText(QString::number(body.value("battery_return_pct").toInt(28)));
            voiceWaitEdit_->setText(QString::number(body.value("voice_wait_sec").toInt(8)));
            reconnectRetryEdit_->setText(QString::number(body.value("reconnect_retry_sec").toInt(30)));
            medMissedWaitEdit_->setText(QString::number(body.value("med_missed_wait_min").toInt(10)));
            medReannounceEdit_->setText(QString::number(body.value("med_reannounce_count").toInt(2)));
        }
        reply->deleteLater();
    });
}

namespace {
QJsonObject collectThresholdBody(QLineEdit* batteryAttention, QLineEdit* batteryReturn, QLineEdit* voiceWait,
                                  QLineEdit* reconnectRetry, QLineEdit* medMissedWait, QLineEdit* medReannounce) {
    return QJsonObject{
        {"battery_attention_pct", batteryAttention->text().trimmed().toInt()},
        {"battery_return_pct", batteryReturn->text().trimmed().toInt()},
        {"voice_wait_sec", voiceWait->text().trimmed().toInt()},
        {"reconnect_retry_sec", reconnectRetry->text().trimmed().toInt()},
        {"med_missed_wait_min", medMissedWait->text().trimmed().toInt()},
        {"med_reannounce_count", medReannounce->text().trimmed().toInt()},
    };
}
}  // namespace

void P7SettingsPage::saveThresholdsForCurrentRobot() {
    if (thresholdRobotCombo_->count() == 0) return;
    const int robotDbId = thresholdRobotCombo_->currentData().toInt();
    const QJsonObject body = collectThresholdBody(batteryAttentionEdit_, batteryReturnEdit_, voiceWaitEdit_,
                                                   reconnectRetryEdit_, medMissedWaitEdit_, medReannounceEdit_);
    QNetworkRequest request(QUrl(QStringLiteral("%1/robots/%2/thresholds").arg(serverUrl_).arg(robotDbId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = netManager_->put(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 204) {
            QMessageBox::warning(this, QStringLiteral("임계값 저장 실패"), QStringLiteral("서버에 연결할 수 없습니다"));
        }
        reply->deleteLater();
    });
}

void P7SettingsPage::applyThresholdsToAllRobots() {
    const QJsonObject body = collectThresholdBody(batteryAttentionEdit_, batteryReturnEdit_, voiceWaitEdit_,
                                                   reconnectRetryEdit_, medMissedWaitEdit_, medReannounceEdit_);
    QNetworkRequest request(QUrl(serverUrl_ + QStringLiteral("/robots/thresholds/apply-all")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = netManager_->put(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 204) {
            QMessageBox::warning(this, QStringLiteral("전체 적용 실패"), QStringLiteral("서버에 연결할 수 없습니다"));
        } else {
            QMessageBox::information(this, QStringLiteral("전체 적용"),
                                      QStringLiteral("현재 등록된 모든 로봇에 임계값을 적용했습니다"));
        }
        reply->deleteLater();
    });
}
