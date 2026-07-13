# 模块化桌面机器人系统设计

- 状态：已确认
- 日期：2026-07-14
- 目标硬件：YD-ESP32-S3 N16R8、STM32F103C8T6、双 28BYJ-48 + ULN2003、SSD1306 OLED、EC11、1 MB W25Q Flash
- 首版交互：局域网页面文字对话、手动控制、设备配置、OLED/旋钮本地交互
- 后续扩展：语音输入输出、传感器和新增执行器

## 1. 目标与范围

本项目实现一台模块化桌面差速轮机器人。ESP32-S3 运行 MicroPython，负责联网、网页、大模型 API 和高层行为编排；STM32 负责电机、显示、旋钮、外置 Flash、实时状态和安全保护。两个 MCU 通过带校验、确认和心跳的 UART 二进制协议通信。

首版必须满足以下目标：

1. 网页可完成首次配网、OpenAI/DeepSeek 配置、服务商切换、文字对话、手动控制、状态查看和急停。
2. 大模型只能通过白名单工具控制机器人，不能执行任意 MicroPython，也不能直接生成 UART 数据。
3. STM32 独立完成动作边界检查、通信失联停车、急停和线圈释放，不依赖云端或网页保持安全。
4. OLED 显示表情、模式、网络/API 状态和错误码；EC11 用于模式切换、确认和软件急停。
5. 左右 28BYJ-48 采用开环差速控制，支持有限步数、方向、速度和加减速。
6. 模块通过稳定接口注册，后续增加语音、传感器或执行器时不需要重写主循环和大模型适配层。
7. 每个里程碑均能独立构建、模拟和硬件验收。

## 2. 非目标与已知限制

- 首版不包含麦克风、扬声器、语音识别或语音合成。
- 首版不提供公网远程访问、端口映射或云端设备中继。
- 左右轮没有编码器，因此无法检测打滑、堵转或保证实际距离精度。
- 28BYJ-48 和 ULN2003 适合低速原型，不适合高动态运动或承载较大的底盘。
- EC11 长按是软件急停，不是安全认证的硬件急停。后续应增加常闭开关，直接切断电机 5V 电源。
- 网页首版运行在可信局域网 HTTP 上。管理密码只能阻止未授权操作，不能抵御同一网络内的流量窃听。
- 标准 MicroPython 文件系统无法抵御设备被物理读取。API Key 不通过网页回显或日志输出，但设备丢失后仍应立即轮换密钥。
- 未增加电压采样电路前，页面不显示虚假的电池百分比。

## 3. 系统架构

```text
网页 / EC11 / 未来语音
          |
          v
+---------------------------------------------------------+
| ESP32-S3 / MicroPython                                  |
| 配置与密钥 | Wi-Fi/Web | LLM Provider | 工具策略 | UART |
+---------------------------------------------------------+
          | 版本化 UART 二进制协议
          v
+---------------------------------------------------------+
| STM32F103C8T6                                           |
| 协议路由 | 安全监督 | 运动服务 | 界面服务 | 资源存储   |
+---------------------------------------------------------+
          |
          v
双 ULN2003/28BYJ-48 | SSD1306 | EC11 | W25Q
```

### 3.1 ESP32-S3 职责

- 启动和任务监督。
- 首次配网热点、家庭 Wi-Fi 连接和断线恢复。
- 本地网页、REST API、WebSocket 状态流和管理会话。
- 配置与密钥的校验、原子写入和只写式密钥接口。
- OpenAI Responses API 与 DeepSeek Chat Completions API 适配。
- 统一大模型工具、参数策略、动作排队和结果回传。
- UART 编解码、ACK/重试、心跳、状态镜像和事件分发。
- STM32 模拟器与真实设备使用相同的传输接口。

ESP32 不是运动状态的权威源。网页点击和模型工具调用只产生请求，必须等 STM32 返回接受、完成、中止或拒绝事件后才更新为最终状态。

