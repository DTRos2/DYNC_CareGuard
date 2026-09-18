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

        # rviz2는 여기서 안 띄운다 — 이 로봇(헤드리스 Jetson)엔 디스플레이가 없어
        # rviz2가 뜨자마자 SIGABRT로 죽고(2026-09-18 R-07 실기 확인), 비정상 종료된
        # 프로세스가 남긴 상태 때문에 같은 도메인의 다른 노드(/scan, 카메라 등)까지
        # discovery/데이터 수신이 막히는 문제로 이어졌다. rviz2는 시각화 전용이라
        # cartographer_node의 실제 매핑 기능에는 필요 없다(navigation_diff_launch.py와
        # 같은 이유·같은 조치).

        # 대시보드 P5 매핑 카드의 "경과·탐색면적" 표시용 — cartographer_node와 같은
        # 프로세스 그룹으로 떠서, downlink_node._stop_slam()이 그룹 전체를 죽일 때
        # 같이 종료된다(매핑 재시작 = 경과·면적도 자동으로 0부터 다시 시작).
        Node(
            package='careguard_mission',
            executable='slam_progress_publisher',
            name='slam_progress_publisher',
            output='screen'),
    ])