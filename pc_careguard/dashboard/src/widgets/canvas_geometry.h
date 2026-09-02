#pragma once
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QImage>

// P5 SlamMapView, P6 WaypointCanvas가 공유하는 리사이즈 좌표 보정 유틸.
// 두 캔버스 모두 웨이포인트·로봇 위치·궤적을 캔버스 절대 픽셀 좌표로 들고 있어서,
// 창 크기가 바뀌면(예: 최대화) 배경 지도는 새 크기에 맞춰 다시 배치되는데 마커는
// 옛 픽셀 자리에 그대로 남아 지도에서 벗어나 보인다. 그걸 막기 위해 리사이즈 전/후
// bounds 사이에서 이미지(또는 bounds) 기준 상대 위치(비율)를 보존한 채로 점을 옮긴다.
namespace CanvasGeometry {

// image를 bounds 안에 KeepAspectRatio로 중앙 배치했을 때의 (topLeft, 스케일된 크기).
// image가 비어 있으면 false.
bool imagePlacement(const QRectF& bounds, const QImage& image, QPointF* outTopLeft, QSize* outScaledSize);

// oldBounds 기준 점 p를, image가 있으면 이미지 배치 비율로, 없으면 bounds 자체의
// 비율로 보존해 newBounds 기준 좌표로 옮긴다.
QPointF remapOnResize(const QPointF& p, const QRectF& oldBounds, const QRectF& newBounds, const QImage& image);

}  // namespace CanvasGeometry