### 3.2 STM32 职责

- UART 帧校验、版本检查、去重、ACK/NACK 和消息路由。
- 全局模式、安全状态和设备状态的权威管理。
- 双轮半步序列、差速动作、加减速、任务超时和线圈释放。
- SSD1306 帧缓冲、表情动画与状态页。
- EC11 旋转、短按、长按、消抖和本地模式切换。
- W25Q JEDEC 探测、资源包读取、校验和双分区更新。
- 在 ESP、Wi-Fi、API 或网页不可用时保持本地安全。

### 3.3 线程与实时模型

ESP32 使用 `asyncio` 协作任务：UART 接收、心跳、Web 服务、LLM 请求、配置和状态广播均不得使用长时间忙等待。HTTPS 使用异步 TLS 流分块读写。DNS 解析仍可能短暂阻塞，因此硬实时保护必须留在 STM32。

STM32 首版不使用 RTOS。HAL 中断只处理 UART 字节、定时器节拍和必要的 GPIO 事件；业务逻辑运行在非阻塞主循环状态机中。TIM2 提供运动调度节拍，SysTick 提供毫秒系统时间。SSD1306 和 W25Q 操作分块执行，不允许长时间关闭中断。

## 4. 硬件连接与供电

### 4.1 ESP32-S3 到 STM32 UART

| 信号 | YD-ESP32-S3 | STM32F103C8T6 | 说明 |
| --- | --- | --- | --- |
| ESP TX | GPIO17 | PA10 / USART1_RX | 3.3V 逻辑 |
| ESP RX | GPIO18 | PA9 / USART1_TX | 3.3V 逻辑 |
| GND | GND | GND | 必须共地 |

GPIO17/18 在 YD-ESP32-S3 板上引出，避开 GPIO19/20 原生 USB、启动脚和板载存储相关连接。接线前仍需按 V1.3 实物丝印复核。

### 4.2 STM32 引脚分配

| 模块 | 信号 | STM32 引脚 |
| --- | --- | --- |
| 左 ULN2003 | IN1、IN2、IN3、IN4 | PA0、PA1、PA2、PA3 |
| 右 ULN2003 | IN1、IN2、IN3、IN4 | PB0、PB1、PB10、PB11 |
| W25Q | CS、SCK、MISO、MOSI | PA4、PA5、PA6、PA7 |
| SSD1306 | SCL、SDA | PB6、PB7 / I2C1 |
| EC11 | A、B、SW | PB12、PB13、PB14 |
| UART | TX、RX | PA9、PA10 / USART1 |
| ST-Link | SWDIO、SWCLK | PA13、PA14 |
| 状态灯 | LED | PC13，可选 |

约束：

- OLED 使用 3.3V 供电，使 I2C 上拉保持 3.3V。
- W25Q 使用 3.3V 供电，并在芯片或模块附近放置 100 nF 去耦电容。
- ULN2003 输入由 STM32 3.3V GPIO 驱动，电机端 VCC 使用 5V；不能把 5V 接入 GPIO。
- 左右电机的安装方向、相序和正方向均通过校准配置修正，不通过修改运动业务代码修正。

### 4.3 电源拓扑

成品 18650 电池升压模块必须验证能持续输出稳定的 5V/3A，并具有充电、过充、过放和短路保护。5V 输出经过总开关后分支供电：

```text
5V 电池输出
  +-- 电机支路 -> ULN2003 左/右 -> 28BYJ-48 左/右
  +-- 逻辑支路 -> ESP32 VIN
  +-- 逻辑支路 -> STM32 5V
所有 GND 在电源入口附近汇聚
```

建议在每块 ULN2003 附近增加 470 uF 电解电容，在逻辑支路增加至少 100 uF 储能电容，并保留 100 nF 高频去耦。首次测试时轮子悬空，检查双电机同时启动是否导致 ESP32 或 STM32 复位。电源模块温升、电池温升和堵转电流必须纳入台架验收。

