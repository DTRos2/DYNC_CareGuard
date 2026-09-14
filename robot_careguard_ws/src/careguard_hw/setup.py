from setuptools import find_packages, setup

package_name = 'careguard_hw'

setup(
    name=package_name,
    # find_packages()는 firmware/(아두이노 .ino, 파이썬 패키지 아님)는 무시하고
    # careguard_hw/(이 파이썬 패키지)만 잡는다.
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
    description='하드웨어 계층 (약통 서보·응급 호출 버튼 등 아두이노/ESP32 브리지)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            # pillbox_controller.py가 생기면 여기에 등록한다.
        ],
    },
)
