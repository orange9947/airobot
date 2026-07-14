# 无按键 EC11 输入策略实施计划

- 日期：2026-07-14
- 设计依据：`docs/superpowers/specs/2026-07-14-desktop-robot-design.md`
- 范围：仅 STM32 输入策略、测试和接线说明；不修改 SPI 协议或 ESP32 API

## 目标

当前硬件使用无按键 EC11：A=PA0、B=PA1、公共端 C=GND。每个有效旋转档位立即切换 `IDLE`、`MANUAL`、`AI`。PA2 保留给未来独立按键；启用后恢复旋转预选、短按确认、长按急停。

## Task 1：提取可测试的输入策略

文件：

- 新建 `firmware/stm32/App/Input/input_policy.h`
- 新建 `firmware/stm32/App/Input/input_policy.c`
- 修改 `tests/CMakeLists.txt`
- 修改 `tests/unit/test_robot_core.c`

步骤：

1. 先添加失败测试，覆盖无按键顺/逆向立即切换、ESTOP 中拒绝旋转解锁，以及有按键候选模式、短按确认、长按急停和短按解除请求。
2. 定义不依赖 HAL 的 `input_policy_t` 和动作结果；策略输入为 EC11 事件与当前状态，输出为无动作、设置模式、急停或解除急停。
3. 保持 `ec11.c` 只负责电气事件解码，不包含模式业务逻辑。
4. 运行主机 CTest，确认新增策略和现有运动、安全测试通过。

## Task 2：接入 STM32 应用和板级配置

文件：

- 修改 `firmware/stm32/App/Board/board_pins.h`
- 修改 `firmware/stm32/App/app.c`
- 修改 `firmware/stm32/CMakeLists.txt`

步骤：

1. 增加 `BOARD_ENCODER_BUTTON_PRESENT`，当前固定为 `0`。
2. ISR 在无按键配置下向 EC11 解码器传入稳定的“松开”状态，PA2 保持上拉但不产生按键事件。
3. 应用层使用输入策略处理旋转：成功切换前终止当前动作，更新状态/候选模式并发送本地 `MODE_CHANGED` 事件。
4. 保留有按键策略入口；未来只需接入按键并启用板级配置。
5. 保持 STOP、SPI 心跳超时和故障停车逻辑不变；无按键配置不提供运行时 ESTOP 解锁。

## Task 3：文档、构建和真机验收

文件：

- 修改 `docs/hardware/wiring-v1.md`

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure
cmake --build build/stm32-live
python -m unittest discover -s tests
```

真机步骤：

1. 电机保持断电或车轮悬空。
2. 烧录 STM32 并恢复 ESP32 正式程序。
3. 顺时针逐格验证 `IDLE -> MANUAL -> AI -> IDLE`，逆时针验证反向循环。
4. 确认 OLED 标签和 ESP32 状态事件同步，`degraded_flags=0`、`rx_errors=0`。
5. 触发网页 STOP 后确认旋转不能解除 ESTOP，维护重启后回到 IDLE。

## 提交

实现、测试和接线说明作为一个固件提交推送到 `main`。不提交构建产物、临时探针或密钥。
