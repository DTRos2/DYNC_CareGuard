from setuptools import find_packages, setup

package_name = 'careguard_nav'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
    ],
    install_requires=['setuptools', 'requests'],
    zip_safe=True,
    maintainer='CareGuard Team',
    maintainer_email='careguard@example.com',
    description='주행 계층 (웨이포인트 관리·초기 위치·충전소 복귀)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'initial_pose_setter = careguard_nav.initial_pose_setter:main',
        ],
    },
)
