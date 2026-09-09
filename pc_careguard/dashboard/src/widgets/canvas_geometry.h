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

// 실제 map 프레임 미터 좌표 <-> 캔버스(화면) 좌표 환산. resolution(m/px)·origin은
// map_server .yaml과 같은 값(서버 map.resolution/origin_x/origin_y), bounds/image는
// imagePlacement가 이미지를 그리는 데 쓰는 것과 같은 값이어야 한다 — 그래야 화면에
// 그려지는 자리와 실제로 계산되는 좌표가 일치한다. resolution<=0이거나 image가
// 비어 있으면(아직 서버에서 못 받음) (0,0)을 반환한다 — 호출자가 폴백을 결정한다.
//
// image는 좌상단 원점(row 0 = 이미지 맨 위)인데 map 프레임은 좌하단 원점·Y 위쪽
// 증가라서 세로를 한 번 뒤집어야 한다(SlamMapView::mapMetersToCanvas와 같은 공식).
QPointF metersToCanvas(const QPointF& meters, qreal resolution, const QPointF& origin, const QRectF& bounds,
                       const QImage& image);
QPointF canvasToMeters(const QPointF& canvasPos, qreal resolution, const QPointF& origin, const QRectF& bounds,
                       const QImage& image);

// 방향(yaw) 환산. 캔버스 화면은 Y가 아래로 증가하고, map 프레임은 Y가 위로 증가한다 —
// 이건 위치를 옮길 때는 metersToCanvas/canvasToMeters가 세로를 뒤집어서 처리하는데,
// "각도"는 세로를 뒤집으면 부호가 반대로 뒤집힌다는 걸 깜빡하기 쉽다(회전이 아니라
// 반사이기 때문 — X축은 그대로 두고 Y축만 뒤집으면 시계 방향과 반시계 방향이 서로
// 바뀐다). 이 보정이 빠져 있으면, 캔버스에서 드래그로 잡은 화살표 방향과 실제 로봇이
// 그 지점에 도착했을 때 보는 방향이 서로 다른 경우가 생긴다 — 다만 0°/180°(정확히
// 좌우를 향할 때)는 뒤집어도 그대로라 우연히 맞아 보이고, 90°/270°처럼 위아래를
// 향할수록 차이가 가장 크게(반대 방향으로) 나타난다(실사용에서 발견: 충전
// 스테이션(yaw=0)은 정상으로 보이는데 다른 웨이포인트(yaw≈90°)는 로봇이 정반대를
// 보고 멈춤, 2026-09-08). 캔버스 yaw(도) <-> map 프레임 yaw(라디안)를 주고받는 모든
// 지점(웨이포인트 저장/전송, 위치 보정 전송, 서버에서 불러올 때, 로봇 실시간 위치
// 표시)이 반드시 이 함수들을 거쳐야 한다.
qreal canvasYawDegToMapYawRad(qreal canvasYawDeg);
qreal mapYawRadToCanvasYawDeg(qreal mapYawRad);

}  // namespace CanvasGeometry
