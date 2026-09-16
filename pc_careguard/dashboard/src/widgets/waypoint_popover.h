#pragma once
#include <QFrame>
#include <QPoint>
#include "waypoint_canvas.h"
#include "yaw_dial.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QHideEvent;
class QEvent;
class QMouseEvent;

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
    // preserveEditMode=false(기본, 마커를 새로 클릭했을 때)면 항상 "보기" 모드로
    // 연다 — 편집(input값/위치/회전)은 "편집" 버튼을 눌러야 가능하다(요청 사양:
    // 팝오버가 열리자마자 바로 편집 가능한 상태였던 것을 막기 위함). true(드래그로
    // 위치를 옮긴 직후, 또는 리사이즈로 좌표를 다시 맞출 때 — P6PatrolPage::
    // handleCanvasMoved/viewRescaled)면 지금 편집 중이던 상태를 그대로 유지한 채
    // canvasPos_/필드 값만 최신으로 새로고친다.
    void setForEditWaypoint(const WaypointCanvas::Waypoint& wp, bool preserveEditMode = false);
    // 지금 "편집" 모드인지(저장 버튼이 활성화된 입력값/위치/회전 수정을 허용하는
    // 상태인지) — P6PatrolPage가 이 값이 바뀔 때(editModeToggled) 캔버스의 드래그
    // 가능한 마커(WaypointCanvas::setEditableWaypointId)를 맞추는 데 쓴다.
    bool isEditModeActive() const { return editModeActive_; }
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
    // "편집"/"저장" 토글 상태가 바뀌었다(신규 웨이포인트는 항상 true라 신호를 내지
    // 않는다 — 아직 캔버스에 마커가 없어 draggable 여부가 의미 없다). P6PatrolPage가
    // WaypointCanvas::setEditableWaypointId(editing ? editingWaypointId() : "")로 잇는다.
    void editModeToggled(bool editing);

protected:
    // 팝오버 프레임(제목 라벨 포함, 입력칸·버튼 위는 제외)을 마우스로 눌러 드래그하면
    // 페이지 안에서 자유롭게 옮길 수 있다 — 예전엔 처음 뜬 자리에 고정이라 지도의
    // 다른 부분을 가리면 치울 방법이 없었다.
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    // 팝오버가 어떤 경로로 닫히든(저장/취소/삭제/호출부의 hide()) 편집 모드를 그때마다
    // 각자 리셋해주지 않아도 여기 한 곳에서 자동으로 정리한다.
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setType(const QString& kind);
    void emitSave();
    void handleSaveEditClick();
    void enterEditMode();
    // editModeActive_를 지정한 값으로 맞추고 필드 활성화/버튼 문구·색을 그에 맞게
    // 다시 그린다 — 값 자체를 바꾸지 않고(=preserveEditMode) 그냥 다시 칠할 때도 쓴다.
    void applyEditModeState();

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
    // 기존 웨이포인트를 처음 열면 false("보기") — "편집" 버튼을 눌러야 true가 되고,
    // 그래야 이름/유형/yaw 필드가 활성화되고 캔버스에서 이 마커를 드래그(위치/회전
    // 재설정)할 수 있다. 신규 웨이포인트(setForNewWaypoint)는 항상 true.
    bool editModeActive_ = false;

    bool dragging_ = false;
    QPoint dragStartMouseGlobalPos_;
    QPoint dragStartFramePos_;
};
