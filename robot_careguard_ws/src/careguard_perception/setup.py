from setuptools import find_packages, setup

package_name = 'careguard_perception'

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
    description='인지 계층 (이벤트 영상 녹화·침입자 검출)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'video_recorder = careguard_perception.video_recorder:main',
        ],
    },
)
