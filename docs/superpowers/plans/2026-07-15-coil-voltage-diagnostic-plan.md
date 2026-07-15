# ULN2003 逐相线圈电压诊断实施计划

- 日期：2026-07-15
- 设计依据：`docs/superpowers/specs/2026-07-15-coil-voltage-diagnostic-design.md`
- 范围：内部诊断协议、STM32 单通道定时服务、ESP32 内部接口、模拟器、自动测试、真机右侧 A-D 测量
- 不在本计划：正常运动保持、网页入口、AI 工具、自动连续测量、左轮复测或相序调整

## 执行原则

1. 测试先行：协议和纯 C 服务先出现失败测试，再实现最小通过路径。
2. 诊断隔离：正常 `MOVE_WHEELS` 的参数、半步序列和完成后释放行为不变。
3. 硬截止：单次导通时间只能为 100-3000ms，本次真机探针固定为 3000ms。
4. 统一释放：正常完成、STOP、模式切换、ESTOP、FAULT、心跳超时、会话更换和 MCU 复位都关闭两轮输出。
5. 单次确认：右侧 A、B、C、D 分四次执行，每次都在 5V 断电时接线并重新取得操作员确认。
6. 私有信息隔离：不提交照片、`/config`、临时探针、设备地址、API Key 或第三方中转地址。

## Task 1：追加线圈诊断协议

文件：

- 修改 `protocol/messages.json`
- 重新生成 `protocol/generated/protocol_ids.py`
- 重新生成 `protocol/generated/protocol_ids.h`
- 重新生成 `protocol/generated/protocol_layouts.h`
- 重新生成 `protocol/generated/protocol_golden.h`
- 重新生成 `protocol/golden/golden_slots.json`
- 修改 `tests/test_protocol_python.py`
- 修改 `tests/unit/test_protocol_c.c`

步骤：

1. 追加轮侧、通道和诊断结果枚举：左右轮、A-D、完成/中止。
2. 追加 `COIL_DIAGNOSTIC` 请求，载荷固定为 `command_id:u32`、`wheel:u8`、`channel:u8`、`duration_ms:u16`。
3. 追加 `COIL_DIAGNOSTIC_RESULT` 事件，载荷包含命令 ID 和完成/中止结果；不修改任何旧消息 ID 或布局。
4. 运行生成器并锁定 Python/C 长度、格式和黄金槽解码。
5. 在 STM32 `robot_spi_slot.c` 和 `controller_session_policy.c` 中登记新请求，使其只在有效控制器会话后可路由。

验证：

```bash
.venv/bin/python protocol/generate.py
.venv/bin/python protocol/generate.py --check
.venv/bin/python -m unittest tests.test_protocol_generator tests.test_protocol_python
cmake --build build/host --target test_protocol_c
ctest --test-dir build/host --output-on-failure -R protocol_c
```

计划提交：`feat(protocol): add bounded coil diagnostics`

## Task 2：实现独立的 STM32 线圈诊断服务

文件：

- 新建 `firmware/stm32/App/Diagnostics/coil_diagnostic_service.h`
- 新建 `firmware/stm32/App/Diagnostics/coil_diagnostic_service.c`
- 新建 `tests/unit/test_coil_diagnostic_service.c`
- 修改 `tests/CMakeLists.txt`
- 修改 `firmware/stm32/CMakeLists.txt`

步骤：

1. 定义可注入 `apply(left_mask, right_mask, context)` 的纯 C 服务，不直接依赖 HAL。
2. `start()` 严格拒绝空指针、非法轮侧、非法通道、100ms 以下、3000ms 以上和重复启动。
3. 启动时只输出目标轮的 `1 << channel`，另一轮固定输出零；保存命令 ID、开始时刻和持续时间。
4. `tick()` 使用无符号时间差处理 `uint32_t` 回绕，达到截止时间时先输出左右全零，再记录完成。
5. `abort()` 对任何活动诊断先输出左右全零，再记录中止；重复 abort 不重新通电。
6. 测试左右各通道掩码、2999/3000ms 边界、时间回绕、重复启动、显式中止和最终输出全零。

验证：

```bash
cmake --build build/host --target test_coil_diagnostic_service
ctest --test-dir build/host --output-on-failure -R coil_diagnostic
```

## Task 3：接入 STM32 应用状态与安全链路

文件：

- 修改 `firmware/stm32/App/app.c`
- 修改 `firmware/stm32/App/Storage/resource_policy.h`
- 修改 `firmware/stm32/App/Storage/resource_policy.c`
- 修改 `tests/unit/test_resource_policy.c`
- 修改 `tests/unit/test_robot_core.c`（仅互斥和释放回归需要时）

步骤：

