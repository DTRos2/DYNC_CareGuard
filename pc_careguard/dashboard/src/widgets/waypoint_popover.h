#pragma once
#include <QFrame>
#include "waypoint_canvas.h"
#include "yaw_dial.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

// P6 웨이포인트 추가/편집 팝오버 — p6_waypoint_add_popover_wireframe.html 대응.
// 모달이 아니라 캔버스 위에 떠 있는 카드(부모=페이지, 클릭 지점 근처에 절대 좌표 배치).
class WaypointPopover : public QFrame {
    Q_OBJECT
public:
    explicit WaypointPopover(QWidget* parent);

    // meters는 canvasPos를 실제 map 프레임 미터로 환산한 값(표시용 — 저장은 여전히
    // canvasPos 기준). yawDeg는 "WP 추가" 드래그로 이미 잡은 방향(WaypointCanvas::
    // waypointAddRequested) — 다이얼을 처음부터 그 방향으로 채워서, 드래그로 방향까지
    // 정한 경우 다이얼을 또 따로 만질 필요가 없게 한다(여전히 조정은 가능).
    void setForNewWaypoint(QPointF canvasPos, QPointF meters, qreal yawDeg);
    void setForEditWaypoint(const WaypointCanvas::Waypoint& wp);
    // "경로 이동"/"주행 취소" 토글 버튼 상태 — 호출부(P6PatrolPage)가 로봇 연결/주행 중
    // 상태에 맞춰 팝오버를 열기 직전, 그리고 driveRequested/cancelDriveRequested 응답이
    // 온 뒤에 맞춘다. isDrivingThis가 true면 canInteract는 무시하고 "주행 취소"로
    // 항상 활성화한다(오프라인이어도 DB의 stuck 기록 정리는 가능해야 하므로). 신규
    // 웨이포인트(setForNewWaypoint) 상태에선 버튼 자체가 숨어 있어 이 값과 무관하다.
    void setDriveButtonState(bool canInteract, bool isDrivingThis);
    // 지금 편집 중인 웨이포인트 id(신규면 빈 문자열). 팝오버가 열려 있는 동안
    // P6PatrolPage가 "이 팝오버가 지금 주행 중인 그 웨이포인트를 보고 있는지" 판단하는 데 쓴다.
    QString editingWaypointId() const { return editingId_; }

signals:
    // id가 비어 있으면 신규 생성. kind는 "patrol"/"charge"/"emergency" (WaypointCanvas::
    // Waypoint::kind와 같은 값).
    void saveRequested(const QString& id, const QString& name, const QString& kind, qreal yawDeg,
                        QPointF canvasPos);
    void deleteRequested(const QString& id);
    void cancelled();
    // "경로 이동" 클릭 — 편집 중인(이미 저장된) 웨이포인트로만 뜨므로 id가 항상 채워져 있다.
    // 팝오버는 hide()하지 않는다 — 응답이 오면 호출부가 setDriveButtonState로 "주행 취소"로
    // 토글해 같은 팝오버 안에서 상태를 보여준다.
    void driveRequested(const QString& id);
    // "주행 취소" 클릭(토글된 상태에서).
    void cancelDriveRequested(const QString& id);

private:
    void setType(const QString& kind);
    void emitSave();

    QLabel* titleLabel_;
    QLineEdit* nameEdit_;
    QPushButton* typePatrolBtn_;
    QPushButton* typeChargingBtn_;
    QPushButton* typeEmergencyBtn_;
    YawDial* yawDial_;
    QSpinBox* yawSpin_;
    QPushButton* saveBtn_;
    QPushButton* cancelBtn_;
    QPushButton* driveBtn_;
    QPushButton* deleteBtn_;

    QString editingId_;
    QPointF canvasPos_;
    QString kind_;
    bool drivingThis_ = false;
};
