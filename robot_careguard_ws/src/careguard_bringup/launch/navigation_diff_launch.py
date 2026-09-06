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

from launch.actions import IncludeLaunchDescription
from launch.substitutions import PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare

from launch_ros.descriptions import ParameterFile
from nav2_common.launch import RewrittenYaml

def generate_launch_description():
    bringup_share_dir = get_package_share_directory('careguard_bringup')

    # setting for rviz configuration path
    rviz_file_name = 'navigation.rviz'
    rviz_config_path = os.path.join(bringup_share_dir, 'rviz', rviz_file_name)

    # set the parameters
    parameter_file_name = 'diff_navigation_params.yaml'
    parameter_file_path = os.path.join(bringup_share_dir, 'params', parameter_file_name)

    # set the map yaml file — 기본값은 이 로봇이 실제로 SLAM해서 만든 맵 중 하나.
    # 실제 순찰 실행 시엔 DB에 등록된 로봇별 맵으로 `map:=` 인자를 덮어써야 한다.
    map_file_name = 'mapd_origin.yaml'
    map_file_path = os.path.join(bringup_share_dir, 'maps', map_file_name)

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

    # setting for rviz
    rviz_config_node = Node(
            package='rviz2',
            executable='rviz2',
            name='rviz2',
            output='screen',
            arguments=['-d', rviz_config_path],
        )

    return LaunchDescription([
        nav2_container,
        localization_launch,
        navigation_launch,
        rviz_config_node,
    ])