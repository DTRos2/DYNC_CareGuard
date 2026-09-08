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

signals:
    // id가 비어 있으면 신규 생성.
    void saveRequested(const QString& id, const QString& name, bool charging, qreal yawDeg,
                        QPointF canvasPos);
    void deleteRequested(const QString& id);
    void cancelled();

private:
    void setType(bool charging);
    void emitSave();

    QLabel* titleLabel_;
    QLineEdit* nameEdit_;
    QPushButton* typePatrolBtn_;
    QPushButton* typeChargingBtn_;
    YawDial* yawDial_;
    QSpinBox* yawSpin_;
    QPushButton* saveBtn_;
    QPushButton* cancelBtn_;
    QPushButton* deleteBtn_;

    QString editingId_;
    QPointF canvasPos_;
    bool isCharging_;
};
