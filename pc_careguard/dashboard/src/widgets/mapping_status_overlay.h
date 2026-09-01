#pragma once
#include <QFrame>
#include <QVector>

class QLabel;
class QResizeEvent;
class QMouseEvent;

// P5 "매핑 현황" 펼침 오버레이 — 문서 §6: "레이아웃을 밀지 않는" 자식 오버레이 위젯.
// 헤더(this)는 일반 카드 한 줄이고, 클릭하면 같은 부모 위에 떠 있는 패널(panel_)을
// 헤더 바로 아래에 절대 좌표로 배치·raise()해서 아래 콘텐츠를 덮으며 펼쳐진다.
class MappingStatusOverlay : public QFrame {
    Q_OBJECT
public:
    explicit MappingStatusOverlay(QWidget* parent);

    // 파일 등록/SLAM 저장으로 robotId의 맵이 실제 배포됐음을 표시. 맵 이름·대상자도
    // 사용자가 등록 시 입력한 값으로 갱신한다 (WP/경로/최종배포 갱신 + 경고 해제).
    void markMapDeployed(const QString& robotId, const QString& mapName, const QString& recipientName);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

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
    void toggle();
    void reposition();

    QFrame* panel_;
    QLabel* chevron_;
    QLabel* hint_;
    QLabel* summary_;
    bool open_;
    QVector<RowWidgets> rows_;
};
