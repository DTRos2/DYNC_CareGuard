from setuptools import find_packages, setup

package_name = 'careguard_hmi'

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
    description='HMI 계층 (LCD 표정 · LED · 스피커 · 통화)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'audio_player = careguard_hmi.audio_player:main',
        ],
    },
)