## 5. 全局状态机与控制权

### 5.1 状态

| 状态 | 行为 |
| --- | --- |
| `BOOT` | GPIO 保持安全默认值，电机线圈关闭 |
| `SELF_TEST` | 检查 UART、OLED、EC11、W25Q 和内部配置 |
| `IDLE` | 电机线圈释放，允许配置、聊天和选择模式 |
| `MANUAL` | 仅接受网页手动动作和 STOP |
| `AI` | 仅接受策略层验证后的模型动作和 STOP |
| `ESTOP` | 清空队列、停止步进、释放线圈，拒绝远程运动 |
| `FAULT` | 显示错误码并保持电机关闭，等待维护或重启自检 |

OLED 或 W25Q 故障属于降级标志，不单独增加全局状态；EC11 按键故障会进入 `FAULT`，因为本体软件急停入口已经失效。

### 5.2 状态转换

- `BOOT -> SELF_TEST -> IDLE`。STM32 尚未收到第一次有效 `HELLO` 时不启动心跳超时计时。
- `IDLE <-> MANUAL`、`IDLE <-> AI` 由旋钮旋转选择、短按确认；网页可提出模式切换请求，最终由 STM32 确认。
- `MANUAL <-> AI` 必须经由停车和清空旧队列，不允许动作跨模式延续。
- 任意运行状态收到本体长按、网页 STOP、会话建立后的 UART 心跳超时或安全监督错误时进入 `ESTOP`。
- `ESTOP -> IDLE` 需要危险条件消失、长按按键已释放、UART 链路健康，并在机器人本体再次短按确认。远程请求不能单独解除急停。
- 初始化失败、协议持续不兼容或 EC11 故障进入 `FAULT`。恢复后通过维护重启重新自检。

### 5.3 并发和抢占

- STOP 优先级最高，不进入普通队列。
- 同一时刻只有一个差速运动任务；左右轮是一个原子动作。
- AI 动作队列最多 3 项。超过上限的工具调用返回 `queue_full`。
- 表情任务可以和运动并行，但模式改变或急停会切换到安全状态表情。
- 每个运动命令必须包含有限步数和不超过 2000 ms 的超时。
- 重复的 `boot_id + seq + command_id` 只返回既有结果，不重复执行动作。

## 6. UART 二进制协议

### 6.1 物理层

- USART1，115200 baud，8N1，无硬件流控。
- 单帧最大载荷 256 字节。
- 所有多字节整数使用小端序。
- 命令 ACK 目标时间不超过 100 ms。
- ESP 每 250 ms 发送一次心跳；建立会话后，STM 连续 750 ms 未收到有效帧即进入 `ESTOP`。

### 6.2 帧格式

| 字段 | 大小 | 说明 |
| --- | ---: | --- |
| SOF | 2 B | 固定 `0xAA 0x55` |
| VER | 1 B | 协议主版本，首版为 1 |
| FLAGS | 1 B | 请求 ACK、响应、事件等标志 |
| TYPE | 2 B | 消息类型 |
| SEQ | 2 B | 发送方会话内递增序号 |
| LEN | 2 B | 载荷长度，0 到 256 |
| PAYLOAD | LEN B | 按消息类型定义的固定字段 |
| CRC16 | 2 B | CRC16-CCITT，覆盖 VER 至 PAYLOAD |

解析器遇到非法长度或 CRC 错误时丢弃当前候选帧，继续扫描下一个 SOF。缓冲区设固定上限，不根据未验证的 LEN 动态分配内存。

### 6.3 消息域

