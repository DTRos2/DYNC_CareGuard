# 출처: WeGo Robotics limo_pro(wego 패키지) launch/teleop_launch.py (Apache-2.0).
# 프로젝트 요구사항상 wego_ws를 참조하지 않고 이 저장소 안에서 자체 완결되도록
# careguard_bringup 패키지로 그대로 옮겨왔다 — 로직은 원본과 동일, 참조하는
# 공유 디렉터리 이름만 'wego' -> 'careguard_bringup'으로 바꿨다.
# 라이선스 원문: THIRD_PARTY_LICENSE_wego.Apache-2.0.txt 참고.
#
# 카메라 패키지 이름은 원본(limo_pro)의 'astra_camera'가 아니라 'orbbec_camera'로
# 바꿨다 — 이 로봇(2026-09-06 실기 확인)에는 astra_camera가 없고 orbbec_camera만
# 설치되어 있다(같은 이름의 launch 파일 dabai.launch.py는 그대로 존재).
import launch

from launch import LaunchDescription

from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare
from launch.actions import IncludeLaunchDescription, DeclareLaunchArgument
from launch.substitutions import PathJoinSubstitution, LaunchConfiguration, TextSubstitution, PythonExpression

from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.conditions import IfCondition

from ament_index_python.packages import get_package_share_directory
import os

def generate_launch_description():
    bringup_share_dir = get_package_share_directory('careguard_bringup')
    rviz_config_path = os.path.join(bringup_share_dir, 'rviz', 'display.rviz')

    degree = LaunchConfiguration('degree')
    degree_launch_arg = DeclareLaunchArgument(
        'degree',
        default_value='0.0'
    )

    viz_launch_arg = DeclareLaunchArgument(
        'viz',
        default_value='false'
    )

    return launch.LaunchDescription([
        degree_launch_arg,
        viz_launch_arg,

        IncludeLaunchDescription(
            PathJoinSubstitution([FindPackageShare('limo_description'), 'launch', 'load_urdf.launch.py'])
        ),
        IncludeLaunchDescription(
            PythonLaunchDescriptionSource([
                PathJoinSubstitution([
                    FindPackageShare('careguard_bringup'),
                    'launch',
                    'camera_tilt_launch.py'
                    ])
            ]),
            launch_arguments={
                'degree': degree
            }.items()
        ),
        IncludeLaunchDescription(
            PathJoinSubstitution([FindPackageShare('limo_base'), 'launch', 'limo_base.launch.py'])
        ),
        IncludeLaunchDescription(
            PathJoinSubstitution([FindPackageShare('orbbec_camera'), 'launch', 'dabai.launch.py'])
        ),
        IncludeLaunchDescription(
            PathJoinSubstitution([FindPackageShare('ydlidar_ros2_driver'), 'launch', 'ydlidar.launch.py'])
        ),
        IncludeLaunchDescription(
            PathJoinSubstitution([FindPackageShare('robot_localization'), 'launch', 'limo_ekf_launch.py'])
        ),

        Node(
            package='rviz2',
            executable='rviz2',
            name='rviz2',
            output='screen',
            condition=IfCondition(LaunchConfiguration('viz')),
            arguments=['-d', rviz_config_path]
        )
  ])