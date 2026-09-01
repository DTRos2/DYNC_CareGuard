cmake_minimum_required(VERSION 3.16)
project(careguard_dashboard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets SvgWidgets)

add_executable(careguard_dashboard
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/loginwindow.h
    src/loginwindow.cpp
    src/theme/tokens.h
    src/widgets/simple_icons.h
    src/widgets/simple_icons.cpp
    src/widgets/icon_line_edit.h
    src/widgets/icon_line_edit.cpp
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
target_link_libraries(careguard_dashboard PRIVATE Qt6::Widgets Qt6::SvgWidgets)