| 范围 | 域 | 首版消息 |
| --- | --- | --- |
| `0x01xx` | 系统 | `HELLO_REQ`、`HELLO_RSP`、`HEARTBEAT`、`GET_STATE`、`STATE_SNAPSHOT` |
| `0x02xx` | 控制 | `SET_MODE`、`MOVE_WHEELS`、`STOP`、`SET_EXPRESSION`、`SET_RUNTIME_CONFIG` |
| `0x03xx` | 事件 | `ACK`、`NACK`、`MOTION_STARTED`、`MOTION_DONE`、`MOTION_ABORTED`、`MODE_CHANGED`、`FAULT_EVENT` |
| `0x04xx` | 资源 | `FLASH_INFO`；资源包写入消息在 M4 启用 |

协议消息 ID、字段顺序、字段类型和固定长度保存在 `protocol/messages.json`。生成器输出：

- `protocol/generated/protocol_ids.py`
- `protocol/generated/protocol_ids.h`
- C/Python 共用的黄金帧测试向量
- 消息长度静态断言

生成文件进入版本控制，使固件构建不依赖现场安装代码生成工具；CI 会验证生成结果没有漂移。

### 6.4 确认和重试

1. ESP 发送带 `SEQ` 和 `command_id` 的命令。
2. STM 完成 CRC、版本、模式和参数边界检查。
3. STM 在 100 ms 内返回 ACK 或带错误码的 NACK。
4. ESP 超时后使用相同序号重发，最多 3 次。
5. STM 对重复命令返回原 ACK 或最终动作结果，不重复运动。
6. 动作开始、完成、中止和故障通过异步事件上报。
7. STM 每秒发送状态快照，弥补非可靠状态事件丢失。

协议不兼容时允许交换最小 `HELLO` 错误信息，但禁止进入运动模式。

## 7. 运动控制设计

### 7.1 动作模型

大模型和网页使用高层动作：方向、速度百分比和时长。ESP 将其转换为统一 `MOVE_WHEELS`：

- `command_id: u32`
- `left_steps: i32`
- `right_steps: i32`
- `max_rate_sps: u16`
- `accel_sps2: u16`
- `timeout_ms: u16`

STM 使用自己的硬上限再次钳制所有字段。前进和后退使用同号轮步数，原地左/右转使用反号轮步数；安装方向通过每轮 `invert` 配置修正。

### 7.2 开环调度

- 使用 8 相半步序列驱动 28BYJ-48。
- TIM2 以 1 kHz 节拍更新双轮相位累加器。
- 加速度规划每 10 ms 更新一次目标步频，避免从静止直接跳到高频。
- 首次标定使用保守速率，经过台架失步测试后再提高软上限；固件硬上限不允许由网页或大模型修改。
- `steps_per_revolution`、左右轮反向、轮径和轮距是设备校准配置。没有编码器时，这些参数只用于估算，不构成闭环里程计。
- 动作完成、超时、急停或故障后立即停止调度，并按配置在短暂保持后释放线圈。急停不等待保持时间。

## 8. OLED、旋钮和资源 Flash

### 8.1 OLED

- SSD1306，128x64，I2C1，400 kHz。
- 使用 1024 字节 1bpp 帧缓冲。
- 状态页显示模式、ESP 链路、Wi-Fi、模型配置状态和错误码。
- 表情页使用基础表情：`NEUTRAL`、`HAPPY`、`SAD`、`THINKING`、`SURPRISED`、`SLEEPY`、`ESTOP`、`FAULT`。
- 动画目标刷新率为 10 fps；资源或 I2C 异常时降低刷新率，不影响运动定时器。

### 8.2 EC11

- A/B 相采用定时采样和状态表消抖，不在 GPIO 中断里修改全局模式。
- 旋转选择 `IDLE`、`MANUAL`、`AI`。
- 短按确认模式或在安全条件满足时解除 ESTOP。
- 长按 1.5 秒触发 ESTOP；长按一经识别不等待松手。
- 启动时检测按键卡住。持续按下或输入异常会进入 `FAULT`。

### 8.3 W25Q 资源包

1 MB W25Q 连接在 STM32 SPI1。驱动启动时读取 JEDEC ID 并验证容量。资源格式使用主机工具生成的只读包，避免在 STM32 上实现完整文件系统。

