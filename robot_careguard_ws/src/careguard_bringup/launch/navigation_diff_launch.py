# 출처: WeGo Robotics limo_pro의 wego/launch/navigation_diff_launch.py +
# wego_2d_nav 패키지(둘 다 Apache-2.0). wego_ws를 참조하지 않고 이 저장소 안에서
# 자체 완결되도록 careguard_bringup 하나로 합쳐 옮겨왔다 — 로직은 원본과 동일,
# 공유 디렉터리 참조만 'wego'/'wego_2d_nav' -> 'careguard_bringup'으로 바꿨다.
# 기본 맵 경로도 wego_2d_nav의 예시 맵 대신 이 로봇이 실제 SLAM으로 만든 맵으로
# 바꿨다 — 실제 순찰 시에는 어느 로봇의 어느 맵인지 DB로 결정되므로, 그때는
# `map:=<등록된 맵 yaml 경로>` 인자로 덮어써야 한다. 라이선스 원문:
# THIRD_PARTY_LICENSE_wego.Apache-2.0.txt 참고.
import os
from launch import LaunchDescription
from launch_ros.actions import Node
from ament_index_python.packages import get_package_share_directory

from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare

from launch_ros.descriptions import ParameterFile
from nav2_common.launch import RewrittenYaml

def generate_launch_description():
    bringup_share_dir = get_package_share_directory('careguard_bringup')

    # set the parameters
    parameter_file_name = 'diff_navigation_params.yaml'
    parameter_file_path = os.path.join(bringup_share_dir, 'params', parameter_file_name)

    # set the map yaml file — 기본값은 이 로봇이 실제로 SLAM해서 만든 맵 중 하나.
    # 실제 순찰 실행 시엔 DB에 등록된 로봇별 맵으로 `map:=` 인자를 덮어써야 한다.
    #
    # 예전엔 이 값이 그냥 파이썬 지역 변수라, `ros2 launch ... map:=<경로>`로
    # 실행해도 ros2 launch가 그런 이름의 인자를 아예 모르고(DeclareLaunchArgument가
    # 없었음) 조용히 무시했다 — 그래서 실제로는 항상 이 기본값(mapd_origin.yaml)만
    # 쓰였을 것이다. downlink_node.py가 로봇/맵별로 다운로드한 파일을 map:=으로
    # 넘겨서 순찰을 자동화하려는데 이게 안 먹히면 의미가 없어서 DeclareLaunchArgument로
    # 실제로 받게 고쳤다.
    map_arg = DeclareLaunchArgument(
        'map',
        default_value=os.path.join(bringup_share_dir, 'maps', 'mapd_origin.yaml'),
        description='Nav2가 쓸 맵 yaml 경로',
    )
    map_file_path = LaunchConfiguration('map')

    # remapping tf topic
    remappings = [('/tf', 'tf'), ('/tf_static', 'tf_static')]

    # set container for composable node
    nav2_container = Node(
        name='nav2_container',
        package='rclcpp_components',
        executable='component_container_isolated',
        parameters=[ParameterFile(parameter_file_path), {'autostart': True}],
        arguments=['--ros-args', '--log-level', 'info'],
        remappings=remappings,
        output='screen',
    )

    # For localization
    localization_launch = IncludeLaunchDescription(
        PathJoinSubstitution([
            FindPackageShare('careguard_bringup'),
            'launch',
            'localization_launch.py',
        ]),
        launch_arguments={
            'map' : map_file_path,
            'params_file': parameter_file_path
        }.items()
    )

    # For navigation
    navigation_launch = IncludeLaunchDescription(
        PathJoinSubstitution([
            FindPackageShare('careguard_bringup'),
            'launch',
            'navigation_only_launch.py',
        ]),
        launch_arguments={
            'params_file': parameter_file_path
        }.items()
    )

    # rviz2는 여기서 안 띄운다 — 이 로봇(헤드리스 Jetson)엔 디스플레이가 없어
    # rviz2가 뜨자마자 SIGABRT로 죽었고(2026-09-18 R-07 실기 확인), 비정상 종료된
    # 프로세스가 남긴 상태 때문에 같은 도메인의 다른 노드들(/scan, 카메라 등)까지
    # discovery/데이터 수신이 막히는 문제로 이어졌다. rviz2는 시각화 전용(구독만
    # 하고 아무것도 publish 안 함)이라 AMCL/costmap/planner 등 실제 내비게이션
    # 기능에는 원래 필요 없다.
    return LaunchDescription([
        map_arg,
        nav2_container,
        localization_launch,
        navigation_launch,
    ])
