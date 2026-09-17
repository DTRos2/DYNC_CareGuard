cmake_minimum_required(VERSION 3.16)
project(careguard_dashboard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets SvgWidgets Multimedia MultimediaWidgets Network WebSockets)

# 통화 기능(call_audio_client)의 Opus 인코딩/디코딩용 — LIMO(careguard_hmi/
# audio_capture_node.py, audio_downlink_player.py)와 같은 코덱을 써야 서로 재생할 수
# 있다. 빌드 전에 `sudo apt install libopus-dev`가 필요하다(런타임 libopus0만으로는
# 헤더/링크 대상이 없어 컴파일이 안 된다).
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

add_executable(careguard_dashboard
    src/main.cpp
    src/windows/mainwindow.h
    src/windows/mainwindow.cpp
    src/windows/loginwindow.h
    src/windows/loginwindow.cpp
    src/net/api_client.h
    src/net/api_client.cpp
    src/net/ws_client.h
    src/net/ws_client.cpp
    src/net/server_config.h
    src/net/server_config.cpp
    src/audio/call_audio_client.h
    src/audio/call_audio_client.cpp
    src/theme/tokens.h
    src/theme/calendar_style.h
    src/widgets/simple_icons.h
    src/widgets/simple_icons.cpp
    src/widgets/icon_line_edit.h
    src/widgets/icon_line_edit.cpp
    src/widgets/splashscreenwidget.h
    src/widgets/splashscreenwidget.cpp
    src/widgets/splashscreencontentwidget.h
    src/widgets/splashscreencontentwidget.cpp
    src/widgets/splashassetresolver.h
    src/widgets/splashassetresolver.cpp
    src/widgets/canvas_geometry.h
    src/widgets/canvas_geometry.cpp
    src/widgets/slam_mapping_card.h
    src/widgets/slam_mapping_card.cpp
    src/widgets/teleop_panel.h
    src/widgets/teleop_panel.cpp
    src/widgets/mapping_status_overlay.h
    src/widgets/mapping_status_overlay.cpp
    src/widgets/pgm_image_loader.h
    src/widgets/pgm_image_loader.cpp
    src/widgets/toggle_switch.h
    src/widgets/toggle_switch.cpp
    src/widgets/yaw_dial.h
    src/widgets/yaw_dial.cpp
    src/widgets/camera_live_view.h
    src/widgets/camera_live_view.cpp
    src/widgets/image_zoom_view.h
    src/widgets/image_zoom_view.cpp
    src/widgets/waypoint_canvas.h
    src/widgets/waypoint_canvas.cpp
    src/widgets/waypoint_popover.h
    src/widgets/waypoint_popover.cpp
    src/widgets/event_table_model.h
    src/widgets/event_table_model.cpp
    src/widgets/event_filter_proxy_model.h
    src/widgets/event_filter_proxy_model.cpp
    src/widgets/event_row_delegate.h
    src/widgets/event_row_delegate.cpp
    src/widgets/live_search_line_edit.h
    src/widgets/live_search_line_edit.cpp
    src/widgets/careguard_date_edit.h
    src/widgets/careguard_date_edit.cpp
    src/widgets/careguard_combo_box.h
    src/widgets/careguard_combo_box.cpp
    src/widgets/checkable_list_widget.h
    src/widgets/checkable_list_widget.cpp
    src/dialogs/event_detail_modal.h
    src/dialogs/event_detail_modal.cpp
    src/dialogs/guardian_mail_dialog.h
    src/dialogs/guardian_mail_dialog.cpp
    src/dialogs/robot_control_modal.h
    src/dialogs/robot_control_modal.cpp
    src/dialogs/p5_map_file_register_dialog.h
    src/dialogs/p5_map_file_register_dialog.cpp
    src/dialogs/p5_map_preview_dialog.h
    src/dialogs/p5_map_preview_dialog.cpp
    src/dialogs/p7_robot_dialog.h
    src/dialogs/p7_robot_dialog.cpp
    src/dialogs/p7_account_dialog.h
    src/dialogs/p7_account_dialog.cpp
    src/dialogs/p3_recipient_detail_dialog.h
    src/dialogs/p3_recipient_detail_dialog.cpp
    src/dialogs/p3_recipient_create_dialog.h
    src/dialogs/p3_recipient_create_dialog.cpp
    src/pages/p1_overview_page.h
    src/pages/p1_overview_page.cpp
    src/pages/p2_event_page.h
    src/pages/p2_event_page.cpp
    src/pages/p3_recipient_page.h
    src/pages/p3_recipient_page.cpp
    src/pages/p4_video_page.h
    src/pages/p4_video_page.cpp
    src/pages/p5_map_page.h
    src/pages/p5_map_page.cpp
    src/pages/p6_patrol_page.h
    src/pages/p6_patrol_page.cpp
    src/pages/p7_settings_page.h
    src/pages/p7_settings_page.cpp
    src/pages/p8_logs_page.h
    src/pages/p8_logs_page.cpp
    resources/resources.qrc
)

target_include_directories(careguard_dashboard PRIVATE src)
target_link_libraries(careguard_dashboard PRIVATE Qt6::Widgets Qt6::SvgWidgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Network Qt6::WebSockets PkgConfig::OPUS)

# 대시보드 실행 시 FastAPI 서버를 자동 기동하기 위한 기본 경로.
# 환경변수 CAREGUARD_SERVER_DIR로 재정의 가능 (main.cpp 참고).
# 파일로 맵 등록할 때 .pgm/.yaml 파일 선택 다이얼로그가 처음 열리는 기본 경로.
# 환경변수 CAREGUARD_MAPS_DIR로 재정의 가능 (p5_map_file_register_dialog.cpp 참고).
# 팀원마다 다른 서버(PC)를 가리켜야 할 때 쓰는 .env(CAREGUARD_SERVER_URL=...) 위치.
# 빌드 디렉터리가 아니라 소스 디렉터리를 가리켜야 rm -rf build를 해도 설정이 안 날아간다.
# 환경변수 CAREGUARD_DASHBOARD_DIR로 재정의 가능 (net/server_config.cpp 참고).
target_compile_definitions(careguard_dashboard PRIVATE
    CAREGUARD_SERVER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../server"
    CAREGUARD_MAPS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../robot_careguard_ws/src/careguard_bringup/maps"
    CAREGUARD_DASHBOARD_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
