cmake_minimum_required(VERSION 3.20)
project(ai_desktop_robot LANGUAGES C)

option(ROBOT_BUILD_HOST_TESTS "Build host-side unit tests" ON)
option(ROBOT_BUILD_STM32 "Build STM32 firmware" OFF)
option(ROBOT_ENABLE_SANITIZERS "Enable host sanitizers" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RobotWarnings)

if(ROBOT_BUILD_HOST_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ROBOT_BUILD_STM32)
  add_subdirectory(firmware/stm32)
endif()
