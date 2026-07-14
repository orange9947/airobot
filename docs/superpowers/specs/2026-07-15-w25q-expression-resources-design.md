# W25Q 表情资源包第一阶段设计

## 1. 背景

当前 STM32 固件能读取 W25Q JEDEC ID、识别容量并读取原始字节，但尚无页编程、扇区擦除、A/B 切换、资源目录或上传链路。OLED 表情全部由固件程序化绘制。

已有链路有两个硬边界：

- ESP32 Web 服务单次 HTTP 请求体上限是 8 KiB，并会整体读入 RAM。
- ESP32 与 STM32 的 SPI 邮箱槽载荷固定为 256 字节。

因此资源系统必须从主机到 W25Q 全链路分块，不能将完整资源包缓存在 ESP32 或 STM32 RAM。当前运动模块真机校准暂停；本功能不修改运动参数或执行任何电机命令。

## 2. 目标

1. 从 `128 x 64` 单色 PNG 和 JSON 清单生成确定性表情资源包。
2. 在 1 MiB W25Q 中实现可掉电恢复的 A/B 资源 Bank，更新失败时保留旧 Bank。
3. 通过受认证 HTTP API、ESP32 `ResourceService` 和 SPI 分块命令上传资源包。
4. 使六种普通情绪可以按权重随机选择多个动画片段，并在 OLED 上按帧播放。
5. 任何资源缺失、损坏或版本不兼容时，保持现有内置表情可用。

## 3. 非目标

- 第一阶段不增加网页文件选择、资源预览或编辑器；上传由主机命令行工具执行。
- 不把 API Key、Wi-Fi 口令、管理密码、聊天历史或运行配置写入 W25Q。
- 不实现通用文件系统、语音资源、固件 OTA 或不受限的任意文件存储。
- 不允许资源包定义 ESTOP、FAULT、模式文字或链路指示图形。
- CRC 只提供完整性校验，不提供发布者身份认证。第一阶段继续依赖可信局域网和管理员认证。

## 4. 系统边界

```text
JSON 清单 + PNG 帧
  -> 主机 resource_pack 打包器
  -> .arp 资源包
  -> 主机 upload_resources 上传器
  -> 受认证 HTTP 分块 API
  -> ESP32 ResourceService
  -> DeviceService / SPI 邮箱
  -> STM32 StorageService
  -> W25Q 非活动 Bank
  -> 校验、提交、激活
  -> FaceResourceProvider
  -> UI Service / SSD1306
```

### 4.1 主机工具

- `resource_pack` 读取 JSON 清单和 PNG，执行尺寸、编码、范围和 CRC 检查，输出确定性 `.arp` 文件。
- `upload_resources` 在本地交互读取管理密码，登录后使用 Cookie 和 CSRF 上传，显示擦除、写入、校验和激活进度。
- 密码不作为命令行参数，不写入配置、资源包或日志。

### 4.2 ESP32

- `ResourceService` 拥有单个更新会话、偏移、超时、SPI 分块和状态轮询。
- `WebService` 只负责路由、认证、CSRF、HTTP 大小限制和错误映射，不直接操作 SPI。
- `DeviceService` 扩展资源命令与状态事件，继续复用序列号、ACK/NACK、重试和心跳。

### 4.3 STM32

- W25Q 驱动只提供 JEDEC/状态读取、快速读、写使能、扇区擦除启动、页编程启动和 Busy 查询。它不知道 Bank、资源包或 UI。
- `StorageService` 拥有 A/B 布局、更新状态机、代数、CRC、提交日志和启动恢复。
- `FaceResourceProvider` 只读已验证的活动 Bank，按表情 ID 和随机权重选择片段，向 UI 提供当前帧。
- `UI Service` 不理解分区或上传；它只在资源帧可用时解码到现有 SSD1306 缓冲区，否则调用内置绘制。

## 5. Flash 布局

1 MiB 容量按 4 KiB 扇区对齐：

| 区域 | 起始 | 结束 | 用途 |
| --- | ---: | ---: | --- |
| Commit journal | `0x000000` | `0x000FFF` | 追加式激活记录 |
| Bank A | `0x001000` | `0x07FFFF` | 127 个扇区 |
| Bank B | `0x080000` | `0x0FFFFF` | 128 个扇区 |

