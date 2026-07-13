set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_tool_hints)
if(DEFINED ENV{AIROBOT_ARM_GCC_ROOT})
  list(APPEND _tool_hints "$ENV{AIROBOT_ARM_GCC_ROOT}/usr/bin" "$ENV{AIROBOT_ARM_GCC_ROOT}/bin")
endif()

find_program(CMAKE_C_COMPILER arm-none-eabi-gcc HINTS ${_tool_hints} REQUIRED)
find_program(CMAKE_ASM_COMPILER arm-none-eabi-gcc HINTS ${_tool_hints} REQUIRED)
find_program(CMAKE_OBJCOPY arm-none-eabi-objcopy HINTS ${_tool_hints} REQUIRED)
find_program(CMAKE_SIZE arm-none-eabi-size HINTS ${_tool_hints} REQUIRED)

set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m3 -mthumb -ffunction-sections -fdata-sections -fno-common")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cortex-m3 -mthumb -x assembler-with-cpp")
