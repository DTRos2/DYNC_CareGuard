#include "waypoint_popover.h"

#include <cmath>

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include "theme/tokens.h"

WaypointPopover::WaypointPopover(QWidget* parent) : QFrame(parent), kind_(QStringLiteral("patrol")) {
    setObjectName("mappingStatusPanel");  // 흰 카드 + 테두리 재사용
    // 주의: 여기서 setFixedWidth()로 즉시 resize()하면 그 시점엔 자식이 하나도 없어
    // 높이가 기본값(작은 값)으로 굳어버리고(WA_Resized), 이후 레이아웃이 실제 내용에
    // 맞춰 다시 키워주지 않아 팝오버가 납작하게 보인다. min/max만 지정하고 실제 크기는
    // 아래에서 자식 구성을 마친 뒤 adjustSize()로 정한다.
    // 유형 버튼이 2개("순찰 지점"/"충전 스테이션")에서 3개(+"긴급 버튼")로 늘어나
    // 210px로는 한 줄에 눌러 담기 좁아 240px로 넓혔다.
    setMinimumWidth(240);
    setMaximumWidth(240);
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
    // 제목 라벨 자체는 QWidget이라 클릭을 그냥 삼켜버려(부모로 안 올라옴) 이 프레임의
    // mousePressEvent가 안 불린다 — 이벤트 필터로 가로채 같은 드래그 로직을 태운다.
    titleLabel_->setCursor(Qt::SizeAllCursor);
    titleLabel_->installEventFilter(this);
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
    typeChargingBtn_ = new QPushButton(QStringLiteral("충전"), this);
    typeEmergencyBtn_ = new QPushButton(QStringLiteral("긴급 버튼"), this);
    for (auto* btn : {typePatrolBtn_, typeChargingBtn_, typeEmergencyBtn_}) {
        btn->setObjectName("segBtn");
        btn->setCheckable(true);
        typeRow->addWidget(btn);
    }
    typePatrolBtn_->setProperty("pos", "first");
    typeEmergencyBtn_->setProperty("pos", "last");
    connect(typePatrolBtn_, &QPushButton::clicked, this, [this] { setType(QStringLiteral("patrol")); });
    connect(typeChargingBtn_, &QPushButton::clicked, this, [this] { setType(QStringLiteral("charge")); });
    connect(typeEmergencyBtn_, &QPushButton::clicked, this, [this] { setType(QStringLiteral("emergency")); });
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
    connect(saveBtn_, &QPushButton::clicked, this, &WaypointPopover::handleSaveEditClick);
    btnRow->addWidget(saveBtn_, /*stretch=*/1);
    cancelBtn_ = new QPushButton(QStringLiteral("취소"), this);
    cancelBtn_->setProperty("variant", "secondary");
    connect(cancelBtn_, &QPushButton::clicked, this, [this] {
        hide();
        emit cancelled();
    });
    btnRow->addWidget(cancelBtn_);
    layout->addLayout(btnRow);

    // "경로 이동" — 저장된(편집 중인) 웨이포인트로 실제 주행을 요청한다. 신규
    // 웨이포인트(아직 저장 전)에는 보낼 좌표가 서버에 없으니 숨긴다(setForNewWaypoint).
    driveBtn_ = new QPushButton(QStringLiteral("경로 이동"), this);
    driveBtn_->setProperty("variant", "secondary");
    driveBtn_->hide();
    connect(driveBtn_, &QPushButton::clicked, this, [this] {
        // hide()하지 않는다 — 토글 버튼이라 응답이 오면 같은 팝오버 안에서
        // setDriveButtonState로 "경로 이동" <-> "주행 취소"만 바뀐다.
        if (drivingThis_) {
            emit cancelDriveRequested(editingId_);
        } else {
            emit driveRequested(editingId_);
        }
    });
    layout->addWidget(driveBtn_);

    deleteBtn_ = new QPushButton(QStringLiteral("삭제"), this);
    deleteBtn_->setProperty("variant", "danger");
    deleteBtn_->hide();
    connect(deleteBtn_, &QPushButton::clicked, this, [this] {
        hide();
        emit deleteRequested(editingId_);
    });
    layout->addWidget(deleteBtn_);

    setType(QStringLiteral("patrol"));
    adjustSize();
}

