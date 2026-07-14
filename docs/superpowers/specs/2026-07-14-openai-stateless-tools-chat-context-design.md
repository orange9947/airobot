# OpenAI 无状态工具回传与对话上下文清除设计

- 日期：2026-07-14
- 状态：已确认
- 关联设计：`docs/superpowers/specs/2026-07-14-desktop-robot-design.md`

## 1. 背景

ESP32 当前通过 OpenAI Responses API 调用模型。首次文本响应和函数工具选择均可通过第三方中转完成，但工具执行结果使用 `previous_response_id` 回传时，中转返回 HTTP 400：该中转只在其 WebSocket v2 接口支持此字段。结果是模型能够选择 `robot_set_expression`，STM32 侧工具也能执行，但模型无法收到执行结果并生成最终回复。

官方 Responses 工具调用还支持无状态流程：第二次请求同时携带原始输入、模型输出项和 `function_call_output`。本设计统一使用该流程，使官方 OpenAI 和兼容中转不依赖服务端响应状态。

网页端已经提供 `DELETE /api/v1/chat`，但没有可见入口。用户需要在 AI 对话页清除 ESP32 内存上下文和当前网页聊天记录，并要求执行前二次确认。

## 2. 目标与非目标

### 2.1 目标

- 官方 OpenAI 和第三方 Responses 兼容中转共用一套工具回传逻辑。
- 工具执行结果回传不使用 `previous_response_id`。
- 保留完整模型输出项，兼容函数调用及 reasoning 项。
- 保持现有三轮工具调用上限、工具白名单和 STM32 安全约束。
- 在 AI 对话页提供可发现的上下文清除入口和确认对话框。
- 清除成功时同步清理服务端内存会话和浏览器聊天内容。

### 2.2 非目标

- 不改 DeepSeek Chat Completions 适配器。
- 不新增“官方/中转”兼容模式配置项。
- 不持久化聊天历史，不跨重启恢复会话。
- 不改变模型、Base URL、API Key 或 STM32 协议配置。
- 本轮真机验收不驱动电机。

## 3. OpenAI 无状态工具回传

### 3.1 Provider continuation

`OpenAIProvider` 的 continuation 从单一响应 ID 改为仅在 Provider 内部使用的结构：

```text
{
  input: [原始消息, 已返回的模型输出项, 已提交的工具结果, ...]
}
```

业务层仍只接收标准化的 `text`、`tool_calls` 和 continuation，不依赖 OpenAI 原始响应格式。continuation 不保存 API Key、Authorization 或 HTTP Header。

### 3.2 首次请求

1. `LlmService` 生成 system 消息和当前内存历史。
2. `OpenAIProvider.create_turn()` 以这些消息作为 `input`，并附带四个现有工具 Schema。
3. Provider 解析文本和 `function_call`，同时保留完整 `response.output`。
4. continuation 的 input 由首次请求 input 与完整 output 拼接得到。

保留完整 output 是为了在 reasoning 模型返回推理项时，将其与函数调用一起提交回模型，而不是只重建 `function_call`。

### 3.3 工具结果请求

1. `LlmService` 按现有白名单执行函数调用并生成结构化结果。
2. Provider 将每个结果编码为 `function_call_output`，使用原 `call_id` 关联。
3. 第二次请求的 `input` 为 continuation input 加本轮全部工具结果。
4. 请求继续携带模型、工具 Schema 和输出上限，但不包含 `previous_response_id`。
5. 若模型再次调用工具，将新请求 input 与新 `response.output` 继续累积到 continuation。

现有最多三轮工具调用限制不变。历史消息仍只保留用户与最终 assistant 文本；一次工具循环结束后不把原始 Provider 对象长期写入会话历史。

### 3.4 内存边界

无状态流程会增加一次工具循环内的请求体。风险由现有约束限制：历史最多 20 条、模型输出默认 256 token、工具参数有严格上限、响应正文最大 64 KiB、工具循环最多三轮。实现不深拷贝原始响应，只保留本轮所需的 input/output 列表。

## 4. 清除上下文交互

### 4.1 入口

AI 对话页标题右侧增加垃圾桶图标按钮，复用现有 `trash-2.svg`。按钮使用 `aria-label` 和 `title` 标记为“清除上下文”，尺寸与现有图标按钮一致。服务商和模型读数仍保留在标题操作区。

### 4.2 确认对话框

点击按钮打开自定义模态对话框：

- 标题：`清除对话上下文？`
- 说明：`网页聊天记录和机器人当前会话将被清除，且无法恢复。`
- 操作：`取消`、`确认清除`

对话框使用 `role="dialog"`、`aria-modal="true"` 和标题关联。打开后默认聚焦“取消”，避免键盘误确认；ESC、取消按钮和遮罩均关闭对话框并把焦点返回清除按钮。确认请求期间禁用两个按钮，防止重复提交。

### 4.3 清除结果

确认后调用现有 `DELETE /api/v1/chat`：

- 成功：清空 `#chat-stream`，关闭对话框，显示“上下文已清除”提示，并添加一条“新的对话已开始”机器人消息。
- 失败：保留网页聊天内容和对话框状态，恢复按钮，显示后端错误；不得表现为已经清除。
- 退出登录或服务商切换仍沿用现有会话行为，不自动触发该确认框。

## 5. 错误与安全

- API Key 仍只从 Secret 存储读取，不进入 continuation、日志或测试输出。
- HTTP 非 2xx、TLS、DNS 和 JSON 错误继续由 `HttpClientError` 统一传播。
- 不对已经执行的机器人动作自动重放。
- 本设计不采用“先发送 `previous_response_id`，失败后重试”，避免工具已执行后的模糊重试状态和额外延迟。
- 清除操作要求登录和 CSRF 校验，继续使用现有 WebService 安全检查。

## 6. 测试与验收

### 6.1 自动测试

- 首次 OpenAI 响应包含 `function_call` 时，Provider 正确解析调用。
- 第二次请求包含原始消息、完整响应 output 和 `function_call_output`。
- 第二次及后续请求不包含 `previous_response_id`。
- reasoning 项原样保留在下一次 input。
- 多轮工具调用继续累积上下文，且受三轮上限约束。
- DeepSeek 测试保持通过。
- 清除按钮、确认对话框和无障碍属性存在。
- 取消、ESC 和遮罩不会发 DELETE；确认只发送一次 DELETE。
- 删除失败保留聊天内容，成功清空内容并显示新会话消息。

### 6.2 浏览器验收

在 390x844、412x915 和 1440x900 视口验证：

- 清除按钮不挤压服务商/模型文字。
- 对话框完全位于安全区内，文字和按钮不重叠。
- 默认焦点、ESC、遮罩和焦点返回符合设计。

### 6.3 真机验收

1. 使用安全设备桩请求 OLED 表情工具，确认模型完成函数调用和最终回复。
2. 使用真实 STM32 请求 `robot_set_expression(happy)`，确认 OLED 改变且电机无动作。
3. 在网页清除上下文后发送依赖旧消息的问题，确认模型不再引用旧会话。
4. 确认 ESP32 与 STM32 心跳正常，SPI `rx_errors` 不增加。

## 7. 完成标准

- 当前第三方中转的表情工具调用不再返回 HTTP 400。
- 官方 Responses 无状态请求格式的自动测试通过。
- 用户可以通过确认对话框清除服务端和浏览器上下文。
- 全部 Python、C 和浏览器测试通过。
- 真机 OLED 工具闭环通过，测试期间电机不动作。