分区：

- 扇区 0：超级块、活动分区、代数和 CRC。
- Bank A：约 508 KiB。
- Bank B：约 512 KiB。

更新时写入非活动 Bank，完成全包 CRC 校验后再原子切换超级块。掉电后选择校验有效且代数最高的 Bank。每个动画资源包含名称、帧率、帧数、偏移、长度和 CRC；帧数据使用 1bpp RLE。即使两份 Bank 均无效，固件内置的基础表情仍可使用。

## 9. 大模型适配与工具调用

### 9.1 Provider 接口

统一接口：

```text
configure(public_config, secret_ref)
create_turn(messages, tool_schemas) -> ProviderTurn
submit_tool_results(turn, results) -> ProviderTurn
cancel(turn)
health() -> ProviderHealth
```

`ProviderTurn` 只暴露标准化文本、`ToolCall[]`、结束原因、请求 ID、用量和错误，不把服务商原始响应传播到业务层。

OpenAI 适配器使用 Responses API：函数工具由 JSON Schema 定义，解析 `function_call` 输出，并使用 `call_id` 提交 `function_call_output`。DeepSeek 适配器使用 Chat Completions：解析 `choices[0].message.tool_calls`，再追加 assistant 工具调用消息和带 `tool_call_id` 的 tool 消息。服务商切换会开始新的内存会话，避免混用不兼容的上下文标识。

配置字段：

- `provider`: `openai` 或 `deepseek`
- `base_url`: 使用各服务商默认值或用户配置的兼容端点
- `model`: 必填的用户配置模型 ID，不在固件中硬编码“最新模型”
- `api_key`: 各服务商独立保存，只写不回显
- `request_timeout_s`: 默认 60，允许 10 到 120
- `max_output_tokens`: 默认 256，允许 64 到 1024

响应正文硬限制为 64 KiB。连接、TLS、HTTP 状态、JSON 和工具参数错误使用统一错误类型。429 和 5xx 最多重试 2 次并指数退避；已经被 STM 接受的动作绝不因为云端重试而重复执行。

### 9.2 白名单工具

| 工具 | 参数 | 策略 |
| --- | --- | --- |
| `robot_move` | `direction`、`speed_percent`、`duration_ms` | 仅 AI 模式；方向为枚举；10%-100%；100-2000 ms；再受设备软硬上限钳制 |
| `robot_stop` | 无 | 任意模式允许；最高优先级 |
| `robot_set_expression` | `expression` | 仅允许资源清单中的枚举名称 |
| `robot_set_mode` | `mode` | 模型仅能请求 `IDLE`；进入 MANUAL/AI 需要用户操作 |

工具调用流程：

1. Provider 返回一个或多个 ToolCall。
2. ESP 校验工具名、JSON 类型、枚举、范围、当前模式和队列容量。
3. 无效调用不发送 UART，直接把结构化错误结果返回模型和网页。
4. 有效运动转换为设备命令并等待 ACK；动作完成或中止后形成工具结果。
5. 工具结果提交回同一 Provider turn，取得最终自然语言回复。
6. 网页分别显示模型文本和动作事件，避免把“模型打算执行”误显示为“设备已经完成”。

聊天历史默认只保存在 RAM，限制为最近 20 条标准化消息，并受请求大小限制。重启、用户清空或服务商切换都会清除当前会话。

## 10. 网络、网页与配置安全

### 10.1 首次配网

1. 未配置 Wi-Fi 或连续连接失败时，ESP 启动 `Robot-XXXX` 配置热点。
2. OLED 显示热点名、随机 WPA2 口令和配置地址。
3. 用户选择家庭 Wi-Fi 并创建设备管理密码。
4. ESP 原子保存配置、关闭热点并进入 STA 模式。
5. OLED 显示局域网 IP；用户在局域网内配置 OpenAI/DeepSeek。

