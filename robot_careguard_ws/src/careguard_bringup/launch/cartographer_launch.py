# 출처: WeGo Robotics limo_pro(wego 패키지) launch/cartographer_launch.py (Apache-2.0).
# wego_ws를 참조하지 않고 이 저장소 안에서 자체 완결되도록 careguard_bringup
# 패키지로 그대로 옮겨왔다 — 로직은 원본과 동일, 공유 디렉터리 이름만
# 'wego' -> 'careguard_bringup'으로 바꿨다. 라이선스 원문:
# THIRD_PARTY_LICENSE_wego.Apache-2.0.txt 참고.
import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch_ros.actions import Node
from launch.substitutions import LaunchConfiguration
from launch.actions import IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import ThisLaunchFileDir


def generate_launch_description():
    bringup_share_dir = get_package_share_directory('careguard_bringup')
    cartographer_config_dir = LaunchConfiguration('cartographer_config_dir',
                                    default=os.path.join(bringup_share_dir, 'config'))
    configuration_file = LaunchConfiguration('configuration_file', default='limo_lds_2d.lua')

    resolution = LaunchConfiguration('resolution', default='0.05')
    publish_period_sec = LaunchConfiguration('publish_period_sec', default='1.0')

    rviz_config_dir = os.path.join(bringup_share_dir, 'rviz', 'cartographer.rviz')

    return LaunchDescription([
        DeclareLaunchArgument(
            'cartographer_config_dir',
            default_value=cartographer_config_dir,
            description='Full path to config file to load'),
        DeclareLaunchArgument(
            'configuration_file',
            default_value=configuration_file,
            description='Name of lua file for cartographer'),
        Node(
            package='cartographer_ros',
            executable='cartographer_node',
            name='cartographer_node',
            remappings=[('odom','odometry/filtered'),],
            output='screen',
            arguments=['-configuration_directory', cartographer_config_dir,
                       '-configuration_basename', configuration_file]),

        DeclareLaunchArgument(
            'resolution',
            default_value=resolution,
            description='Resolution of a grid cell in the published occupancy grid'),

        DeclareLaunchArgument(
            'publish_period_sec',
            default_value=publish_period_sec,
            description='OccupancyGrid publishing period'),

        IncludeLaunchDescription(
            PythonLaunchDescriptionSource([ThisLaunchFileDir(), '/occupancy_grid_launch.py']),
            launch_arguments={'resolution': resolution,
                              'publish_period_sec': publish_period_sec}.items(),
        ),

        Node(
            package='rviz2',
            executable='rviz2',
            name='rviz2',
            arguments=['-d', rviz_config_dir],
            output='screen'),

        # 대시보드 P5 매핑 카드의 "경과·탐색면적" 표시용 — cartographer_node와 같은
        # 프로세스 그룹으로 떠서, downlink_node._stop_slam()이 그룹 전체를 죽일 때
        # 같이 종료된다(매핑 재시작 = 경과·면적도 자동으로 0부터 다시 시작).
        Node(
            package='careguard_mission',
            executable='slam_progress_publisher',
            name='slam_progress_publisher',
            output='screen'),
    ])