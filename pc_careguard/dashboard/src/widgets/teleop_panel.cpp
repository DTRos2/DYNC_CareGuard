#include "teleop_panel.h"

#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/simple_icons.h"

namespace {
constexpr qreal kFineLinear = 0.10, kFineAngular = 0.3;
constexpr qreal kNormalLinear = 0.30, kNormalAngular = 0.5;
constexpr qreal kFastLinear = 0.50, kFastAngular = 0.8;
constexpr qreal kLinearMax = 0.80;
constexpr qreal kAngularMax = 1.50;
}  // namespace

TeleopPanel::TeleopPanel(QWidget* parent) : QFrame(parent) {
    setObjectName("card");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                              Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    root->setSpacing(4);

    auto* title = new QLabel(QStringLiteral("원격 조작 — 방향키"), this);
    title->setProperty("role", "label");
    root->addWidget(title);
    root->addSpacing(4);

    buildKeypad(root);
    buildPresets(root);

    buildSpeedRow(root, QStringLiteral("선속도"), QStringLiteral("m/s"), linearSpin_, linearSlider_,
                  kLinearMax, 0.05);
    buildSpeedRow(root, QStringLiteral("각속도"), QStringLiteral("rad/s"), angularSpin_, angularSlider_,
                  kAngularMax, 0.05);

    currentInputLabel_ = new QLabel(QStringLiteral("현재 입력: 정지"), this);
    currentInputLabel_->setProperty("role", "caption");
    root->addWidget(currentInputLabel_);

    applyPreset(kNormalLinear, kNormalAngular);
    presetNormal_->setChecked(true);
}

void TeleopPanel::buildKeypad(QVBoxLayout* root) {
    auto* grid = new QGridLayout();
    grid->setSpacing(4);

    // p5_slam_mapping_wireframe_standalone.html: grid-template-columns:repeat(3,1fr) —
    // 3칸이 패널 폭을 균등하게 나눠 끝까지 채운다. 고정 폭 버튼은 옆에 여백이 남아
    // 와이어프레임보다 좁아 보이므로, 높이만 고정하고 가로는 칸을 채우게 한다.
    auto makeArrow = [this](const QIcon& icon) {
        auto* btn = new QPushButton(icon, QString(), this);
        btn->setObjectName("keypadBtn");
        btn->setIconSize(QSize(15, 15));
        btn->setFixedHeight(32);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return btn;
    };

    upBtn_ = makeArrow(SimpleIcons::arrowUp(Tokens::TEXT_SECONDARY, 15));
    leftBtn_ = makeArrow(SimpleIcons::arrowLeft(Tokens::TEXT_SECONDARY, 15));
    downBtn_ = makeArrow(SimpleIcons::arrowDown(Tokens::TEXT_SECONDARY, 15));
    rightBtn_ = makeArrow(SimpleIcons::arrowRight(Tokens::TEXT_SECONDARY, 15));

    grid->addWidget(upBtn_, 0, 1);
    grid->addWidget(leftBtn_, 1, 0);
    grid->addWidget(downBtn_, 1, 1);
    grid->addWidget(rightBtn_, 1, 2);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    root->addLayout(grid);
    root->addSpacing(2);

    spaceLabel_ = new QLabel(QStringLiteral("Space — 정지"), this);
    spaceLabel_->setObjectName("keypadSpace");
    spaceLabel_->setAlignment(Qt::AlignCenter);
    spaceLabel_->setFixedHeight(22);
    root->addWidget(spaceLabel_);
    root->addSpacing(8);

    auto wireHold = [this](QPushButton* btn, int key) {
        connect(btn, &QPushButton::pressed, this, [this, key] { setDirectionActive(key, true); });
        connect(btn, &QPushButton::released, this, [this, key] { setDirectionActive(key, false); });
    };
    wireHold(upBtn_, Qt::Key_Up);
    wireHold(downBtn_, Qt::Key_Down);
    wireHold(leftBtn_, Qt::Key_Left);
    wireHold(rightBtn_, Qt::Key_Right);
}