1. 在应用上下文初始化 `CoilDiagnosticService`，使用现有 `uln2003_hw_apply`，并在 1ms 定时入口执行其截止检查。
2. 将现有 `stop_motion()` 收敛为统一的 `stop_motor_outputs()`：运动和诊断任一活动都中止；都不活动时仍调用 `uln2003_hw_off()`。
3. 新增诊断 handler，仅允许链路健康、`MANUAL`、无资源更新、无运动且无诊断时启动；忙返回 `QUEUE_FULL`，状态不符返回 `BAD_STATE`，参数非法返回 `OUT_OF_RANGE`。
4. `MOVE_WHEELS` 在诊断活动时返回 `QUEUE_FULL`；资源更新入口把“运动或诊断活动”统一视为电机输出占用。
5. 状态快照的 `active_command_id` 优先报告活动运动，否则报告活动诊断；CLEAR_ESTOP 也要求两者都不活动。
6. HELLO 会话更换、STOP、模式切换、EC11 停止、心跳超时和故障路径全部调用统一释放函数。
7. 诊断结束后发送一次 `COIL_DIAGNOSTIC_RESULT`；重复请求只重放 ACK/NACK，不再次导通。
8. 更新能力位，允许 ESP32 在调用内部诊断前确认 STM32 固件支持该消息。

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

计划提交：`feat(stm32): add safe coil diagnostic service`

## Task 4：接入 ESP32 内部接口与模拟器

文件：

- 修改 `firmware/esp32/services/device_service.py`
- 修改 `tools/stm_simulator.py`
- 修改 `tests/test_esp_services.py`
- 修改 `tests/test_stm_simulator.py`

步骤：

1. `DeviceService` 增加有界的诊断结果等待表；会话更换时清空，迟到结果不形成无界缓存。
2. 增加仅供本地诊断使用的方法，校验轮侧、通道和 100-3000ms，发送请求并等待结果。
3. 等待超时、任务取消或 STM32 会话变化时排队发送 best-effort STOP；不自动重试。
4. 保持 Web 路由和 AI 工具白名单不变，用静态契约测试确认没有新增公开入口。
5. 模拟器实现同样的状态门禁、互斥、3000ms 完成和 STOP/心跳中止语义，便于端到端主机测试。
6. 测试正常完成、非法参数、MANUAL 门禁、与 MOVE/资源更新互斥、STOP、链路丢失、迟到结果和会话更换。

验证：

```bash
.venv/bin/python -m unittest tests.test_esp_services tests.test_stm_simulator
```

计划提交：`feat(esp32): support internal coil diagnostics`

## Task 5：全量回归、提交和真机部署

验证命令：

```bash
.venv/bin/python protocol/generate.py --check
.venv/bin/python -m unittest discover -s tests -p 'test_*.py'
cmake --build build/host
ctest --test-dir build/host --output-on-failure
AIROBOT_ARM_GCC_ROOT=/home/orange/.cache/airobot-toolchain/root cmake --build build/stm32-live
.venv/bin/python tools/check_firmware_size.py build/stm32-live/firmware/stm32/desktop_robot.elf
.venv/bin/python tools/check_firmware_isa.py build/stm32-live/firmware/stm32/desktop_robot.elf
.venv/bin/python tools/build_esp_bundle.py --output build/esp32-bundle
git diff --check
```

步骤：

1. 记录 Python/C 测试数量以及 STM32 Flash、RAM/stack 和 ISA 检查结果。
2. 扫描待提交差异，确认不含凭据、私有设备信息、第三方中转地址、`/config`、照片或临时探针。
3. 只按上述逻辑提交暂存对应文件并推送 `main`，不暂存现有无关文件。
4. 刷写前要求电机 5V 保持物理断开，并明确告知刷写会复位 STM32、OLED 和 PC13。
5. 使用已验证的 ST-Link/OpenOCD 流程写入并校验 STM32；随后恢复 ESP32 正式应用和 SPI 会话，不发送诊断或运动命令。
6. 先确认正式网页、OLED、STM32 状态和 ESTOP 行为正常，再准备测量。

## Task 6：右侧 A-D 四次现场测量

每一路都单独执行，不将以下确认复用于下一路：

1. 电机 5V 断开时，用两根杜邦线把右侧 ULN2003 的 VCC 和当前待测输出引到板外，末端彼此分开。
2. 万用表置直流电压档，红表笔接 VCC 延长端，黑表笔接待测输出延长端；表笔不接触板上相邻针脚。
3. 接通电机 5V，确认两板灯全灭、裸轴安全且能够立即断电。
4. 临时创建固定通道、固定 3000ms、无重试的 `.codex-tmp/coil_voltage_probe.py`，语法检查通过后等待操作员当次确认。
5. 在运行前再次明确通知“右侧目标通道指示灯即将亮 3 秒”，收到确认后才执行唯一命令。
6. 记录稳定压差，确认 3 秒后两块板指示灯全部熄灭；任何异常立即断开 5V并停止。
7. 恢复 ESP32 正式应用并保留因 USB 切换产生的 ESTOP；删除临时探针。
8. 断开电机 5V后才进入下一通道。顺序固定为右侧 A、B、C、D。

四路完成后比较相对值：单路明显偏低则定位该通道；四路都接近零则检查 3.3V 输入兼容和公共线；四路相近且有明显压差则转入已知正常电机/驱动板交叉替换。真机测量不会因本计划或代码部署自动开始。
