#pragma once
#include <QWidget>
#include <QImage>
#include <QMap>
#include <QVector>

#include "widgets/waypoint_canvas.h"

class QLabel;
class QPushButton;
class QLineEdit;
class QComboBox;
class QListWidget;
class QTimeEdit;
class QVBoxLayout;
class QHBoxLayout;
class QStackedLayout;
class WaypointPopover;
class QKeyEvent;
class QMenu;
class QFrame;

// P6 순찰·경로 — 웨이포인트 편집 캔버스 + WP 목록/경로/스케줄.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 5), §7 참고.
class P6PatrolPage : public QWidget {
    Q_OBJECT
public:
    explicit P6PatrolPage(QWidget* parent = nullptr);

public slots:
    // P5MapPage::mapRegistered를 MainWindow가 중계해서 부른다. robotId가 이미 있는 맵이면
    // 이름·대상자·배경 이미지를 갱신하고, 없으면 새 맵으로 추가한다.
    void upsertMap(const QImage& mapImage, const QString& mapName, const QString& robotId,
                    const QString& recipientName);

protected:
    // 웨이포인트 목록의 각 행(QFrame, "wpId" 프로퍼티로 표시)을 클릭하면 편집 팝오버를
    // 연다. p6_patrol_route_wireframe.html의 목록 행에는 별도 액션 아이콘이 없어서,
    // 대신 행 전체를 눌러 편집으로 들어가는 방식으로 그 상호작용을 유지한다.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Schedule {
        QString routeName;
        QString timeText;
        QVector<int> days;  // 0=월 .. 6=일
        bool enabled;
    };

    // 맵마다 웨이포인트·경로·스케줄이 독립적이다. 현재 맵의 상태는 waypoints_/routes_/...
    // 멤버에 "펼쳐진" 채로 들고 있다가, 맵을 전환할 때 이 구조체로 저장·복원한다.
    struct MapData {
        QString mapName;
        QString robotId;
        QString recipientName;
        QVector<WaypointCanvas::Waypoint> waypoints;
        QMap<QString, QVector<QString>> routes;
        QString currentRoute;
        int nextWpSeq = 1;
        QVector<Schedule> schedules;
        QImage backgroundImage;  // P5에서 파일로 등록한 실제 pgm 맵 (없으면 null)
    };

    QWidget* buildHeaderRow();
    QWidget* buildCanvasCard();
    QWidget* buildRightColumn();
    QWidget* buildWpListCard();
    QWidget* buildRouteCard();
    QWidget* buildScheduleCard();

    void rebuildWpList();
    void rebuildRouteChips();
    void rebuildRouteOrderList();
    void rebuildRouteEditList();
    void rebuildScheduleList();
    void refreshCanvasPreview();

    QString nextWaypointId() const;
    QString chargingWaypointId() const;

    void handleCanvasAddRequested(QPointF pos);
    void handleCanvasEditRequested(const QString& id);
    void handleCanvasDeleteRequested(const QString& id);
    void handleCanvasMoved(const QString& id, QPointF pos);
    void handlePopoverSave(const QString& id, const QString& name, bool charging, qreal yaw, QPointF pos);

    void enterRouteEditMode(const QString& routeName);
    void exitRouteEditMode(bool save);
    void handleRouteWaypointClicked(const QString& id);
    void startNewRoute();
    void deleteCurrentRoute();
    void runTestRun();
    void driveToSingleWaypoint(const QString& id);

    void toggleScheduleForm(bool show);
    void addScheduleFromForm();

    void saveCurrentMapData();
    void loadMapData(int index);
    void switchToMap(int index);
    void rebuildMapMenu();

    QPushButton* mapChipBtn_;
    QMenu* mapMenu_;
    QVector<MapData> maps_;
    int currentMapIndex_;

    QWidget* wpListCard_;
    QVBoxLayout* wpListLayout_;

    QStackedLayout* routeStack_;
    QHBoxLayout* routeChipsLayout_;
    QListWidget* routeOrderList_;
    QLabel* routeStatsLabel_;
    QLineEdit* routeEditNameEdit_;
    QVBoxLayout* routeEditListLayout_;

    QVBoxLayout* scheduleListLayout_;
    QWidget* scheduleForm_;
    QComboBox* scheduleRouteCombo_;
    QTimeEdit* scheduleTimeEdit_;
    QVector<QPushButton*> scheduleDayChips_;

    WaypointCanvas* canvas_;
    WaypointPopover* popover_;
    QWidget* canvasToolbarRow_;
    QFrame* routeEditBanner_;

    QVector<WaypointCanvas::Waypoint> waypoints_;
    QMap<QString, QVector<QString>> routes_;
    QString currentRoute_;
    int nextWpSeq_;

    bool routeEditMode_;
    QString routeEditTargetName_;
    QVector<QString> routeEditSelection_;

    QVector<Schedule> schedules_;
};
