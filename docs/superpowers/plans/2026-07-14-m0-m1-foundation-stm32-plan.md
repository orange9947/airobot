# M0/M1 协议基础与 STM32 固件实施计划

- 日期：2026-07-14
- 状态：待执行
- 设计依据：[模块化桌面机器人系统设计](../specs/2026-07-14-desktop-robot-design.md)
- 范围：M0 仓库与协议基础、M1 STM32 实时固件
- 不在本计划：ESP32 配网/网页、OpenAI/DeepSeek 实现、W25Q 资源包写入、语音

## 1. 交付目标

完成本计划后，仓库应具备：

1. 无外部 Python 测试框架依赖的主机测试和 GitHub Actions。
2. 单一协议清单、确定性代码生成、C/Python 编解码和黄金帧契约。
3. 可通过 TCP 固定槽运行的 STM32 设备模拟器和协议诊断工具。
4. 可由 `arm-none-eabi-gcc` 构建、由 ST-Link 烧录的 STM32F103C8T6 固件。
5. STM32 全局状态、安全监督、SPI 心跳、命令去重和状态事件。
6. 双 28BYJ-48 开环运动、EC11、SSD1306、W25Q 探测和基础表情。
7. 失联、STOP、长按、重复命令和外设降级的自动测试与台架验收记录。

M1 完成不代表整机完成。网页和大模型必须在 M2/M3 通过同一协议接入，不能绕过本计划形成的安全边界。

## 2. 执行规则

- 每个任务先添加失败测试或可观测验收，再写最小实现。
- 每个任务通过相关测试、全量主机测试和格式检查后自动提交。
- 每个提交只包含当前任务，使用 Conventional Commits；不修改已经确认的系统设计，除非发现明确矛盾并单独提交文档修正。
- 生成文件进入版本控制。CI 重新运行生成器并通过 `git diff --exit-code` 检查漂移。
- STM32 App 层不得包含 HAL 类型；HAL 只出现在 `Core` 和 `Drivers` 适配层。
- STM32 初始化后不使用动态内存；协议、事件、OLED 和运动队列均使用固定容量。
- 硬件测试先让轮子悬空。任何供电异常、明显温升或不可解释复位都停止测试。
- 本计划的每个绿色提交推送到 `origin/main`，不使用强制推送。

## 3. 环境前置条件

当前环境已经有 Python 3.12、CMake 3.28 和 Git，尚缺 ARM 编译器、OpenOCD、Ninja 和 pytest。本计划不需要 pytest 或 Ninja；主机测试使用 `unittest`、CTest 和系统 Make。

进入 Task 6 前安装：

```bash
sudo apt-get update
sudo apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi openocd
arm-none-eabi-gcc --version
openocd --version
```

硬件烧录使用：

```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg
```