热点和 STA 模式不同时长期开放。恢复配网需要本体旋钮确认，避免远程请求把设备切换为开放配置状态。

如果 OLED 自检失败，热点名、一次性口令和配置地址只输出到 USB 串口；设备保持电机关闭，直到完成首次配置或 OLED 恢复。管理密码不保存原文，首版保存 16 字节随机盐和 PBKDF2-HMAC-SHA256 派生值，迭代次数固定为 20000。

### 10.2 页面结构

- `控制台`：设备状态、模式分段控件、方向控制、速度/时长和固定急停按钮。
- `AI 对话`：消息、服务商状态、标准化 ToolCall 事件和取消请求。
- `设备诊断`：ESP/STM 版本、心跳、错误计数、最近故障和自检结果。
- `设置`：Wi-Fi、管理密码、OpenAI、DeepSeek、模型、Base URL、运动软限制和校准。

桌面与手机使用同一响应式页面。静态资源随固件部署，不依赖 CDN。网页源码使用原生 ES 模块和精简的选定图标资源，构建产物写入 ESP 的 `/www` 目录。

### 10.3 本地 API

| 方法与路径 | 用途 |
| --- | --- |
| `POST /api/v1/session/login` | 创建管理会话 |
| `POST /api/v1/session/logout` | 注销会话 |
| `GET /api/v1/status` | 获取初始状态快照 |
| `GET /api/v1/config` | 返回公开配置和密钥是否已配置 |
| `PUT /api/v1/config` | 更新公开配置 |
| `PUT /api/v1/secrets` | 写入或清除指定服务商密钥，不返回原值 |
| `POST /api/v1/mode` | 请求切换模式 |
| `POST /api/v1/motion` | 手动有限动作 |
| `POST /api/v1/stop` | 无条件请求急停 |
| `POST /api/v1/chat` | 发起一个模型 turn |
| `DELETE /api/v1/chat` | 清除内存会话 |
| `GET /api/v1/events` | WebSocket 状态和动作事件流 |

除登录、首次配置和健康检查外，接口要求管理会话。会话使用随机令牌、HttpOnly 和 SameSite Cookie，并对修改请求校验 CSRF 令牌。登录和配置接口限速。错误日志只记录服务商、HTTP 状态和请求 ID，不记录 Authorization、完整请求正文或 API Key。

### 10.4 配置存储

- `/config/device.json`：schema 版本、Wi-Fi、页面设置、Provider 公开配置和运动软限制。
- `/config/secrets.json`：管理密码派生值和 API Key，仅由配置服务访问。
- 每次更新写临时文件，校验后 rename；保留一份上次有效备份。
- 读取时验证 schema、类型、范围和校验值；无效配置进入安全配网模式，电机保持关闭。
- 本地示例配置和真实密钥文件均由 `.gitignore` 排除。

## 11. 错误处理与恢复

| 故障 | 立即行为 | 恢复 |
| --- | --- | --- |
| 会话建立后 ESP 失联或 UART 超时 | 进入 ESTOP、停轮、清队列、释放线圈 | 链路恢复且本体短按确认 |
| CRC 错误、半帧、超长帧 | 丢弃候选帧并累计计数 | 后续有效帧自动恢复；持续超限进入 FAULT |
| MCU 重启 | 输出保持安全默认值，使用新 boot_id 建立会话 | 重新同步配置和状态；旧命令失效 |
| Wi-Fi/API 超时、鉴权或余额错误 | 禁止新 AI 动作，保留本地显示、手动切换和急停 | 指数退避重连，网页/OLED 显示原因 |
| W25Q 无效 | 使用内置表情并标记降级 | 后台重新探测或重新刷写资源包 |
| OLED 无响应 | 保留网页状态，记录降级错误 | 维护模式重试初始化 |
| EC11 按键异常 | 进入 FAULT 并禁止运动 | 修复硬件后重启自检 |
| 电机动作超时 | 中止动作、释放线圈并上报 | 返回 IDLE 或根据错误严重度进入 ESTOP |
| 内存不足 | 取消当前网络请求，保留 UART 和安全任务 | 清理会话并重建网络服务；重复发生则安全重启 ESP |