void TeleopPanel::buildPresets(QVBoxLayout* root) {
    auto* label = new QLabel(QStringLiteral("속도 프리셋"), this);
    label->setProperty("role", "caption");
    root->addWidget(label);

    auto* row = new QHBoxLayout();
    row->setSpacing(4);

    auto* group = new QButtonGroup(this);
    group->setExclusive(true);

    presetFine_ = new QPushButton(QStringLiteral("미세"), this);
    presetNormal_ = new QPushButton(QStringLiteral("보통"), this);
    presetFast_ = new QPushButton(QStringLiteral("빠름"), this);

    for (auto* btn : {presetFine_, presetNormal_, presetFast_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        row->addWidget(btn, /*stretch=*/1);  // 와이어프레임의 flex:1 — 3칸 균등폭
    }
    presetFine_->setProperty("pos", "first");
    presetFast_->setProperty("pos", "last");

    group->addButton(presetFine_);
    group->addButton(presetNormal_);
    group->addButton(presetFast_);

    connect(presetFine_, &QPushButton::clicked, this, [this] { applyPreset(kFineLinear, kFineAngular); });
    connect(presetNormal_, &QPushButton::clicked, this,
            [this] { applyPreset(kNormalLinear, kNormalAngular); });
    connect(presetFast_, &QPushButton::clicked, this, [this] { applyPreset(kFastLinear, kFastAngular); });

    root->addLayout(row);
    root->addSpacing(8);
}

void TeleopPanel::buildSpeedRow(QVBoxLayout* root, const QString& label, const QString& unit,
                                 QDoubleSpinBox*& outSpin, QSlider*& outSlider, double max, double step) {
    auto* row = new QHBoxLayout();
    row->setSpacing(5);

    auto* nameLabel = new QLabel(label, this);
    nameLabel->setProperty("role", "secondary");
    row->addWidget(nameLabel, /*stretch=*/1);

    outSpin = new QDoubleSpinBox(this);
    outSpin->setRange(0.0, max);
    outSpin->setSingleStep(step);
    outSpin->setDecimals(2);
    outSpin->setFixedWidth(52);  // "rad/s" 단위 라벨에 폭을 더 내주기 위해 살짝 줄임.
    outSpin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(outSpin);

    auto* unitLabel = new QLabel(unit, this);
    unitLabel->setProperty("role", "muted");
    // "rad/s"(5자)가 14px 기본 폰트에서 30px 폭에 다 안 들어가 잘려 보이던 문제 —
    // 라벨 폭을 넓히고 그만큼 스핀박스 폭을 줄여, 값 입력칸을 왼쪽으로 당겨 자리를 낸다.
    unitLabel->setFixedWidth(38);
    row->addWidget(unitLabel);
    root->addLayout(row);

    outSlider = new QSlider(Qt::Horizontal, this);
    outSlider->setRange(0, static_cast<int>(max * 100));
    root->addWidget(outSlider);
    root->addSpacing(6);

    connect(outSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, outSlider](double v) {
                const QSignalBlocker blocker(outSlider);
                outSlider->setValue(static_cast<int>(v * 100));
                recomputeVelocity();
            });
    connect(outSlider, &QSlider::valueChanged, this, [this, outSpin](int v) {
        const QSignalBlocker blocker(outSpin);
        outSpin->setValue(v / 100.0);
        recomputeVelocity();
    });
}

void TeleopPanel::applyPreset(qreal linear, qreal angular) {
    linearSpin_->setValue(linear);
    angularSpin_->setValue(angular);
}

void TeleopPanel::setDirectionActive(int key, bool active) {
    if (active) {
        pressedKeys_.insert(key);
    } else {
        pressedKeys_.remove(key);
    }

    auto restyle = [](QPushButton* btn, bool on) {
        btn->setProperty("active", on);
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
    };
    restyle(upBtn_, pressedKeys_.contains(Qt::Key_Up));
    restyle(downBtn_, pressedKeys_.contains(Qt::Key_Down));
    restyle(leftBtn_, pressedKeys_.contains(Qt::Key_Left));
    restyle(rightBtn_, pressedKeys_.contains(Qt::Key_Right));

    recomputeVelocity();
}

void TeleopPanel::handleKeyPress(int key) {
    if (key == Qt::Key_Space) {
        pressedKeys_.clear();
        spaceLabel_->setProperty("active", true);
        spaceLabel_->style()->unpolish(spaceLabel_);
        spaceLabel_->style()->polish(spaceLabel_);
        recomputeVelocity();
        return;
    }
    if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right) {
        setDirectionActive(key, true);
    }
}

void TeleopPanel::handleKeyRelease(int key) {
    if (key == Qt::Key_Space) {
        spaceLabel_->setProperty("active", false);
        spaceLabel_->style()->unpolish(spaceLabel_);
        spaceLabel_->style()->polish(spaceLabel_);
        return;
    }
    if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right) {
        setDirectionActive(key, false);
    }
}

void TeleopPanel::recomputeVelocity() {
    const qreal linearMax = linearSpin_->value();
    const qreal angularMax = angularSpin_->value();

    qreal linear = 0.0, angular = 0.0;
    QString label = QStringLiteral("정지");

    if (pressedKeys_.contains(Qt::Key_Up)) {
        linear += linearMax;
        label = QStringLiteral("전진 ↑");
    }
    if (pressedKeys_.contains(Qt::Key_Down)) {
        linear -= linearMax;
        label = QStringLiteral("후진 ↓");
    }
    if (pressedKeys_.contains(Qt::Key_Left)) {
        angular += angularMax;
        label = pressedKeys_.contains(Qt::Key_Up) || pressedKeys_.contains(Qt::Key_Down)
                    ? label + QStringLiteral(" + 좌회전")
                    : QStringLiteral("좌회전 ↺");
    }
    if (pressedKeys_.contains(Qt::Key_Right)) {
        angular -= angularMax;
        label = pressedKeys_.contains(Qt::Key_Up) || pressedKeys_.contains(Qt::Key_Down)
                    ? label + QStringLiteral(" + 우회전")
                    : QStringLiteral("우회전 ↻");
    }

    currentInputLabel_->setText(QStringLiteral("현재 입력: %1 · 지연 0.3s").arg(label));
    emit velocityChanged(linear, angular);
}