每个 Bank 的第一个扇区保留给 Bank 头，资源包从下一扇区开始。为保证两个 Bank 都能容纳同一包，最大资源包固定为 504 KiB。容量大于 1 MiB 的芯片第一阶段也只使用前 1 MiB。

### 5.1 Bank 头

Bank 头使用小端定长格式，至少包含：

- `ARBK` magic 和 Bank 格式版本。
- Bank ID、单调 `u32` 代数和写入时的资源格式版本。
- 包长度、包 CRC32 和头 CRC32。
- 状态字段和单独的 commit marker。

所有保留字节写零。计算头 CRC 时将头 CRC 字段置零，且不包含 commit marker。写入包和回读 CRC 都成功后，才将 marker 从擦除值 `0xFF` 单向编程为已提交值。代数比较使用可回绕的 `u32` 序号规则。

### 5.2 Commit journal

扇区 0 由定长记录组成，每次激活只向下一个全 `0xFF` 记录位置追加 Bank ID、代数、包 CRC 和记录 CRC。日志只是启动快速路径，Bank 头仍是恢复依据。

日志写满后，只在至少有一个已提交且校验有效的 Bank 时擦除扇区 0，然后重建当前记录。即使在日志压缩中掉电，也能扫描 Bank 头恢复。Bank 已提交后的 journal 追加失败只记录警告，不否定已校验 Bank，下次启动通过 Bank 扫描恢复。

## 6. 资源包格式

`.arp` 使用小端、无时间戳的确定性二进制格式。格式版本 1 包含：

1. `ARPK` 包头：版本、头长度、`128 x 64`、片段数、帧数、各表偏移、总长度和包 CRC32。计算整包 CRC32 时将该 CRC 字段置零。
2. 片段表：表情 ID、权重、帧间隔、帧数和首帧索引。
3. 帧表：编码类型、数据偏移、编码长度、解码长度和帧 CRC32。帧 CRC32 始终针对解码后的 1024 字节计算。
4. 帧数据区。

边界固定为：

- 只接受 `NEUTRAL`、`HAPPY`、`SAD`、`THINKING`、`SURPRISED`、`SLEEPY` 六个表情 ID。
- 最多 32 个动画片段、256 帧；每个片段至少 1 帧。
- 权重范围 `1-255`，帧间隔范围 `50-2000 ms`。
- 每帧解码后必须恰好是 SSD1306 page-major 格式的 1024 字节。
- 所有加法、偏移和长度在解引用前检查整数溢出和包边界。

### 6.1 PNG 转换

主机打包器要求 PNG 尺寸恰好为 `128 x 64`。像素 alpha 低于 128 时为熄灭；其余像素使用无抖动的固定亮度阈值 128 转为 1bpp，确保同一输入在不同主机上产生同一输出。

### 6.2 帧编码

每帧分别在两种编码中选择更短者：

- `RAW1`：1024 字节 SSD1306 page-major 原始帧。
- `RLE1`：`0x00-0x7F` 表示后续 `control + 1` 个字面量；`0x80-0xFF` 表示将后续一字节重复 `(control & 0x7F) + 1` 次。

解码器必须在输入耗尽时正好输出 1024 字节，过长、过短或包结尾后仍有数据都视为损坏。

## 7. 协议扩展

扩展协议生成器，支持带固定长度的 `bytes` 字段；JSON 黄金样例用十六进制字符串，Python 布局生成为 `Ns`，C 端仍使用明确偏移读写。新消息只追加 ID，不修改已有消息布局。

第一阶段新增：

- `RESOURCE_BEGIN(command_id, update_id, package_size, package_crc32, format_version)`
- `RESOURCE_CHUNK(command_id, update_id, offset, data_length, chunk_crc32, data[238])`
- `RESOURCE_FINISH(command_id, update_id)`
- `RESOURCE_ABORT(command_id, update_id)`
- `GET_RESOURCE_STATUS(update_id)`
- `RESOURCE_STATUS(update_id, state, active_bank, generation, next_offset, total_size, error)`

`RESOURCE_CHUNK` 载荷恰好不超过 256 字节。`data_length` 之外的定长 data 填充必须为零，否则拒绝。协议槽 CRC16 保护链路，chunk CRC32 保护 HTTP 到 SPI 的字节和幂等重试，整包 CRC32 保护最终内容。

## 8. HTTP API 和主机上传

第一阶段增加：

