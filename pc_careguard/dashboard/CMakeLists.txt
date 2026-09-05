cmake_minimum_required(VERSION 3.16)
project(careguard_dashboard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets SvgWidgets Multimedia MultimediaWidgets Network)

add_executable(careguard_dashboard
    src/main.cpp
    src/windows/mainwindow.h
    src/windows/mainwindow.cpp
    src/windows/loginwindow.h
    src/windows/loginwindow.cpp
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
    src/dialogs/event_detail_modal.h
    src/dialogs/event_detail_modal.cpp
    src/dialogs/map_file_register_dialog.h
    src/dialogs/map_file_register_dialog.cpp
    src/dialogs/map_preview_dialog.h
    src/dialogs/map_preview_dialog.cpp
    src/dialogs/p7_robot_dialog.h
    src/dialogs/p7_robot_dialog.cpp
    src/dialogs/p7_account_dialog.h
    src/dialogs/p7_account_dialog.cpp
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
    resources/resources.qrc
)

target_include_directories(careguard_dashboard PRIVATE src)
target_link_libraries(careguard_dashboard PRIVATE Qt6::Widgets Qt6::SvgWidgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Network)

# 대시보드 실행 시 FastAPI 서버를 자동 기동하기 위한 기본 경로.
# 환경변수 CAREGUARD_SERVER_DIR로 재정의 가능 (main.cpp 참고).
target_compile_definitions(careguard_dashboard PRIVATE
    CAREGUARD_SERVER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../server")