void WaypointPopover::setType(const QString& kind) {
    kind_ = kind;
    typePatrolBtn_->setChecked(kind == QStringLiteral("patrol"));
    typeChargingBtn_->setChecked(kind == QStringLiteral("charge"));
    typeEmergencyBtn_->setChecked(kind == QStringLiteral("emergency"));
}

void WaypointPopover::setForNewWaypoint(QPointF canvasPos, QPointF meters, qreal yawDeg) {
    editingId_.clear();
    canvasPos_ = canvasPos;
    titleLabel_->setText(QStringLiteral("새 웨이포인트  x %1, y %2")
                              .arg(meters.x(), 0, 'f', 1)
                              .arg(meters.y(), 0, 'f', 1));
    nameEdit_->setText(QString());
    nameEdit_->setFocus();
    setType(QStringLiteral("patrol"));
    // yawDeg는 atan2 결과라 (-180, 180] 범위 — 다이얼/스핀박스는 [0, 359]라 감아준다.
    const int deg = ((static_cast<int>(std::lround(yawDeg)) % 360) + 360) % 360;
    yawSpin_->setValue(deg);
    driveBtn_->hide();
    deleteBtn_->hide();
    // 신규 웨이포인트는 볼 게 아직 없으니(저장 전) "보기" 모드 자체가 의미 없다 —
    // 처음부터 편집 가능 상태로 연다. editModeToggled는 내지 않는다(캔버스에 아직
    // 이 id의 마커가 없어 draggable 처리가 의미 없다).
    editModeActive_ = true;
    applyEditModeState();
    adjustSize();
}

void WaypointPopover::setForEditWaypoint(const WaypointCanvas::Waypoint& wp, bool preserveEditMode) {
    editingId_ = wp.id;
    canvasPos_ = wp.pos;
    titleLabel_->setText(QStringLiteral("웨이포인트 편집  %1").arg(wp.id));
    nameEdit_->setText(wp.name);
    setType(wp.kind);
    // wp.yawDeg는 드래그(atan2)로 잡혔으면 (-180, 180] 범위일 수 있다 — 스핀박스
    // range(0, 359)에 그냥 넣으면 음수가 0으로 clamp돼 버려(감아주지 않고 잘림)
    // 실제 방향과 다르게 보인다. setForNewWaypoint와 같은 방식으로 감아준다.
    yawSpin_->setValue(((static_cast<int>(std::lround(wp.yawDeg)) % 360) + 360) % 360);
    driveBtn_->show();
    // 예전엔 충전 스테이션이면 삭제 버튼을 숨겼다 — 캔버스 우클릭 삭제는 애초에
    // 충전/순찰 구분 없이 항상 허용하고 있었으니(WaypointCanvas::mousePressEvent의
    // 우클릭 분기), 팝오버만 더 제한적이었던 불일치였다. 항상 보이게 맞춘다.
    deleteBtn_->show();
    // 실제 토글 상태는 호출부가 이 직후 setDriveButtonState로 맞춘다 — 여기서는
    // "경로 이동"으로 잠깐이라도 잘못된("주행 취소") 텍스트가 보이지 않게만 초기화한다.
    drivingThis_ = false;
    driveBtn_->setText(QStringLiteral("경로 이동"));
    // preserveEditMode=false(마커를 새로 클릭해서 여는 경우)는 항상 "보기" 모드로
    // 되돌린다 — 편집 중이던 다른 웨이포인트를 닫지 않고 이 함수를 호출하는 경로는
    // 없으므로(호출부가 항상 팝오버를 새로 열 때만 false를 쓴다) 안전하다.
    if (!preserveEditMode) editModeActive_ = false;
    applyEditModeState();
    adjustSize();
}

