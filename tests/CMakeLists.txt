add_executable(test_smoke unit/test_smoke.c)
target_include_directories(test_smoke PRIVATE support)
robot_apply_warnings(test_smoke)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_w25q_addressing unit/test_w25q_addressing.c)
target_include_directories(test_w25q_addressing PRIVATE
  support
  ${PROJECT_SOURCE_DIR}/firmware/stm32/Drivers/Storage
)
robot_apply_warnings(test_w25q_addressing)
add_test(NAME w25q_addressing COMMAND test_w25q_addressing)

add_library(robot_protocol_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Protocol/controller_session_policy.c
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
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Core/session_epoch.c
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

add_library(resource_format_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage/resource_crc32.c
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage/resource_format.c
)
target_include_directories(resource_format_host PUBLIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage
)
robot_apply_warnings(resource_format_host)

add_executable(test_resource_format unit/test_resource_format.c)
target_include_directories(test_resource_format PRIVATE support)
target_link_libraries(test_resource_format PRIVATE resource_format_host)
robot_apply_warnings(test_resource_format)
add_test(NAME resource_format COMMAND test_resource_format)

add_library(storage_service_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage/storage_service.c
)
target_include_directories(storage_service_host PUBLIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage
)
target_link_libraries(storage_service_host PUBLIC resource_format_host)
robot_apply_warnings(storage_service_host)

add_executable(test_storage_service unit/test_storage_service.c)
target_include_directories(test_storage_service PRIVATE support)
target_link_libraries(test_storage_service PRIVATE storage_service_host)
robot_apply_warnings(test_storage_service)
add_test(NAME storage_service COMMAND test_storage_service)

add_library(resource_policy_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage/resource_policy.c
)
target_include_directories(resource_policy_host PUBLIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Core
)
robot_apply_warnings(resource_policy_host)

add_executable(test_resource_policy unit/test_resource_policy.c)
target_include_directories(test_resource_policy PRIVATE support)
target_link_libraries(test_resource_policy PRIVATE resource_policy_host)
robot_apply_warnings(test_resource_policy)
add_test(NAME resource_policy COMMAND test_resource_policy)

add_library(face_resource_provider_host STATIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage/face_resource_provider.c
)
target_include_directories(face_resource_provider_host PUBLIC
  ${PROJECT_SOURCE_DIR}/firmware/stm32/App/Storage
)
target_link_libraries(face_resource_provider_host PUBLIC resource_format_host)
robot_apply_warnings(face_resource_provider_host)

add_executable(test_face_resource_provider unit/test_face_resource_provider.c)
target_include_directories(test_face_resource_provider PRIVATE support)
target_link_libraries(test_face_resource_provider PRIVATE face_resource_provider_host)
robot_apply_warnings(test_face_resource_provider)
add_test(NAME face_resource_provider COMMAND test_face_resource_provider)
