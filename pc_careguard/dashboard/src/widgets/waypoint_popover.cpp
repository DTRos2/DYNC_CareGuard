#include "waypoint_popover.h"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include "theme/tokens.h"

WaypointPopover::WaypointPopover(QWidget* parent) : QFrame(parent), isCharging_(false) {
    setObjectName("mappingStatusPanel");  // 흰 카드 + 테두리 재사용
    // 주의: 여기서 setFixedWidth()로 즉시 resize()하면 그 시점엔 자식이 하나도 없어
    // 높이가 기본값(작은 값)으로 굳어버리고(WA_Resized), 이후 레이아웃이 실제 내용에
    // 맞춰 다시 키워주지 않아 팝오버가 납작하게 보인다. min/max만 지정하고 실제 크기는
    // 아래에서 자식 구성을 마친 뒤 adjustSize()로 정한다.
    setMinimumWidth(210);
    setMaximumWidth(210);
    hide();

    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(Tokens::SHADOW_LG.blur);
    shadow->setOffset(0, Tokens::SHADOW_LG.dy);
    shadow->setColor(Tokens::SHADOW_LG.color);
    setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(3);

    titleLabel_ = new QLabel(QStringLiteral("새 웨이포인트"), this);
    titleLabel_->setStyleSheet(QStringLiteral("font-size:11px; font-weight:500; color:#16202B;"));
    layout->addWidget(titleLabel_);
    layout->addSpacing(4);

    auto* nameLabel = new QLabel(QStringLiteral("이름"), this);
    nameLabel->setProperty("role", "secondary");
    layout->addWidget(nameLabel);
    nameEdit_ = new QLineEdit(this);
    layout->addWidget(nameEdit_);
    layout->addSpacing(5);

    auto* typeLabel = new QLabel(QStringLiteral("유형"), this);
    typeLabel->setProperty("role", "secondary");
    layout->addWidget(typeLabel);
    auto* typeRow = new QHBoxLayout();
    typeRow->setSpacing(4);
    typePatrolBtn_ = new QPushButton(QStringLiteral("순찰 지점"), this);
    typeChargingBtn_ = new QPushButton(QStringLiteral("충전 스테이션"), this);
    for (auto* btn : {typePatrolBtn_, typeChargingBtn_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        typeRow->addWidget(btn);
    }
    typePatrolBtn_->setProperty("pos", "first");
    typeChargingBtn_->setProperty("pos", "last");
    connect(typePatrolBtn_, &QPushButton::clicked, this, [this] { setType(false); });
    connect(typeChargingBtn_, &QPushButton::clicked, this, [this] { setType(true); });
    layout->addLayout(typeRow);
    layout->addSpacing(5);

    auto* yawLabel = new QLabel(QStringLiteral("yaw (도착 방향)"), this);
    yawLabel->setProperty("role", "secondary");
    layout->addWidget(yawLabel);
    auto* yawRow = new QHBoxLayout();
    yawRow->setSpacing(6);
    yawDial_ = new YawDial(this);
    yawRow->addWidget(yawDial_);
    yawSpin_ = new QSpinBox(this);
    yawSpin_->setRange(0, 359);
    yawSpin_->setSuffix(QStringLiteral("°"));
    yawSpin_->setFixedWidth(66);
    yawRow->addWidget(yawSpin_);
    yawRow->addStretch();
    layout->addLayout(yawRow);
    layout->addSpacing(8);

    connect(yawDial_, &YawDial::angleChanged, this, [this](int deg) {
        const QSignalBlocker blocker(yawSpin_);
        yawSpin_->setValue(deg);
    });
    connect(yawSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int deg) {
        const QSignalBlocker blocker(yawDial_);
        yawDial_->setAngleDeg(deg);
    });

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);
    saveBtn_ = new QPushButton(QStringLiteral("저장"), this);
    saveBtn_->setProperty("variant", "primary");
    connect(saveBtn_, &QPushButton::clicked, this, &WaypointPopover::emitSave);
    btnRow->addWidget(saveBtn_, /*stretch=*/1);
    cancelBtn_ = new QPushButton(QStringLiteral("취소"), this);
    cancelBtn_->setProperty("variant", "secondary");
    connect(cancelBtn_, &QPushButton::clicked, this, [this] {
        hide();
        emit cancelled();
    });
    btnRow->addWidget(cancelBtn_);
    layout->addLayout(btnRow);

    deleteBtn_ = new QPushButton(QStringLiteral("삭제"), this);
    deleteBtn_->setProperty("variant", "danger");
    deleteBtn_->hide();
    connect(deleteBtn_, &QPushButton::clicked, this, [this] {
        hide();
        emit deleteRequested(editingId_);
    });
    layout->addWidget(deleteBtn_);

    setType(false);
    adjustSize();
}

void WaypointPopover::setType(bool charging) {
    isCharging_ = charging;
    typePatrolBtn_->setChecked(!charging);
    typeChargingBtn_->setChecked(charging);
}

void WaypointPopover::setForNewWaypoint(QPointF canvasPos) {
    editingId_.clear();
    canvasPos_ = canvasPos;
    titleLabel_->setText(QStringLiteral("새 웨이포인트  x %1, y %2")
                              .arg(canvasPos.x() / 40.0, 0, 'f', 1)
                              .arg(canvasPos.y() / 40.0, 0, 'f', 1));
    nameEdit_->setText(QString());
    nameEdit_->setFocus();
    setType(false);
    yawSpin_->setValue(0);
    deleteBtn_->hide();
    adjustSize();
}

void WaypointPopover::setForEditWaypoint(const WaypointCanvas::Waypoint& wp) {
    editingId_ = wp.id;
    canvasPos_ = wp.pos;
    titleLabel_->setText(QStringLiteral("웨이포인트 편집  %1").arg(wp.id));
    nameEdit_->setText(wp.name);
    setType(wp.charging);
    yawSpin_->setValue(static_cast<int>(wp.yawDeg));
    deleteBtn_->setVisible(!wp.charging);
    adjustSize();
}

void WaypointPopover::emitSave() {
    const QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        nameEdit_->setProperty("state", "error");
        style()->unpolish(nameEdit_);
        style()->polish(nameEdit_);
        return;
    }
    hide();
    emit saveRequested(editingId_, name, isCharging_, yawSpin_->value(), canvasPos_);
}
