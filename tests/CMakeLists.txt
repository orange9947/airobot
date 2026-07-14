add_executable(test_smoke unit/test_smoke.c)
target_include_directories(test_smoke PRIVATE support)
robot_apply_warnings(test_smoke)
add_test(NAME smoke COMMAND test_smoke)

add_library(robot_protocol_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Protocol/robot_crc16.c
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Protocol/robot_spi_slot.c
)
target_include_directories(robot_protocol_host PUBLIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Protocol
  ${PROJECT_SOURCE_DIR}/protocol/generated
)
robot_apply_warnings(robot_protocol_host)

add_executable(test_protocol_c unit/test_protocol_c.c)
target_include_directories(test_protocol_c PRIVATE support)
target_link_libraries(test_protocol_c PRIVATE robot_protocol_host)
robot_apply_warnings(test_protocol_c)
add_test(NAME protocol_c COMMAND test_protocol_c)

add_library(robot_core_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Core/robot_state.c
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Safety/safety_supervisor.c
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Motion/motion_service.c
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Input/ec11.c
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Input/input_policy.c
)
target_include_directories(robot_core_host PUBLIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Core
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Safety
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Motion
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Input
)
robot_apply_warnings(robot_core_host)

add_executable(test_robot_core unit/test_robot_core.c)
target_include_directories(test_robot_core PRIVATE support)
target_link_libraries(test_robot_core PRIVATE robot_core_host)
robot_apply_warnings(test_robot_core)
add_test(NAME robot_core COMMAND test_robot_core)

add_library(face_animator_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Ui/face_animator.c
)
target_include_directories(face_animator_host PUBLIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Ui
  ${PROJECT_SOURCE_DIR}/protocol/generated
)
robot_apply_warnings(face_animator_host)

add_executable(test_face_animator unit/test_face_animator.c)
target_include_directories(test_face_animator PRIVATE support)
target_link_libraries(test_face_animator PRIVATE face_animator_host)
robot_apply_warnings(test_face_animator)
add_test(NAME face_animator COMMAND test_face_animator)

add_library(status_label_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Ui/status_label.c
)
target_include_directories(status_label_host PUBLIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Ui
  ${PROJECT_SOURCE_DIR}/protocol/generated
)
robot_apply_warnings(status_label_host)

add_executable(test_status_label unit/test_status_label.c)
target_include_directories(test_status_label PRIVATE support)
target_link_libraries(test_status_label PRIVATE status_label_host)
robot_apply_warnings(test_status_label)
add_test(NAME status_label COMMAND test_status_label)