错误码按域编号并同时提供稳定机器码与简短显示文本。网页可以显示详细诊断，OLED 只显示状态和短错误码。

## 12. 仓库结构与模块接口

```text
desktop-robot/
├── protocol/
│   ├── messages.json
│   ├── generate.py
│   ├── generated/
│   └── golden/
├── firmware/
│   ├── esp32/
│   │   ├── boot.py
│   │   ├── main.py
│   │   ├── core/
│   │   ├── services/
│   │   ├── providers/
│   │   ├── tools/
│   │   └── transport/
│   └── stm32/
│       ├── Core/
│       ├── App/
│       ├── Drivers/
│       └── CMakeLists.txt
├── web/
├── tools/
├── tests/
│   ├── unit/
│   ├── contract/
│   ├── integration/
│   └── hardware/
└── docs/
```

ESP 服务接口统一包含 `start()`、`stop()`、`health()`；通过事件总线交换不可变事件，不直接访问其他服务的内部状态。Provider、传输和配置存储通过构造参数注入，测试使用模拟实现。

STM 模块统一包含：

```text
init(context) -> status
tick(now_ms)
handle_message(message) -> result
get_status(output)
shutdown(reason)
```

驱动层只提供总线和 GPIO 操作；App 层拥有状态机。模块在编译期注册，启动阶段按依赖顺序初始化。初始化失败必须返回明确状态，不能静默跳过安全相关模块。

工具链：

- ESP32 固件基于 MicroPython v1.28.0，并为 N16R8 固定分区和 PSRAM 配置。
- STM32 使用 STM32Cube HAL、C11、CMake 和 `arm-none-eabi-gcc`；CubeMX 只维护时钟、引脚和 HAL 初始化生成区。
- ST-Link 通过 OpenOCD 或 STM32CubeProgrammer 烧录；CI 只做可复现构建和主机测试。
- Python 主机工具使用锁定依赖的虚拟环境；运行时 MicroPython 代码不依赖 CPython 专用包。

## 13. 测试策略

### 13.1 自动测试

- CRC、帧解析、非法长度、分片输入、粘包和重新同步。
- C 与 Python 对所有黄金帧双向编解码一致。
- 状态转换、急停、模式切换、命令去重和队列边界。
- 高层方向到左右轮步数的换算、软硬上限和动作超时。
- OpenAI/DeepSeek 成功响应、多个工具调用、无工具文本、429、5xx、超时、截断 JSON 和非法参数。
- 配置迁移、原子写入、备份回退和敏感字段不回显。
- STM 模拟器与 ESP UART 网关的伪终端集成。
- 模拟 LLM HTTP 服务与网页主要工作流。
- 网页桌面/移动视口、文本溢出、断线重连、急停和无障碍名称。
- 仓库、构建日志、串口日志和网页响应的密钥扫描。

### 13.2 故障注入

- 丢帧、重复帧、错误 CRC、随机字节、延迟 ACK 和串口断开。
- ESP 或 STM 在动作接受前、执行中和完成事件前重启。
- Wi-Fi 断开、DNS 阻塞、TLS 失败、API 鉴权失败和响应过大。
- W25Q 掉电更新、单 Bank CRC 失败和两 Bank 均无效。
- OLED NACK、EC11 卡键和主循环延迟。

### 13.3 硬件台架

1. 轮子悬空验证左右方向、半步相序和 STOP。
2. 单轮和双轮逐级提高速率，记录失步起点并设置保守硬上限。
3. 动作中拔掉 UART，确认 750 ms 内进入 ESTOP。
4. 动作中长按 EC11，确认停止不依赖网页或 Wi-Fi。
5. 双电机启动和短时堵转条件下监测 5V、电池、升压模块、ULN2003 和稳压器温升。
6. 连续运行网页、UART 心跳、OLED 动画和周期 API 请求，监测内存增长、自动恢复和意外复位。

