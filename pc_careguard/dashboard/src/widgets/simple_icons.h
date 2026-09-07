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
QIcon pause(const QColor& color, int px = 16);        // 재생 모달 하단 일시정지 (ti-player-pause)
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

// P5(맵·로봇 매핑) / P6(순찰·경로) 와이어프레임 대응 아이콘.
// play/trash는 위 P4 선언과 같은 이름으로 겹쳐서(§merge) 여기서는 뺐다 — 위 선언을 그대로 쓴다.
QIcon robot(const QColor& color, int px = 16);           // ti-robot
QIcon chevronDown(const QColor& color, int px = 16);     // ti-chevron-down
QIcon chevronRight(const QColor& color, int px = 16);    // ti-chevron-right
QIcon upload(const QColor& color, int px = 16);          // ti-upload
QIcon radar(const QColor& color, int px = 16);           // ti-radar-2
QIcon arrowUp(const QColor& color, int px = 16);         // ti-arrow-up
QIcon arrowDown(const QColor& color, int px = 16);       // ti-arrow-down
QIcon arrowLeft(const QColor& color, int px = 16);       // ti-arrow-left
QIcon arrowRight(const QColor& color, int px = 16);      // ti-arrow-right
QIcon deviceFloppy(const QColor& color, int px = 16);    // ti-device-floppy
QIcon x(const QColor& color, int px = 16);               // ti-x
QIcon fileCheck(const QColor& color, int px = 16);       // ti-file-check
QIcon fileUpload(const QColor& color, int px = 16);      // ti-file-upload
QIcon alertTriangle(const QColor& color, int px = 16);   // ti-alert-triangle
QIcon infoCircle(const QColor& color, int px = 16);      // ti-info-circle
// play/trash는 위 P4 선언(30~40행)과 이름이 겹쳐서 여기서는 빼고 그 선언을 그대로 쓴다.
QIcon bolt(const QColor& color, int px = 16);            // ti-bolt — 충전 스테이션 유형 표시

// P2(이벤트 관제) 와이어프레임 대응 아이콘.
QIcon alertOctagon(const QColor& color, int px = 16);    // 긴급 배지 (ti-alert-octagon)
QIcon chevronLeft(const QColor& color, int px = 16);     // 페이지네이션 이전 (ti-chevron-left)
QIcon barChart(const QColor& color, int px = 16);        // 통계 버튼 (ti-chart-bar)
QIcon file(const QColor& color, int px = 16);             // PDF 보고서 버튼 (ti-file)

// P2 이벤트 상세 모달 조치 버튼 (gui/wireframe/v1/p2_event_detail_modal_wireframe.html).
QIcon speakerphone(const QColor& color, int px = 16);     // 스피커 통화 (ti-speakerphone)
QIcon phone(const QColor& color, int px = 16);            // 대상자 전화 (ti-phone)
QIcon users(const QColor& color, int px = 16);            // 보호자 연락 (ti-users)
QIcon message(const QColor& color, int px = 16);          // 보호자 문자 (ti-message)
QIcon phoneCall(const QColor& color, int px = 16);        // 119 모의 신고 (ti-phone-call)

// P3(대상자 관리) 와이어프레임 대응 아이콘.
QIcon plus(const QColor& color, int px = 16);             // 대상자 등록 버튼 (ti-plus)
QIcon edit(const QColor& color, int px = 16);             // 보호자·복약 스케줄 행 편집 (ti-edit)

}  // namespace SimpleIcons
