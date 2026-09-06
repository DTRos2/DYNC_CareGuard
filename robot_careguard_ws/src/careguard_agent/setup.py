from setuptools import find_packages, setup

package_name = 'careguard_agent'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
    ],
    install_requires=['setuptools', 'websockets', 'requests', 'numpy'],
    zip_safe=True,
    maintainer='CareGuard Team',
    maintainer_email='careguard@example.com',
    description='ROS2 <-> FastAPI 브리지 (uplink/downlink/media_uploader)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'uplink_node = careguard_agent.uplink_node:main',
            'downlink_node = careguard_agent.downlink_node:main',
        ],
    },
)
