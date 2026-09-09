from setuptools import find_packages, setup

package_name = 'careguard_turtlebot_agent'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
    ],
    install_requires=['setuptools', 'websockets'],
    zip_safe=True,
    maintainer='postwo',
    maintainer_email='ajtwlscldjs@gmail.com',
    description='TurtleBot -> CareGuard 서버 최소 업링크 (온라인 상태만)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'uplink_node = careguard_turtlebot_agent.uplink_node:main',
        ],
    },
)
