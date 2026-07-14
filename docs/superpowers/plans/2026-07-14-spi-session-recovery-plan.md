# SPI 安全会话恢复实施计划

- 日期：2026-07-14
- 设计依据：`docs/superpowers/specs/2026-07-14-spi-session-recovery-design.md`
- 范围：STM32 安全监督器、STM32 仿真器、主机测试与真机无运动验收

## Task 1：固定安全监督器恢复契约

文件：

- 修改 `tests/unit/test_robot_core.c`

步骤：

1. 添加失败断言：初始化后收到合法帧必须启动会话并标记链路健康。
2. 覆盖心跳超时进入 ESTOP、会话关闭和故障码 8。
3. 超时后再次收到合法帧，只恢复会话和链路健康，ESTOP 与故障码保持不变。
4. 最后显式调用 operator-confirmed clear，验证回到 IDLE 且故障码归零。

## Task 2：实现 STM32 会话恢复

文件：

- 修改 `firmware/stm32/App/Safety/safety_supervisor.c`

步骤：

1. 让 `safety_supervisor_valid_slot()` 无论旧会话是否 active，都设置 active、healthy 和最新时间戳。
2. 不修改 `robot_state_t`、运动状态或表达式。
3. 保持 HELLO handler、750 ms 超时和 `CLEAR_ESTOP` 安全检查不变。
4. 运行 `robot_core` CTest，确认新恢复路径和既有安全测试通过。

## Task 3：同步 STM32 仿真器行为

文件：

- 修改 `tools/stm_simulator.py`
- 修改 `tests/test_stm_simulator.py`

步骤：

1. 添加失败测试：仿真器心跳超时后，下一合法 NOOP 恢复 session，但状态仍为 ESTOP。
2. 修改成功解码消息的入口，使所有合法消息刷新 session 和心跳时间。
3. 保持坏帧、坏版本和坏 CRC 不恢复 session。
4. 随后发送 `CLEAR_ESTOP`，验证 ACK、IDLE、故障码 0。

## Task 4：完整验证与提交

验证命令：

```bash
.venv/bin/python -m unittest discover -s tests
cmake --build build/host
ctest --test-dir build/host --output-on-failure
AIROBOT_ARM_GCC_ROOT=/home/orange/.cache/airobot-toolchain/root cmake --build build/stm32-live
git diff --check
```

步骤：

1. 检查固件尺寸与 Thumb-only Cortex-M3 构建结果。
2. 检查差异只包含安全会话恢复、仿真和测试。
3. 扫描暂存差异，禁止凭据和设备 `/config` 进入提交。
4. 自动提交并推送 `main`。

## Task 5：真机无运动验收

步骤：

1. 刷写 STM32 新固件；ESP32 文件无需改动。
2. 保持 ESP32 正式服务运行，然后单独复位 STM32，复现原启动顺序。
3. 使用临时 DeviceService 测试 STOP -> ESTOP(11) -> CLEAR_ESTOP -> IDLE(0)，不调用 `move()`。
4. 结束串口诊断后复位 ESP32，等待正式 Web 服务启动，再最终复位 STM32。
5. 手机网页再次执行 STOP 和二次确认解除，确认不再返回 error 5。

## 安全边界

- 合法通信只恢复 link/session，不自动解除 ESTOP。
- 测试和验收不发送任何电机运动指令。
- 不修改或输出管理密码、Wi-Fi 密码、API Key。