- `GET /api/v1/resources/status`
- `POST /api/v1/resources/updates`
- `PUT /api/v1/resources/updates/{update_id}/chunks/{offset}`
- `POST /api/v1/resources/updates/{update_id}/finish`
- `DELETE /api/v1/resources/updates/{update_id}`

所有路由都要求管理会话；除 GET 外还要求 CSRF。更新开始请求提供格式版本、包长度和 CRC32。块路由使用 `application/octet-stream`，每次最多 4096 字节，不使用 Base64，不提高已有 8 KiB 全局请求体上限。

ESP32 将 HTTP 块拆成最多 238 字节的 SPI 块，严格按偏移等待 ACK。同一更新一次只允许一个 HTTP 块在处理。更新 ID 由 ESP32 生成，在当次启动周期内不重复。

重试规则：

- 当偏移等于 `next_offset` 时写入新数据。
- 当偏移指向紧邻的已接收块且长度、CRC 和内容相同时，幂等返回成功。
- 跳跃偏移、重叠不同内容、越界或非零填充立即拒绝。

HTTP 客户端断线不会中断 ESP32 与 STM32 心跳。在设备未重启且更新会话未超时时，命令行可查询 `next_offset` 续传。ESP32 重启、SPI 链路丢失或 60 秒没有资源会话活动时，STM32 中止更新并保留非提交 Bank，后续从头开始。

## 9. STM32 更新状态机

`StorageService` 使用固定容量、无动态分配的状态机；活动 Bank 是独立属性，`IDLE` 表示当前没有更新，不表示一定没有活动资源：

```text
BOOT_SCAN
  -> BOOT_VERIFY
  -> IDLE

IDLE
  -> ERASING
  -> READY
  -> RECEIVING
  -> VERIFYING
  -> COMMITTING
  -> IDLE

any update state -> ABORTED or FAILED -> IDLE
```

- `BEGIN` 选择非活动 Bank，分配新代数，只擦除 Bank 头和容纳当前资源包所需的数据扇区。
- `BOOT_SCAN` 和 `BOOT_VERIFY` 期间拒绝开始新更新；资源校验完成或确定无有效 Bank 后进入 `IDLE`。
- 扇区擦除和页编程都是“发送命令 + 后续 tick 查询 Busy”，不在单次命令 handler 中等待整个操作完成。
- W25Q 状态显示写保护或写使能未置位时返回明确错误，不自动改写未知芯片的保护位。
- `FINISH` 后分块回读完整包，验证包 CRC、头、目录、范围和帧表，再写 commit marker 和 journal。
- 新 Bank 只在完成提交后于 UI 安全帧边界激活。更新过程中 UI 使用内置表情。

### 9.1 启动恢复

启动时快速扫描 journal 和两个 Bank 头，然后在主循环中以有界块后台校验候选包，不在 `app_init()` 同步读取整个 Bank。未完成全包校验前 UI 使用内置表情。

首选候选失败时继续校验另一 Bank。两份都无效时设置 Flash 资源降级标志，但不进入全局 FAULT。

## 10. UI 集成

- 六种普通表情可以各自定义多个动画片段。设置表情或当前片段完成时，`FaceResourceProvider` 按权重选择下一片段。
- 帧解码只在 OLED 当前没有分页 flush 时更新 1024 字节显存，不使用第二个完整帧缓冲。W25Q 快速读和 RLE 解码使用小型固定 scratch buffer。
- 固件在帧上覆盖状态之前，先清除左上链路点和右上状态文字背景，避免资源像素与文字粘连。
- ESTOP 和 FAULT 始终绘制固件内置安全画面，不读外置帧。
- 某表情未出现在包中时，仅该表情回退内置绘制。运行期帧 CRC 或解码失败时立即回退，标记当前 Bank 内存中不健康，并尝试启用另一个已校验 Bank。

## 11. 状态与安全

更新只能在链路健康、没有活动运动且机器人位于 IDLE 或 ESTOP 时开始。

更新活动期间：

- 拒绝进入 MANUAL/AI 和任何 `MOVE_WHEELS`。
- EC11 模式旋转不能越过存储锁进入可运动模式。
- `STOP`、心跳、故障处理和经过现有人工二次确认的 `CLEAR_ESTOP` 仍按原规则执行。解除 ESTOP 不会解除存储锁。
- 资源代码不调用运动服务、不改运动配置、不自动改模式或解除 ESTOP。

SPI 链路丢失依旧由安全监督进入 ESTOP，同时资源更新中止。无论更新在哪个阶段失败，都不自动激活未提交 Bank。

