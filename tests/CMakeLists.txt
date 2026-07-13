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
