# 登录反馈、ESTOP 恢复与动态表情实施计划

- 日期：2026-07-14
- 设计依据：`docs/superpowers/specs/2026-07-14-login-estop-animated-face-design.md`
- 范围：协议、STM32 状态与 OLED、ESP32 服务、网页交互、自动化和真机部署

## Task 1：新增 CLEAR_ESTOP 协议契约

文件：

- 修改 `protocol/messages.json`
- 重新生成 `protocol/generated/protocol_ids.py`
- 重新生成 `protocol/generated/protocol_ids.h`
- 重新生成 `protocol/generated/protocol_layouts.py`
- 重新生成 `protocol/generated/protocol_layouts.h`
- 重新生成 `protocol/golden/golden_slots.json`
- 修改 `tests/test_protocol_python.py`
- 修改 `tests/unit/test_protocol_c.c`

步骤：

1. 先添加失败断言，要求 `MSG_CLEAR_ESTOP` 和 4 字节 command ID 布局存在。
2. 在命令消息段加入 `CLEAR_ESTOP`，使用下一个稳定消息 ID。
3. 运行协议生成器并检查生成差异只包含新消息。
4. 运行 Python/C 协议测试。

## Task 2：实现 STM32 安全恢复

文件：

- 修改 `firmware/stm32/App/Core/robot_state.h`
- 修改 `firmware/stm32/App/Core/robot_state.c`
- 修改 `firmware/stm32/App/app.c`
- 修改 `firmware/stm32/App/Protocol/robot_spi_slot.c`
- 修改 `tests/unit/test_robot_core.c`
- 修改 `tools/stm_simulator.py`
- 修改 `tests/test_stm_simulator.py`

步骤：

1. 将 `local_confirm` 语义改成 `operator_confirm` 并扩展状态测试。
2. 添加 CLEAR_ESTOP handler：只允许 ESTOP、健康链路、非 active 运动。
3. 成功时回到 IDLE、故障码清零、表情恢复 neutral；失败返回 BAD_STATE。
4. ESTOP/FAULT 下拒绝普通 SET_EXPRESSION，避免 ACK 但画面被覆盖。
5. 更新 payload 长度路由、模拟器与重复命令测试。
6. 构建并运行主机 CTest。

## Task 3：贯通 ESP32 和 Web API

文件：

- 修改 `firmware/esp32/services/device_service.py`
- 修改 `firmware/esp32/services/web_service.py`
- 修改 `firmware/esp32/services/llm_service.py`
- 修改 `tests/test_esp_services.py`
- 修改 `tests/test_web_service.py`
- 修改 `tests/test_llm_services.py`

步骤：

1. 先添加 DeviceService CLEAR_ESTOP 模拟链路测试。
2. 实现 `clear_estop()` 并等待 ACK/NACK。
3. 新增 `POST /api/v1/estop/clear`，要求登录和 CSRF。
4. 单个机器人工具执行失败时生成结构化工具结果并继续模型回合。
5. 验证 AI 工具层没有 CLEAR_ESTOP 能力。

## Task 4：实现确定性的 STM32 表情动画器

文件：

- 新增 `firmware/stm32/App/Ui/face_animator.h`
- 新增 `firmware/stm32/App/Ui/face_animator.c`
- 修改 `firmware/stm32/App/Ui/ui_service.h`
- 修改 `firmware/stm32/App/Ui/ui_service.c`
- 修改 `firmware/stm32/App/app.c`
- 修改 `firmware/stm32/CMakeLists.txt`
- 修改 `tests/CMakeLists.txt`
- 新增 `tests/unit/test_face_animator.c`

步骤：

1. 先编写固定种子的主机测试，覆盖情绪稳定、眨眼、视线、嘴形、时间边界和安全覆盖。
2. 实现 xorshift32、事件调度和 `face_pose` 输出。
3. UI 只在姿态变化时标脏，继续使用分页刷新。
4. 为六种普通情绪绘制明显不同的眼睛和嘴形；ESTOP/FAULT 保持固定。
5. 添加独立 `face_animator` CTest，并检查 STM32 Flash/RAM 尺寸。

## Task 5：实现登录反馈状态机

文件：

- 修改 `web/index.html`
- 修改 `web/styles.css`
- 修改 `web/app.js`
- 修改 `tests/test_esp_bundle.py`

步骤：

1. 先添加失败的静态契约，要求登录状态区域、busy 状态和固定按钮结构存在。
2. 抽取 READY、VERIFYING、LOADING 状态更新函数。
3. 提交后立即禁用输入和按钮；配置、状态加载完成前不隐藏登录层。
4. 区分 401、409、网络错误、加载错误和 20 秒超时。
5. 登录成功后清除密码；加载失败时复用现有会话重试。

## Task 6：实现网页 ESTOP 恢复确认

文件：

- 修改 `web/index.html`
- 修改 `web/styles.css`
- 修改 `web/app.js`
- 修改 `tests/test_esp_bundle.py`

步骤：

1. 在当前状态面板加入默认隐藏的“解除急停”按钮。
2. 仅当设备在线且状态为 ESTOP 时显示。
3. 添加二次确认、默认取消焦点、ESC/遮罩关闭和焦点返回。
4. 确认时调用新 API，成功后刷新状态，失败保留 ESTOP。
5. 保持顶部急停按钮始终可见。

## Task 7：自动化、浏览器和真机验收

验证命令：

```bash
python -m unittest discover -s tests
ctest --test-dir build/host --output-on-failure
node --check web/app.js
cmake --build build/stm32-live
```

Playwright 在 390x844、412x915 和 1440x900 检查：

1. 延迟认证期间登录层不消失，状态和按钮不移位。
2. 错误密码与网络错误可区分，且没有页面导航。
3. ESTOP 恢复按钮和确认框只在正确状态出现。
4. 确认成功/失败、取消、ESC、遮罩和焦点返回均正确。

真机顺序：

1. 构建并烧录 STM32，再部署 ESP32 bundle，保留 `/config`。
2. 结束所有串口探测后复位 ESP32，等待正式 Web 服务启动，再复位 STM32。
3. 验证手机登录反馈和错误密码。
4. 触发/解除 ESTOP，确认恢复到 IDLE 且电机不动作。
5. 依次测试六种普通情绪并连续观察随机细节至少 60 秒。
6. 确认故障码为 0、SPI `rx_errors` 不增加。

## 提交与安全

计划与实现分开提交。临时截图和探测脚本放 `/tmp`，不进入 Git。不得把 API Key、管理密码、Wi-Fi 密码写入源码、文档、日志或提交。真机验收不发送 `robot_move`。
