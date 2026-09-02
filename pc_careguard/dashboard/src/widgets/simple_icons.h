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

// 사이드바 메뉴 아이콘 (p1_fleet_overview_wireframe_v3 ti-* 아이콘 대응).
QIcon dashboard(const QColor& color, int px = 16);   // 통합 관제 (ti-layout-dashboard)
QIcon bell(const QColor& color, int px = 16);        // 이벤트 (ti-bell)
QIcon heart(const QColor& color, int px = 16);       // 대상자 (ti-user-heart)
QIcon video(const QColor& color, int px = 16);       // 영상 (ti-video)
QIcon mapPin(const QColor& color, int px = 16);      // 맵·로봇 (ti-map)
QIcon route(const QColor& color, int px = 16);       // 순찰·경로 (ti-route)
QIcon settings(const QColor& color, int px = 16);    // 시스템 (ti-settings)
QIcon logout(const QColor& color, int px = 16);      // 로그아웃 (ti-logout)
QIcon power(const QColor& color, int px = 16);       // 종료 (ti-power)

// P4 영상 관리 (p4_video_management_wireframe 대응).
QIcon play(const QColor& color, int px = 16);         // 썸네일 재생 (ti-player-play)
QIcon list(const QColor& color, int px = 16);         // 테이블 뷰 토글 (ti-list)

// P4 재생 모달 (p4_video_playback_modal_wireframe 대응).
QIcon close(const QColor& color, int px = 16);        // 모달 닫기 (ti-x)
QIcon trackPrev(const QColor& color, int px = 16);    // 이전 구간 (ti-player-track-prev)
QIcon trackNext(const QColor& color, int px = 16);    // 다음 구간 (ti-player-track-next)
QIcon download(const QColor& color, int px = 16);     // 다운로드 (ti-download)
QIcon share(const QColor& color, int px = 16);        // 공유 링크 (ti-share)
QIcon trash(const QColor& color, int px = 16);        // 즉시 삭제 (ti-trash)
QIcon expand(const QColor& color, int px = 16);       // 전체화면 진입 (ti-maximize)
QIcon collapse(const QColor& color, int px = 16);     // 전체화면 종료 (ti-minimize)

}  // namespace SimpleIcons