## 12. 错误模型

对外报告稳定错误类别，至少包含：

- Flash 不可用、容量不足或写保护。
- 更新已占用、状态不允许或会话不匹配。
- 包版本、尺寸、长度、偏移、目录或编码非法。
- 块 CRC、包 CRC、Bank 头 CRC、帧 CRC 或回读校验失败。
- 擦除、页编程、Busy 超时、HTTP 或 SPI 中断。

错误响应不包含包原始字节、密码、Cookie、CSRF 或设备 `/config` 内容。主机工具默认只显示阶段、偏移、进度和稳定错误码。

## 13. 测试

### 13.1 主机与协议

- 同一清单和 PNG 多次打包字节完全相同。
- 拒绝错误尺寸、越界表情 ID、零权重、非法帧间隔、过多片段/帧和超容量包。
- RAW1/RLE1 对空白、全亮、交替位和随机帧往返一致，损坏 RLE 不能溢出输出缓冲。
- 扩展后的协议生成器、Python/C 布局和黄金槽保持一致。
- 重复块幂等，乱序、重叠不同内容、越界和非零填充被拒绝。

### 13.2 STM32 主机测试

使用内存 Flash 假实现注入每一个掉电点：

- 擦除中、头写入中、数据写入中、包校验中、marker 写入前后和 journal 写入中。
- 任意未提交状态重启后仍选择旧 Bank；marker 已完整写入时允许从 Bank 头恢复新 Bank。
- 单 Bank 损坏选择另一 Bank，两 Bank 损坏回退内置表情。
- 擦除、校验和帧解码以有界 tick 推进，SPI1 邮箱和心跳处理不停止。
- 存储锁拒绝 MANUAL、AI、MOVE 和 EC11 进入运动模式，但 STOP 仍生效。

### 13.3 ESP32 和 HTTP

- API 要求登录和 CSRF，块请求保持 4096 字节上限和全局 8192 字节上限。
- ESP32 正确将 HTTP 块拆分为 238 字节 SPI 块，不一次保留整包。
- HTTP 断线可按 `next_offset` 续传；ESP32 重启或 SPI 丢失后不续传旧会话。
- 登录、上传、轮询和错误日志不包含凭据或资源原文。

### 13.4 真机

真机写入前先确认 STM32/W25Q 3.3 V 逻辑供电稳定，电机 5 V 可断开，测试不发送运动命令。

1. 上传样例包，验证六种普通表情和加权随机片段。
2. 在擦除、写入和校验三个阶段分别复位，确认旧 Bank 仍可用。
3. 上传已损坏包，确认不提交且不影响旧 Bank。
4. 拔下或模拟失效 W25Q，确认仅设置降级标志并使用内置表情。
5. 更新全程确认 SPI 心跳和网页状态仍可更新，不触发任何电机输出。

## 14. 资源与固件预算

- STM32 不把整包、整个 Bank 或第二个 1024 字节帧常驻 RAM。
- 新增 STM32 固定 RAM 预算不超过 1536 字节，并继续通过 20 KiB RAM、64 KiB Flash 链接和尺寸检查。
- ESP32 一次最多持有单个 4096 字节 HTTP 块和单个 SPI 块，不在内存中复制整包。
- 打包产物、密码和上传临时文件默认不进入 Git；仓库只保留清单、小型源 PNG 和可重现的打包工具。

## 15. 第一阶段验收

必须同时满足：

1. 主机能从 JSON/PNG 生成可重现的 `.arp` 包，并在上传前本地校验。
2. 命令行能通过受认证 API 分块上传，显示进度，且不泄露密码或会话。
3. STM32 能在不停止心跳的情况下擦除、写入、回读校验和原子激活非活动 Bank。
4. OLED 能使用资源包播放六种普通表情，随机选择多片段，并保留状态文字和链路点。
5. 更新中断、新 Bank 损坏、两 Bank 失效和 W25Q 缺失均不阻断固件内置表情。
6. 更新过程不允许进入运动模式，资源代码不自动修改或解除 ESTOP，不发送电机命令；现有人工二次确认恢复仍按原规则保留。

## 16. 后续阶段

第二阶段可在不改动包格式、STM32 Bank 状态机或分块 API 的前提下，为网页增加文件选择、进度、错误和回滚界面。语音、聊天记忆和其他传感器资源仍使用独立规格，不直接复用本表情包写入路径。
