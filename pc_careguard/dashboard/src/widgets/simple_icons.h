#pragma once
#include <QColor>
#include <QIcon>

// gui/wireframe/CareGuard_DESIGN_SYSTEM.md §8: 최종 아이콘 세트는 Lucide/Tabler SVG(resources/icons/)로
// 교체 예정이다. 아직 아이콘 자산이 없어서, 그 전까지 쓰는 QPainter 기반 최소 선화 아이콘 모음.
// 로그인 화면(p0_login_final_wireframe)의 필드·상태 배너 아이콘에 사용한다.
namespace SimpleIcons {

QIcon user(const QColor& color, int px = 16);
QIcon lock(const QColor& color, int px = 16);
QIcon eye(const QColor& color, int px = 16);
QIcon eyeOff(const QColor& color, int px = 16);
QIcon alertCircle(const QColor& color, int px = 16);
QIcon lockExclamation(const QColor& color, int px = 16);
QIcon plugOff(const QColor& color, int px = 16);
QIcon refresh(const QColor& color, int px = 16);

}  // namespace SimpleIcons
