cmake_minimum_required(VERSION 3.8)
project(careguard_bringup)

find_package(ament_cmake REQUIRED)

install(DIRECTORY launch config rviz params maps DESTINATION share/${PROJECT_NAME})

ament_package()