## 14. 分阶段交付与验收

### M0：仓库与协议基础

交付：目录结构、构建脚本、协议清单/生成器、C/Python 编解码、黄金帧、STM 模拟器和 CI。

验收：

- C/Python 契约测试全部通过。
- 模拟器能接受 HELLO、心跳、模式、MOVE 和 STOP，并产生状态事件。
- 代码和日志密钥扫描通过。

### M1：STM32 实时固件

交付：HAL 初始化、UART、状态机、安全监督、双轮、SSD1306、EC11、W25Q 探测和内置表情。

验收：

- 串口诊断工具可驱动所有首版消息。
- 轮子悬空完成前进、后退、原地转向和有限动作。
- 重复命令不重复运动；失联、长按和 STOP 均能可靠停车。
- W25Q 缺失时仍能使用内置表情并报告降级状态。

### M2：ESP32 与网页

交付：MicroPython 固件、配网、配置、UART 网关、网页控制台、手动控制、状态流和急停。

验收：

- 手机和桌面浏览器均可完成首次配网和管理登录。
- 网页命令以 STM 事件为最终状态，不使用乐观完成状态。
- 页面刷新、浏览器断开或 Wi-Fi 断开不会造成持续运动。
- 密钥字段只写不回显。

### M3：大模型

交付：OpenAI/DeepSeek Provider、白名单工具、会话、工具结果闭环、超时和错误展示。

验收：

- 两个 Provider 分别通过模拟服务和真实 API 冒烟测试。
- 服务商切换不需要修改工具或 UART 业务代码。
- 非法工具、超范围参数和错误模式均在 ESP/STM 双层被拒绝。
- API 失败不会启动新动作，也不会泄露密钥。

### M4：资源与交付完善

交付：W25Q A/B 资源包、主机打包/上传工具、表情动画、完整接线/烧录/故障文档和长稳测试。

验收：

- 资源更新中断后能从旧 Bank 启动。
- 无效资源自动使用固件内置表情。
- 完成连续运行、故障注入、温升和恢复测试记录。

### M5：未来语音插件

语音模块只实现新的输入/输出适配器，复用标准化聊天消息、Provider、工具策略、状态事件和网页配置。语音不能建立绕过白名单工具或 STM 安全监督的控制通道。

## 15. 设计决策摘要

- 双 MCU：ESP32 管联网和智能，STM32 管实时和安全。
- STM32 是运动、安全和模式状态的权威源。
- UART 使用版本化二进制帧、CRC16、ACK、重试、心跳和命令去重。
- STM32 使用非 RTOS 状态机，ESP32 使用 asyncio 服务。
- OpenAI 与 DeepSeek 是独立 Provider，工具层和设备层不感知服务商格式。
- 大模型只使用 4 个白名单工具，STM32 对运动参数进行最终钳制。
- 网页默认仅可信局域网访问，API Key 只写不回显。
- 外置 W25Q 存放 A/B 表情资源包；固件内置表情作为永久降级路径。
- 首先交付 M0/M1，再接入 Web 和大模型；语音留在稳定接口之后。

## 16. 参考资料

- [OpenAI Function Calling](https://developers.openai.com/api/docs/guides/function-calling.md)
- [DeepSeek Tool Calls](https://api-docs.deepseek.com/guides/tool_calls)
- [MicroPython asyncio TCP/TLS streams](https://docs.micropython.org/en/latest/library/asyncio.html#tcp-stream-connections)
- [MicroPython v1.28.0 release](https://github.com/micropython/micropython/releases/tag/v1.28.0)
- [VCC-GND YD-ESP32-S3 hardware repository](https://github.com/vcc-gnd/YD-ESP32-S3)
