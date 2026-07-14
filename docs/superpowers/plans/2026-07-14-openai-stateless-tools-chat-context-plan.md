# OpenAI 无状态工具回传与上下文清除实施计划

- 日期：2026-07-14
- 设计依据：`docs/superpowers/specs/2026-07-14-openai-stateless-tools-chat-context-design.md`
- 范围：OpenAI Provider、LLM 单元测试、AI 对话页、前端静态契约与真机部署

## Task 1：锁定 HTTP 400 回归

文件：

- 修改 `tests/test_llm_services.py`
- 修改 `firmware/esp32/providers/openai_provider.py`

步骤：

1. 先扩展 OpenAI 工具测试，响应中加入 reasoning 项和 `function_call`。
2. 断言工具结果请求包含首次消息、完整模型 output 和 `function_call_output`。
3. 断言任何请求均不包含 `previous_response_id`。
4. 运行目标测试并确认它先因当前 ID 回传实现失败。
5. 将 Provider continuation 改为累积 input 的无状态结构，并使多轮工具调用继续累积。
6. 运行全部 LLM 测试，确认 DeepSeek 行为不变。

## Task 2：定义上下文清除界面契约

文件：

- 修改 `tests/test_esp_bundle.py`
- 修改 `web/index.html`
- 修改 `web/styles.css`

步骤：

1. 先添加失败的静态契约测试，要求清除按钮、确认层、对话框标题和两个操作按钮存在。
2. 在 AI 对话标题操作区加入复用 `trash-2.svg` 的图标按钮。
3. 添加自定义 `role="dialog"` 确认层，默认隐藏。
4. 为桌面和手机实现不溢出、不遮挡的确认框布局，保持现有控制台视觉语言。

## Task 3：实现确认与清除行为

文件：

- 修改 `web/app.js`
- 按需扩展 `tests/test_web_service.py`

步骤：

1. 实现打开、取消和关闭确认框的函数。
2. 打开时默认聚焦取消按钮；ESC 和遮罩关闭后焦点返回清除按钮。
3. 确认时只发送一次 `DELETE /api/v1/chat`，请求期间禁用操作按钮。
4. 成功后清空聊天流、加入新会话消息并显示提示。
5. 失败时保留聊天流、恢复按钮并显示错误。
6. 验证现有 WebService DELETE 路由继续要求认证和 CSRF，无需修改后端接口。

## Task 4：自动化与视觉验收

验证命令：

```bash
python -m unittest discover -s tests
ctest --test-dir build/host --output-on-failure
node --check web/app.js
```

使用 Playwright 检查 390x844、412x915 和 1440x900：

1. 清除按钮可见且不挤压服务商和模型读数。
2. 对话框完全位于视口内，默认焦点在取消按钮。
3. 取消、ESC 和遮罩不发送 DELETE。
4. 确认成功后聊天内容被替换为新会话消息。
5. 模拟 DELETE 失败时旧聊天内容仍存在。

## Task 5：真机部署与安全工具验收

步骤：

1. 提交并推送实现，部署 ESP32 bundle，保留 `/config`。
2. 复位 STM32 和 ESP32，确认网页服务恢复。
3. 先用安全设备桩执行 `robot_set_expression(happy)` 工具闭环，确认不再出现 HTTP 400。
4. 再通过真实设备仅切换 OLED 表情，明确要求不移动。
5. 检查 STM32 心跳、状态、故障码和 SPI `rx_errors`。
6. 不发送 `robot_move`，不进行电机验收。

## 提交

计划、实现和临时真机探测脚本分开处理。测试脚本和截图放在 `/tmp`，不进入 Git；API Key、管理密码和 Wi-Fi 密码不得进入源码、日志或提交。
