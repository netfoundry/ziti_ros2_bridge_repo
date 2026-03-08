cmake_minimum_required(VERSION 3.8)
project(ziti_ros2_bridge)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(PkgConfig REQUIRED)

# 1. Look for RapidJSON (Header-only)
# On Ubuntu: sudo apt-get install rapidjson-dev
find_package(RapidJSON REQUIRED)

# 2. Find system libraries for Ziti/Networking
pkg_check_modules(UV REQUIRED libuv)
pkg_check_modules(OPENSSL REQUIRED openssl)

include_directories(
  include
  /usr/local/include
  ${UV_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIRS}
  ${RapidJSON_INCLUDE_DIRS}
)

add_executable(ziti_bridge_node src/ziti_bridge_node.cpp)

# 3. Link Libraries 
# Note: We kept the --no-as-needed to ensure Ziti symbols are resolved correctly
target_link_libraries(ziti_bridge_node
  "-Wl,--no-as-needed"
  ziti
  tlsuv
  ${UV_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  "-Wl,--as-needed"
  pthread
  dl
)

ament_target_dependencies(ziti_bridge_node
  rclcpp
  geometry_msgs
  sensor_msgs
)

install(TARGETS ziti_bridge_node DESTINATION lib/${PROJECT_NAME})
ament_package()
