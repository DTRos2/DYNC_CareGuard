#pragma once
#include <QFrame>
#include <QVector>

class QLabel;
class QResizeEvent;
class QMouseEvent;
class QGridLayout;
class QEvent;

// P5 "매핑 현황" 펼침 오버레이 — 문서 §6: "레이아웃을 밀지 않는" 자식 오버레이 위젯.
// 헤더(this)는 일반 카드 한 줄이고, 클릭하면 같은 부모 위에 떠 있는 패널(panel_)을
// 헤더 바로 아래에 절대 좌표로 배치·raise()해서 아래 콘텐츠를 덮으며 펼쳐진다.
//
// 행은 고정 더미 목록이 아니라 실제로 맵이 배포된 로봇만큼만 존재한다 — markMapDeployed()가
// 처음 불리는 로봇은 새 행을 만들고, 이미 있는 로봇이면 그 행을 갱신한다. 서버에 아직
// 아무 맵도 없으면(GET /maps/active가 빈 배열) 패널은 헤더 한 줄만 남는다.
class MappingStatusOverlay : public QFrame {
    Q_OBJECT
public:
    explicit MappingStatusOverlay(QWidget* parent);

    // 파일 등록/SLAM 저장 또는 서버 조회(GET /maps/active)로 robotId의 맵이 배포됐음을
    // 표시. 해당 robotId 행이 없으면 새로 만든다. wpCount/routeCount는 실제 개수 —
    // 아직 모르면(예: 방금 파일로 등록해 웨이포인트가 없는 새 맵) 0으로 둔다.
    void markMapDeployed(const QString& robotId, const QString& mapName, const QString& recipientName,
                          int wpCount = 0, int routeCount = 0);

signals:
    // 행의 미리보기 아이콘을 눌렀을 때 — 이 위젯은 맵 이미지를 들고 있지 않으므로
    // 실제 이미지 조회·모달 표시는 P5MapPage가 담당한다.
    void mapPreviewRequested(const QString& robotId);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    // 미리보기 아이콘뿐 아니라 행 전체(라벨 위)를 눌러도 mapPreviewRequested가 나가도록
    // — addRow()가 각 rowFrame에 "robotId" 프로퍼티를 심고 installEventFilter(this)로
    // 등록한다. P6PatrolPage::eventFilter(웨이포인트 목록 "행 클릭 = 편집")와 같은 패턴.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct RowWidgets {
        QString robotId;
        QFrame* frame;
        QLabel* name;
        QLabel* recipient;
        QLabel* wp;
        QLabel* route;
        QLabel* lastDeployed;
    };

    void buildPanel();
    void addRow(const QString& robotId, const QString& mapName, const QString& recipientName, int wpCount,
                int routeCount);
    void updateSummary();
    void toggle();
    void reposition();

    QFrame* panel_;
    QGridLayout* grid_;
    QLabel* chevron_;
    QLabel* hint_;
    QLabel* summary_;
    bool open_;
    QVector<RowWidgets> rows_;
};
