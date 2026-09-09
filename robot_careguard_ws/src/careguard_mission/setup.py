from setuptools import find_packages, setup

package_name = 'careguard_mission'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='CareGuard Team',
    maintainer_email='careguard@example.com',
    description='시나리오 실행 엔진 (순찰 액션 서버)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'patrol_task = careguard_mission.patrol_task:main',
            'mission_scheduler = careguard_mission.mission_scheduler:main',
            'slam_progress_publisher = careguard_mission.slam_progress_publisher:main',
        ],
    },
)