STM32 官方依赖固定为 [STM32CubeF1 v1.8.7](https://github.com/STMicroelectronics/STM32CubeF1/tree/v1.8.7)，仓库提交 `d12e75247d5bcedc734f829b394517ab4c2726e3`。仅初始化所需的 CMSIS Device 和 HAL 嵌套子模块，避免下载无关 BSP 与中间件。

## 4. M0 实施任务

### Task 1：建立主机构建、测试和 CI 基线

文件：

- 修改 `README.md`
- 新建 `CMakeLists.txt`
- 新建 `cmake/RobotWarnings.cmake`
- 新建 `tests/CMakeLists.txt`
- 新建 `tests/__init__.py`
- 新建 `tests/support/test_harness.h`
- 新建 `tests/unit/test_smoke.c`
- 新建 `.github/workflows/ci.yml`

步骤：

1. 把 README 扩展为项目入口，列出当前 M0/M1 范围、设计文档、主机测试命令和安全提示。
2. 根 CMake 默认只构建主机测试；交叉固件由 `ROBOT_BUILD_STM32=ON` 显式启用。
3. `RobotWarnings.cmake` 为仓库 C 代码启用 `-Wall -Wextra -Wpedantic -Werror`，第三方代码不继承 `-Werror`。
4. 创建最小 C 测试入口，验证 CTest 能发现和执行测试。
5. CI 在 Ubuntu 上运行 Python `unittest`、CMake 主机构建、CTest、生成文件漂移检查和 `git diff --check`。

验证：

```bash
cmake -S . -B build/host -DROBOT_BUILD_HOST_TESTS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py'
```

预期：CTest 至少发现一个测试，Python 在没有测试模块时正常返回，工作树只有预期文件。

提交：`build: add host test and CI foundation`

### Task 2：定义协议清单和确定性生成器

文件：

- 新建 `protocol/messages.json`
- 新建 `protocol/__init__.py`
- 新建 `protocol/generate.py`
- 新建 `protocol/generated/__init__.py`
- 新建 `protocol/generated/protocol_ids.py`
- 新建 `protocol/generated/protocol_ids.h`
- 新建 `protocol/generated/protocol_layouts.h`
- 新建 `protocol/golden/golden_frames.json`
- 新建 `tests/test_protocol_generator.py`
- 修改 `CMakeLists.txt`
- 修改 `.github/workflows/ci.yml`

清单内容：

- 协议版本、帧大小、消息域和稳定 ID。
- `HELLO_REQ/RSP`、`HEARTBEAT`、`GET_STATE`、`STATE_SNAPSHOT`。
- `SET_MODE`、`MOVE_WHEELS`、`STOP`、`SET_EXPRESSION`、`SET_RUNTIME_CONFIG`。
- `ACK/NACK`、`MOTION_STARTED/DONE/ABORTED`、`MODE_CHANGED`、`FAULT_EVENT`。
- `FLASH_INFO`。
- 字段名称、整数宽度、符号、小端序、固定载荷长度和枚举。

测试先行：

1. 同一清单连续生成两次必须字节一致。
2. 重复消息 ID、超出域范围、未知字段类型和载荷大于 256 字节必须失败。
3. 生成的 Python 和 C 常量必须具有相同 ID 和长度。
4. 黄金帧覆盖空载荷、最大首版载荷、负轮步数、ACK 和状态快照。

实现约束：

- 生成器只使用 Python 标准库。
- 输出按消息 ID 排序，并在文件头写入“生成文件，不要手改”。
- 黄金帧固定输入值，不包含时间戳或随机值。
- C 布局使用显式字段读写函数，不依赖编译器结构体 packing。

验证：

```bash
python3 -m unittest tests.test_protocol_generator -v
python3 protocol/generate.py --check
git diff --exit-code -- protocol/generated protocol/golden
```

提交：`feat(protocol): add schema and deterministic code generation`

### Task 3：实现 MicroPython 兼容的 Python 固定槽编解码

文件：

- 新建 `firmware/esp32/__init__.py`
- 新建 `firmware/esp32/transport/__init__.py`
- 新建 `firmware/esp32/transport/crc16.py`
- 新建 `firmware/esp32/transport/frame_codec.py`
- 新建 `firmware/esp32/transport/spi_mailbox.py`
- 新建 `tests/test_protocol_python.py`

测试先行：

1. CRC16-CCITT 使用标准检查值和黄金帧。
2. 所有黄金帧解码后字段与清单输入一致，再编码必须得到原字节。
3. 编解码器只接受恰好 268 字节的槽，并验证 MAGIC、版本、LEN、零填充和 CRC。
4. CRC 错误、LEN 大于 256、未知版本和非零填充必须产生计数，不导致 SPI 轮询任务退出。
5. 邮箱跟踪命令序号、100 ms 轮询、250 ms 重试阈值和最多 3 次相同 command_id 重发。

实现约束：

- 运行时代码只使用 MicroPython/CPython 共有的 `struct`、`binascii` 和基础容器。
- 不使用 dataclass、CPython 专用缓冲协议或第三方包。
- 每次只保留一个 268 字节 RX 槽和有限待确认命令表，不累积原始事务流。
- `Frame` 对象只保存已验证字段；原始未验证数据不进入业务层。

验证：

```bash
python3 -m unittest tests.test_protocol_python -v
python3 -m unittest discover -s tests -p 'test_*.py'
```

提交：`feat(protocol): add MicroPython-compatible frame codec`

### Task 4：实现无动态内存的 C 固定槽编解码

文件：

- 新建 `firmware/stm32/App/Protocol/robot_crc16.h`
- 新建 `firmware/stm32/App/Protocol/robot_crc16.c`
- 新建 `firmware/stm32/App/Protocol/robot_protocol.h`
- 新建 `firmware/stm32/App/Protocol/robot_protocol.c`
- 新建 `firmware/stm32/App/Protocol/robot_spi_slot.h`
- 新建 `firmware/stm32/App/Protocol/robot_spi_slot.c`
- 新建 `tests/unit/test_protocol_c.c`
- 新建 `tests/unit/test_spi_slot_c.c`
- 修改 `tests/CMakeLists.txt`

测试先行：

1. C CRC 和 Python 黄金值一致。
2. C 解码/编码全部黄金帧字节一致。
3. 固定槽解析覆盖错误 MAGIC、CRC、长度、非零填充和下一事务恢复用例。
4. 输出缓冲不足、空指针和非法枚举返回稳定错误码，不越界写入。
5. 使用 AddressSanitizer/UndefinedBehaviorSanitizer 运行主机测试。

实现约束：

- 编解码 API 由调用方提供输入输出缓冲和容量。
- 槽编解码使用调用方提供的 268 字节缓冲，不调用 `malloc`、`calloc` 或 `realloc`。
- 整数字段通过显式 `read_u16_le` 等函数处理，不强转未对齐指针。
- HAL 无关代码在主机 GCC 下以 `-Werror` 编译。

验证：

```bash
cmake -S . -B build/host -DROBOT_BUILD_HOST_TESTS=ON -DROBOT_ENABLE_SANITIZERS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

提交：`feat(protocol): add bounded C frame codec and parser`

### Task 5：建立 STM 模拟器、诊断 CLI 和 M0 集成测试

文件：

- 新建 `tools/stm_simulator.py`
- 新建 `tools/__init__.py`
- 新建 `tools/protocol_console.py`
- 新建 `tools/transports.py`
- 新建 `tests/test_stm_simulator.py`
- 新建 `tests/test_protocol_faults.py`
- 新建 `docs/testing/m0-protocol.md`
- 修改 `README.md`

模拟器行为：

- 支持 `--tcp host:port`，每次收发恰好一个 268 字节槽，模拟 SPI 全双工事务。
- 实现 boot_id、HELLO、心跳、IDLE/MANUAL/AI/ESTOP、ACK/NACK、MOVE 和 STOP。
- 运动只模拟时间和事件，不模拟物理惯性。
- 提供丢帧、延迟 ACK、重复事件、错误 CRC 和重启注入开关。

测试先行：

1. 客户端完成 HELLO 后取得状态快照。
2. 错误模式下 MOVE 返回 NACK；正确模式下产生 ACK、STARTED、DONE。
3. 同一 command_id 重发不产生第二个 STARTED。
4. 心跳超时进入 ESTOP；STOP 抢占活动动作。
5. 错误帧后通信可恢复。

验证：

```bash
python3 -m unittest tests.test_stm_simulator tests.test_protocol_faults -v
python3 tools/stm_simulator.py --tcp 127.0.0.1:9001
# 在另一终端运行：
python3 tools/protocol_console.py --tcp 127.0.0.1:9001 hello
```

M0 完成标准：

- 所有主机测试与生成检查通过。
- 协议模拟器可完成完整命令闭环和故障恢复。
- CI 在远端绿色。
- 设计中的首版消息均有 ID、布局和至少一个黄金向量。

提交：`feat(tools): add STM simulator and protocol diagnostics`

## 5. M1 实施任务

### Task 6：引入 STM32CubeF1 并建立交叉编译

文件：

- 新建 `.gitmodules`
- 新建子模块 `third_party/STM32CubeF1`
- 新建 `tools/bootstrap_stm32cube.sh`
- 新建 `cmake/arm-none-eabi-gcc.cmake`
- 新建 `firmware/stm32/CMakeLists.txt`
- 新建 `firmware/stm32/STM32F103C8Tx_FLASH.ld`
- 新建 `firmware/stm32/Core/Inc/stm32f1xx_hal_conf.h`
- 新建 `firmware/stm32/Core/Inc/main.h`
- 新建 `firmware/stm32/Core/Src/syscalls.c`
- 修改根 `CMakeLists.txt`
- 修改 `.github/workflows/ci.yml`

步骤：

1. 将 STM32CubeF1 作为子模块固定在 v1.8.7 对应提交。
2. bootstrap 脚本只初始化：
   - `Drivers/CMSIS/Device/ST/STM32F1xx`
   - `Drivers/STM32F1xx_HAL_Driver`
3. CMake 使用 CubeF1 自带 CMSIS Core、CMSIS Device、startup 和 HAL 源码。
4. 定义 `STM32F103xB`、`USE_HAL_DRIVER`，链接目标为 64 KiB Flash、20 KiB RAM。
5. 启用 RCC、GPIO、CORTEX、SPI、DMA、FLASH 和 PWR HAL 模块；OLED 使用软件 I2C。
6. 添加固件大小报告；Flash 或 RAM 超出链接脚本立即失败。

验证：

```bash
./tools/bootstrap_stm32cube.sh
cmake -S . -B build/stm32 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
  -DROBOT_BUILD_STM32=ON
cmake --build build/stm32
arm-none-eabi-size build/stm32/desktop_robot.elf
```

预期：生成 ELF/HEX/BIN，链接使用 64 KiB Flash 和 20 KiB RAM，不包含未使用 BSP/中间件。

提交：`build(stm32): add pinned CubeF1 cross build`

### Task 7：实现安全上电、时钟和板级引脚

文件：

- 新建 `firmware/stm32/Core/Src/main.c`
- 新建 `firmware/stm32/Core/Src/system_clock.c`
- 新建 `firmware/stm32/Core/Src/stm32f1xx_it.c`
- 新建 `firmware/stm32/Core/Src/stm32f1xx_hal_msp.c`
- 新建 `firmware/stm32/App/Board/board_pins.h`
- 新建 `firmware/stm32/App/Board/board.h`
- 新建 `firmware/stm32/App/Board/board.c`
- 新建 `tests/unit/test_board_contract.c`
- 修改 `firmware/stm32/CMakeLists.txt`

上电顺序：

1. 立即把 8 个电机相位输出配置为低电平。
2. 尝试 HSE 8 MHz + PLL 72 MHz；HSE 失败时回退 HSI + PLL 64 MHz，并记录降级状态。
3. 初始化 SysTick、TIM2、SPI1 从机 DMA、SPI2 主机、OLED 软件 I2C GPIO 和 PC13 状态灯。
4. 不启用 JTAG，只保留 SWD，确保 PA13/PA14 可调试。
5. 外设初始化失败保持线圈关闭并进入可诊断状态。

测试：

- 编译期检查所有已分配 GPIO 不冲突，SWD 脚不被业务占用。
- 主机测试验证板级配置表包含设计文档中的全部信号。
- 使用 OpenOCD 连接 ST-Link，复位后读取 PC 和基本寄存器。
- 烧录最小固件，确认 PC13 状态节拍且所有 ULN2003 输入为低。

验证：

```bash
ctest --test-dir build/host --output-on-failure -R board
cmake --build build/stm32
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/stm32/desktop_robot.elf verify reset exit"
```

提交：`feat(stm32): add safe board bring-up`

### Task 8：实现全局状态、事件队列和安全监督

文件：

- 新建 `firmware/stm32/App/Core/robot_types.h`
- 新建 `firmware/stm32/App/Core/robot_state.h`
- 新建 `firmware/stm32/App/Core/robot_state.c`
- 新建 `firmware/stm32/App/Core/event_queue.h`
- 新建 `firmware/stm32/App/Core/event_queue.c`
- 新建 `firmware/stm32/App/Safety/safety_supervisor.h`
- 新建 `firmware/stm32/App/Safety/safety_supervisor.c`
- 新建 `tests/unit/test_robot_state.c`
- 新建 `tests/unit/test_event_queue.c`
- 新建 `tests/unit/test_safety_supervisor.c`

测试先行：

- `BOOT -> SELF_TEST -> IDLE` 合法，非法跳转被拒绝。
- MANUAL/AI 互切先产生停车请求并清队列。
- STOP 和本体长按从任意运行状态进入 ESTOP。
- HELLO 前不计心跳；会话建立后 750 ms 超时进入 ESTOP。
- ESTOP 只有在链路健康、危险消失、按键释放且本体短按时解除。
- EC11 故障进入 FAULT；OLED/W25Q 故障只设置降级位。
- 固定事件队列满时保留 STOP/FAULT，丢弃低优先级重复状态事件。

实现约束：

- 状态模块为纯 C，无 HAL 依赖。
- 所有转换通过单一 `robot_state_request()` 入口。
- 安全监督每 1 ms tick，时间比较处理 32 位毫秒回绕。
- 事件队列容量固定，统计溢出次数。

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R 'state|event|safety'
```

提交：`feat(stm32): add authoritative state and safety supervisor`

### Task 9：接入 SPI 邮箱、会话、命令路由和状态事件

文件：

- 新建 `firmware/stm32/Drivers/Spi/spi_mailbox.h`
- 新建 `firmware/stm32/Drivers/Spi/spi_mailbox.c`
- 新建 `firmware/stm32/App/Protocol/protocol_router.h`
- 新建 `firmware/stm32/App/Protocol/protocol_router.c`
- 新建 `firmware/stm32/App/Protocol/command_dedup.h`
- 新建 `firmware/stm32/App/Protocol/command_dedup.c`
- 新建 `tests/unit/test_protocol_router.c`
- 新建 `tests/unit/test_command_dedup.c`
- 新建 `tests/integration/test_stm_session.c`

实现：

- SPI1 使用硬件 NSS 从机和固定 268 字节 RX/TX DMA 槽；每次事务结束后主循环处理 RX，并准备下一次 TX。
- TX 邮箱队列中 STOP/NACK/FAULT 具有高优先级槽位；无事件时填充 NOOP。
- HELLO 响应包含 STM boot_id、协议版本、固件版本和能力位。
- 去重缓存保存最近 16 个 `boot_id + seq + command_id` 结果。
- 命令路由先做通用校验，再调用状态、运动、UI 或存储接口。
- 每秒发送状态快照，状态变化即时发送事件。

测试先行：

- 未 HELLO 的控制命令被拒绝。
- 版本不匹配只允许最小诊断，不能运动。
- ACK、NACK 和错误码与协议清单一致。
- 重复 MOVE 只调用一次运动接口。
- STOP 即使 TX 普通队列已满仍可入队。
- boot_id 改变后旧命令和旧 ACK 不再有效。

验证：

```bash
ctest --test-dir build/host --output-on-failure -R 'router|dedup|session'
cmake --build build/stm32
```

提交：`feat(stm32): add SPI mailbox session and command routing`

### Task 10：实现双轮半步驱动和有限运动任务

文件：

- 新建 `firmware/stm32/Drivers/Motor/uln2003.h`
- 新建 `firmware/stm32/Drivers/Motor/uln2003.c`
- 新建 `firmware/stm32/App/Motion/motion_planner.h`
- 新建 `firmware/stm32/App/Motion/motion_planner.c`
- 新建 `firmware/stm32/App/Motion/motion_service.h`
- 新建 `firmware/stm32/App/Motion/motion_service.c`
- 新建 `tests/unit/test_uln2003_sequence.c`
- 新建 `tests/unit/test_motion_planner.c`
- 新建 `tests/unit/test_motion_service.c`
- 修改 `firmware/stm32/Core/Src/stm32f1xx_it.c`

固定安全参数：

- 8 相半步序列。
- TIM2 1 kHz 调度。
- 默认软上限 400 half-steps/s。
- 固件硬上限 800 half-steps/s。
- 默认加速度 600 half-steps/s²，硬上限 1200 half-steps/s²。
- 单动作超时不超过 2000 ms。
- 左右轮各自支持相序和方向反转配置。

测试先行：

- 正反向半步序列完整且互为逆序。
- 双轮相位累加器按给定速率产生正确步数比例。
- 加减速不跳过硬上限，短动作可以在目标前减速。
- STOP、超时、模式切换和 ESTOP 清队列并关闭所有相位。
- 左右轮作为一个原子任务产生一次 STARTED 和一次终态事件。
- 零步数、溢出步数、负速率编码和超时越界被拒绝。

硬件验收：

1. 不接电机时用万用表或逻辑分析仪验证 8 路相位。
2. 接单轮并悬空，验证前后方向和停止释放。
3. 接双轮并悬空，验证前进、后退、原地左右转。
4. 动作中发送 STOP 和停止 SPI 轮询，确认停车时间。

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R 'uln2003|motion'
cmake --build build/stm32
```

提交：`feat(stm32): add bounded differential stepper motion`

### Task 11：实现 EC11 输入、模式操作和本体急停

文件：

- 新建 `firmware/stm32/Drivers/Input/ec11.h`
- 新建 `firmware/stm32/Drivers/Input/ec11.c`
- 新建 `firmware/stm32/App/Input/input_service.h`
- 新建 `firmware/stm32/App/Input/input_service.c`
- 新建 `tests/unit/test_ec11.c`
- 新建 `tests/unit/test_input_service.c`

测试先行：

- 正转、反转、跳变和抖动序列得到稳定档位事件。
- 短于消抖阈值的按键脉冲被忽略。
- 短按只在释放后产生一次确认。
- 持续 1500 ms 产生一次长按 ESTOP，不等待松手且不重复触发。
- 启动卡键进入 FAULT。
- ESTOP 后必须先松手，再短按才能提出本地解除请求。

实现：

- 1 ms 采样 A/B/SW，四状态查表解码旋转。
- 旋转只改变候选模式，短按才请求状态转换。
- 长按直接调用安全监督入口，不通过普通事件队列排队。

硬件验收：连续快速旋转、慢速跨档、短按、长按和启动按住均与状态机一致。

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R 'ec11|input'
cmake --build build/stm32
```

提交：`feat(stm32): add EC11 mode control and local estop`

### Task 12：实现 SSD1306、状态页和内置表情

文件：

- 新建 `firmware/stm32/Drivers/Display/ssd1306.h`
- 新建 `firmware/stm32/Drivers/Display/ssd1306.c`
- 新建 `firmware/stm32/App/Ui/ui_canvas.h`
- 新建 `firmware/stm32/App/Ui/ui_canvas.c`
- 新建 `firmware/stm32/App/Ui/ui_service.h`
- 新建 `firmware/stm32/App/Ui/ui_service.c`
- 新建 `firmware/stm32/App/Ui/assets_builtin.c`
- 新建 `firmware/stm32/App/Ui/font_5x7.c`
- 新建 `tests/unit/test_ui_canvas.c`
- 新建 `tests/golden/oled/*.pbm`

测试先行：

- 像素、线、矩形、字符边界不越过 1024 字节帧缓冲。
- 文本超长时截断或换行，不覆盖状态图标。
- 每个内置表情输出固定 PBM 黄金图。
- IDLE/MANUAL/AI/ESTOP/FAULT 页面含预期模式和错误码区域。
- I2C NACK 设置 OLED 降级位，不阻塞安全/运动任务。

实现：

- OLED I2C 地址启动时探测 `0x3C`，只在配置允许时尝试 `0x3D`。
- 完整帧刷新分为小块，目标 10 fps。
- 动画和状态 UI 只写 canvas；SSD1306 驱动只负责命令与数据传输。
- ESTOP/FAULT 页面优先级高于普通表情。

硬件验收：显示全部基础表情、模式切换、ESP 链路状态和错误码；运动期间动画不影响步进稳定性。

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R 'ui|oled'
cmake --build build/stm32
```

提交：`feat(stm32): add OLED status UI and built-in expressions`

### Task 13：实现 W25Q 探测和 M1 降级路径

文件：

- 新建 `firmware/stm32/Drivers/Storage/w25q.h`
- 新建 `firmware/stm32/Drivers/Storage/w25q.c`
- 新建 `firmware/stm32/App/Storage/storage_service.h`
- 新建 `firmware/stm32/App/Storage/storage_service.c`
- 新建 `tests/unit/test_w25q.c`
- 新建 `tests/unit/test_storage_service.c`

M1 只实现：

- JEDEC ID 读取。
- 容量识别和 1 MB 最低容量检查。
- 状态/错误计数。
- `FLASH_INFO` 协议响应。
- 缺失或无效时切换内置表情。

M1 不实现擦除、写入或 A/B 资源切换，这些属于 M4。

测试先行：

- 有效 W25Q80 ID 返回 1 MB 能力。
- 全 `0xFF`、全 `0x00`、未知厂商和容量过小均标记降级。
- SPI 超时不会重试到阻塞主循环。
- Flash 缺失时 UI 内置资源仍可使用，运动状态不进入 FAULT。

硬件验收：连接和断开 W25Q 分别验证 FLASH_INFO 与内置表情回退。

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R 'w25q|storage'
cmake --build build/stm32
```

提交：`feat(stm32): add W25Q detection and graceful fallback`

### Task 14：整合主循环、故障注入和固件资源预算

文件：

- 新建 `firmware/stm32/App/app.h`
- 新建 `firmware/stm32/App/app.c`
- 新建 `tests/integration/test_app_flow.c`
- 新建 `tests/integration/test_app_faults.c`
- 新建 `tools/check_firmware_size.py`
- 修改 `firmware/stm32/Core/Src/main.c`
- 修改 `firmware/stm32/CMakeLists.txt`
- 修改 `.github/workflows/ci.yml`

主循环顺序：

1. 消费已完成的 SPI RX 槽并解析有限数量消息。
2. tick 安全监督和输入。
3. 路由已验证命令。
4. tick 运动、UI 和存储服务。
5. 发送高优先级事件和有限数量普通事件。
6. 无工作时执行 `__WFI()`，由 SysTick/SPI DMA/定时器唤醒。

集成测试：

- BOOT/自检/HELLO/IDLE/MANUAL/MOVE/DONE 完整路径。
- AI 模式 MOVE 与错误模式 NACK。
- MOVE 执行中 STOP、SPI 心跳超时和本体长按。
- 重复帧、错误 CRC、TX 队列满和 MCU 新 boot_id。
- OLED/W25Q 降级、EC11 FAULT。
- 毫秒计数回绕附近的心跳和动作超时。

资源预算门禁：

- 链接脚本硬限制 64 KiB Flash、20 KiB RAM。
- CI 额外要求静态 RAM + 预留栈不超过 16 KiB，保留至少 4 KiB 栈/异常余量。
- 生成 map 文件并在 CI artifact 中保存。
- 检查固件符号，不允许 App/Drivers 之外出现 `malloc/calloc/realloc/free` 调用。

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure
cmake --build build/stm32
python3 tools/check_firmware_size.py build/stm32/desktop_robot.elf
```

提交：`feat(stm32): integrate realtime application and fault handling`

### Task 15：执行 M1 硬件验收并固化操作文档

文件：

- 新建 `docs/hardware/wiring-v1.md`
- 新建 `docs/hardware/stlink-flashing.md`
- 新建 `docs/testing/m1-hardware-acceptance.md`
- 新建 `docs/testing/m1-results.md`
- 修改 `README.md`

接线文档必须包含：

- ESP GPIO10/11/12/13 与 STM PA4/PA7/PA5/PA6 的 SPI 连接。
- W25Q PB12/PB13/PB14/PB15、OLED PB6/PB7、EC11 PA0/PA1/PA2 和双电机最终引脚表。
- 左右 ULN2003、OLED、EC11、W25Q 和 ST-Link 引脚表。
- 5V/3A 电源分支、共地、去耦和禁止 5V 进入 GPIO 的警告。
- 电机悬空测试顺序和软件急停的限制。

硬件验收顺序：

1. 不接电机供电，ST-Link 连接、烧录、复位和状态灯。
2. OLED、EC11、W25Q 单独验证。
3. 单轮低速验证相序和方向。
4. 双轮悬空验证差速动作和加减速。
5. 动作中 STOP、本体长按、停止 SPI 轮询、复位 ESP 侧链路。
6. 重复命令和错误 CRC 注入。
7. 双电机 10 分钟循环，记录 5V 波动、复位、ULN2003、升压模块和电池温升。
8. 运行 2 小时心跳、模式切换、OLED 动画和动作循环，记录错误计数和恢复结果。

`m1-results.md` 每项记录日期、固件提交、接线版本、测量值、结果和失败原因。没有实际测量的数据不能标记通过。

最终验证：

```bash
python3 -m unittest discover -s tests -p 'test_*.py'
cmake --build build/host
ctest --test-dir build/host --output-on-failure
cmake --build build/stm32
python3 tools/check_firmware_size.py build/stm32/desktop_robot.elf
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/stm32/desktop_robot.elf verify reset exit"
```

M1 完成标准：

- 所有主机、契约、集成和交叉编译测试通过。
- 固件满足 Flash/RAM 预算。
- 设计中的状态、协议、运动、EC11、OLED 和 W25Q 探测行为均有测试。
- STOP、长按和心跳失联在真实硬件上可靠停车。
- M1 硬件验收记录没有未解释失败。
- `main` 工作树干净，CI 绿色，提交已推送。

提交：`docs: add STM32 wiring and M1 acceptance results`

## 6. 依赖关系与执行顺序

```text
Task 1 构建基线
  -> Task 2 协议清单
  -> Task 3 Python 协议
  -> Task 4 C 协议
  -> Task 5 模拟器（M0 完成）
  -> Task 6 交叉编译
  -> Task 7 板级上电
  -> Task 8 状态与安全
  -> Task 9 SPI 邮箱路由
  -> Task 10 电机
  -> Task 11 EC11
  -> Task 12 OLED
  -> Task 13 W25Q
  -> Task 14 全系统集成
  -> Task 15 硬件验收（M1 完成）
```

Task 10-13 的纯逻辑测试可以并行准备，但真实驱动集成必须在 Task 7-9 稳定后依次进行。任何安全测试失败都会阻止后续带电电机测试。

## 7. 后续衔接

M0/M1 通过后，M2 直接复用：

- `protocol/generated/protocol_ids.py`
- `firmware/esp32/transport/frame_codec.py`
- `firmware/esp32/transport/spi_mailbox.py`
- `tools/stm_simulator.py`
- STM32 真实协议端点

网页和 AI 开发先连接 STM 模拟器，再连接真实 SPI。M2/M3 不得修改 M1 的硬上限、ESTOP 解除条件或状态权威关系；需要协议扩展时只能增加向后兼容消息并更新黄金契约。
