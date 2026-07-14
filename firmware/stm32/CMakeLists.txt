enable_language(ASM)

set(CUBE_ROOT "${PROJECT_SOURCE_DIR}/third_party/STM32CubeF1")
set(CMSIS_DEVICE "${CUBE_ROOT}/Drivers/CMSIS/Device/ST/STM32F1xx")
set(HAL_ROOT "${CUBE_ROOT}/Drivers/STM32F1xx_HAL_Driver")

if(NOT EXISTS "${CMSIS_DEVICE}/Include/stm32f103xb.h" OR
   NOT EXISTS "${HAL_ROOT}/Inc/stm32f1xx_hal.h")
  message(FATAL_ERROR "STM32CubeF1 submodules are missing. Run tools/bootstrap_stm32cube.sh")
endif()

set(HAL_SOURCES
  ${HAL_ROOT}/Src/stm32f1xx_hal.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_cortex.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_dma.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_flash.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_flash_ex.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_gpio.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_pwr.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_rcc.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_rcc_ex.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_spi.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_tim.c
  ${HAL_ROOT}/Src/stm32f1xx_hal_tim_ex.c
)

set(APP_SOURCES
  Core/Src/main.c
  Core/Src/stm32f1xx_hal_msp.c
  Core/Src/stm32f1xx_it.c
  Core/Src/syscalls.c
  App/Board/board.c
  App/app.c
  App/Core/robot_state.c
  App/Safety/safety_supervisor.c
  App/Motion/motion_service.c
  App/Input/ec11.c
  App/Input/input_policy.c
  App/Ui/ui_service.c
  App/Protocol/robot_crc16.c
  App/Protocol/robot_spi_slot.c
  Drivers/Display/soft_i2c.c
  Drivers/Display/ssd1306.c
  Drivers/Motor/uln2003_hw.c
  Drivers/Spi/spi_mailbox.c
  Drivers/Storage/w25q.c
)

add_executable(desktop_robot
  ${APP_SOURCES}
  ${HAL_SOURCES}
  ${CMSIS_DEVICE}/Source/Templates/system_stm32f1xx.c
  ${CMSIS_DEVICE}/Source/Templates/gcc/startup_stm32f103xb.s
)

target_include_directories(desktop_robot PRIVATE
  Core/Inc
  App/Board
  App
  App/Core
  App/Safety
  App/Motion
  App/Input
  App/Ui
  App/Protocol
  Drivers/Display
  Drivers/Motor
  Drivers/Spi
  Drivers/Storage
  ${PROJECT_SOURCE_DIR}/protocol/generated
  ${CUBE_ROOT}/Drivers/CMSIS/Core/Include
  ${CMSIS_DEVICE}/Include
  ${HAL_ROOT}/Inc
)

if(DEFINED ENV{AIROBOT_ARM_GCC_ROOT})
  set(ARM_ROOT "$ENV{AIROBOT_ARM_GCC_ROOT}")
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m3 -mthumb -print-multi-directory
    OUTPUT_VARIABLE ARM_MULTILIB_DIRECTORY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  set(ARM_NEWLIB_DIRECTORY
      "${ARM_ROOT}/usr/lib/arm-none-eabi/newlib/${ARM_MULTILIB_DIRECTORY}")
  if(NOT EXISTS "${ARM_NEWLIB_DIRECTORY}/libc_nano.a")
    message(FATAL_ERROR "Cortex-M3 newlib multilib is missing: ${ARM_NEWLIB_DIRECTORY}")
  endif()
  target_include_directories(desktop_robot SYSTEM PRIVATE "${ARM_ROOT}/usr/include/newlib")
  target_link_directories(desktop_robot PRIVATE "${ARM_NEWLIB_DIRECTORY}")
  set(NANO_SPECS "--specs=${ARM_NEWLIB_DIRECTORY}/nano.specs")
  set(NOSYS_SPECS "--specs=${ARM_NEWLIB_DIRECTORY}/nosys.specs")
else()
  set(NANO_SPECS "--specs=nano.specs")
  set(NOSYS_SPECS "--specs=nosys.specs")
endif()

target_compile_definitions(desktop_robot PRIVATE STM32F103xB USE_HAL_DRIVER)
target_compile_options(desktop_robot PRIVATE
  -mcpu=cortex-m3 -mthumb -Wall -Wextra -Wpedantic
  -ffunction-sections -fdata-sections -fstack-usage
)
target_link_options(desktop_robot PRIVATE
  -mcpu=cortex-m3 -mthumb
  -nostartfiles
  -T${CMAKE_CURRENT_SOURCE_DIR}/STM32F103C8Tx_FLASH.ld
  -Wl,--gc-sections
  -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/desktop_robot.map
  ${NANO_SPECS}
  ${NOSYS_SPECS}
)
target_link_libraries(desktop_robot PRIVATE c_nano m nosys)

set_target_properties(desktop_robot PROPERTIES OUTPUT_NAME "desktop_robot" SUFFIX ".elf")

add_custom_command(TARGET desktop_robot POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:desktop_robot>
          ${CMAKE_CURRENT_BINARY_DIR}/desktop_robot.hex
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:desktop_robot>
          ${CMAKE_CURRENT_BINARY_DIR}/desktop_robot.bin
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:desktop_robot>
  COMMAND ${CMAKE_COMMAND} -E env
          AIROBOT_ARM_GCC_ROOT=$ENV{AIROBOT_ARM_GCC_ROOT}
          python3 ${PROJECT_SOURCE_DIR}/tools/check_firmware_isa.py
          $<TARGET_FILE:desktop_robot>
  VERBATIM
)