void WaypointPopover::setDriveButtonState(bool canInteract, bool isDrivingThis) {
    drivingThis_ = isDrivingThis;
    if (isDrivingThis) {
        driveBtn_->setText(QStringLiteral("주행 취소"));
        driveBtn_->setProperty("variant", "danger");
        // 취소는 오프라인이어도(DB의 stuck 기록 정리를 위해) 항상 누를 수 있게 둔다.
        driveBtn_->setEnabled(true);
        driveBtn_->setToolTip(QString());
    } else {
        driveBtn_->setText(QStringLiteral("경로 이동"));
        driveBtn_->setProperty("variant", "secondary");
        driveBtn_->setEnabled(canInteract);
        driveBtn_->setToolTip(canInteract ? QString()
                                           : QStringLiteral("로봇 ws 연결을 확인해주세요"));
    }
    style()->unpolish(driveBtn_);
    style()->polish(driveBtn_);
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
    emit saveRequested(editingId_, name, kind_, yawSpin_->value(), canvasPos_);
}

void WaypointPopover::handleSaveEditClick() {
    // 신규 웨이포인트는 editModeActive_가 항상 true라 여기선 항상 emitSave() 경로다 —
    // "보기->편집" 토글은 기존 웨이포인트를 다시 열었을 때(기본 false)만 실제로 일어난다.
    if (!editModeActive_) {
        enterEditMode();
        return;
    }
    emitSave();
}

void WaypointPopover::enterEditMode() {
    editModeActive_ = true;
    applyEditModeState();
    emit editModeToggled(true);
}

void WaypointPopover::applyEditModeState() {
    const bool editing = editModeActive_;
    nameEdit_->setEnabled(editing);
    typePatrolBtn_->setEnabled(editing);
    typeChargingBtn_->setEnabled(editing);
    typeEmergencyBtn_->setEnabled(editing);
    yawDial_->setEnabled(editing);
    yawSpin_->setEnabled(editing);

    // "편집"(secondary, 중립색)과 "저장"(primary, 강조색)을 다른 색으로 구분해 지금
    // 편집 가능한 상태인지 한눈에 보이게 한다(요청 사양).
    saveBtn_->setText(editing ? QStringLiteral("저장") : QStringLiteral("편집"));
    saveBtn_->setProperty("variant", editing ? "primary" : "secondary");
    style()->unpolish(saveBtn_);
    style()->polish(saveBtn_);
}

void WaypointPopover::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    // 신규 웨이포인트(editingId_가 비어 있음)는 editModeToggled(true)를 낸 적이 없으니
    // false도 낼 필요가 없다 — enterEditMode()가 그때만 신호를 낸다는 것과 짝을 이룬다.
    if (editModeActive_ && !editingId_.isEmpty()) {
        editModeActive_ = false;
        emit editModeToggled(false);
    }
}

bool WaypointPopover::eventFilter(QObject* watched, QEvent* event) {
    if (watched == titleLabel_) {
        switch (event->type()) {
            case QEvent::MouseButtonPress:
                mousePressEvent(static_cast<QMouseEvent*>(event));
                return true;
            case QEvent::MouseMove:
                mouseMoveEvent(static_cast<QMouseEvent*>(event));
                return true;
            case QEvent::MouseButtonRelease:
                mouseReleaseEvent(static_cast<QMouseEvent*>(event));
                return true;
            default:
                break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void WaypointPopover::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QFrame::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    dragStartMouseGlobalPos_ = event->globalPosition().toPoint();
    dragStartFramePos_ = pos();
    event->accept();
}

void WaypointPopover::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_ || !(event->buttons() & Qt::LeftButton)) {
        QFrame::mouseMoveEvent(event);
        return;
    }
    const QPoint delta = event->globalPosition().toPoint() - dragStartMouseGlobalPos_;
    QPoint newPos = dragStartFramePos_ + delta;
    // 부모(P6PatrolPage) 영역 밖으로 완전히 나가버리면 다시 찾을 수 없으니 안에 가둔다.
    if (QWidget* p = parentWidget()) {
        newPos.setX(qBound(0, newPos.x(), qMax(0, p->width() - width())));
        newPos.setY(qBound(0, newPos.y(), qMax(0, p->height() - height())));
    }
    move(newPos);
    event->accept();
}

void WaypointPopover::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_) {
        QFrame::mouseReleaseEvent(event);
        return;
    }
    dragging_ = false;
    event->accept();
}
