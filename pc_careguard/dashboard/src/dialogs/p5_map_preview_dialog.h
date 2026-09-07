#pragma once
#include <QDialog>
#include <QImage>

class QFrame;
class QLabel;
class QStackedLayout;
class ImageZoomView;

// P5 맵 미리보기 모달 — MappingStatusOverlay 각 행의 미리보기 버튼에서 연다.
// 편집 기능은 없는 순수 읽기 전용 뷰어: 맵 이미지(휠 확대/축소·드래그 이동 가능)
// + 맵 이름·대상자·로봇 ID만 보여준다.
class MapPreviewDialog : public QDialog {
    Q_OBJECT
public:
    explicit MapPreviewDialog(QWidget* parent = nullptr);

    // image가 비어 있으면(아직 매핑·등록된 지도가 없는 로봇) 빈 상태 화면을 보여준다.
    void setMapInfo(const QImage& image, const QString& mapName, const QString& recipientName,
                     const QString& robotId);

private:
    QFrame* buildHeader();

    QStackedLayout* previewStack_;
    ImageZoomView* imageView_;
    QLabel* emptyStateIcon_;
    QLabel* emptyStateLabel_;
    QLabel* mapNameValue_;
    QLabel* recipientValue_;
    QLabel* robotIdValue_;
};
