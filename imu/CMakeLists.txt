cmake_minimum_required(VERSION 2.8.3)
project(imu)
find_package(catkin_simple REQUIRED)
catkin_simple()

cs_add_executable(imu
  src/prog/imu.cpp)
target_link_libraries(imu)


