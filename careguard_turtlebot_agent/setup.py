from setuptools import find_packages, setup

package_name = 'careguard_turtlebot_agent'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
        ('share/' + package_name + '/config', ['config/nav2_burger_params.yaml']),
    ],
    install_requires=['setuptools', 'websockets', 'requests', 'pyyaml'],
    zip_safe=True,
    maintainer='postwo',
    maintainer_email='ajtwlscldjs@gmail.com',
    description='TurtleBot <-> CareGuard 서버 업링크/다운링크',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'uplink_node = careguard_turtlebot_agent.uplink_node:main',
            'downlink_node = careguard_turtlebot_agent.downlink_node:main',
            'face_capture_node = careguard_turtlebot_agent.perception.face_capture_node:main',
            'photo_identify_node = careguard_turtlebot_agent.perception.photo_identify_node:main',
            'video_recorder = careguard_turtlebot_agent.perception.video_recorder:main',
        ],
    },
)
