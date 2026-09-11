from setuptools import find_packages, setup

package_name = 'careguard_hw'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
    ],
    install_requires=['setuptools', 'pyserial'],
    zip_safe=True,
    maintainer='CareGuard Team',
    maintainer_email='careguard@example.com',
    description='로봇 부착 하드웨어 드라이버 계층 (약통 서보·IR 센서)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'pillbox_controller = careguard_hw.pillbox_controller:main',
        ],
    },
)
