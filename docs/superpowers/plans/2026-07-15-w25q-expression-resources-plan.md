# W25Q 表情资源包第一阶段实施计划

- 日期：2026-07-15
- 设计依据：`docs/superpowers/specs/2026-07-15-w25q-expression-resources-design.md`
- 范围：资源格式、主机工具、协议、STM32 A/B 存储、OLED 资源帧、ESP32 分块上传 API 和命令行上传
- 不在本计划：网页上传界面、语音、聊天存储、电机校准或电机动作

## 执行原则

1. 测试先行：每个纯逻辑模块先加失败用例，再实现最小通过路径。
2. 严格分层：驱动层不理解资源格式，UI 不理解 Bank，Web 不直接操作 SPI。
3. 固定内存：STM32 不动态分配，不保留整包、整 Bank 或第二个 OLED 完整帧。
4. 向后兼容：协议只追加消息和枚举值，不改旧消息布局。
5. 安全优先：资源更新期间锁住运动入口，STOP/心跳/ESTOP 保持；所有真机资源测试都断开电机 5 V。
6. 凭据不落盘：上传工具使用 `getpass`，密码不进命令行、日志、测试夹具或 Git。

## Task 1：建立主机资源格式与打包器

文件：

- 新建 `tools/resource_format.py`
- 新建 `tools/resource_pack.py`
- 新建 `tests/test_resource_pack.py`
- 新建并锁定根目录 `requirements-tools.txt`（仅主机工具，不进 MicroPython bundle）

步骤：

1. 先写 RAW1/RLE1 往返测试，覆盖空白、全亮、交替位、随机数据和损坏控制字节。
2. 定义小端 `ARPK` 包头、片段表和帧表，将 CRC 字段置零后计算 IEEE CRC32。
3. 实现严格 parser/validator，检查整数溢出、偏移、长度、表情 ID、权重、帧间隔、数量和 504 KiB 上限。
4. 使用锁定的 Pillow 主机依赖将 `128 x 64` PNG 按 alpha/亮度 128 阈值转为 SSD1306 page-major 1bpp，不抖动。
5. 实现 `build`、`inspect`、`verify` 命令；输出不包时间戳和主机路径。
6. 验证同一 JSON/PNG 多次生成字节完全相同。

验证：

```bash
.venv/bin/python -m unittest tests.test_resource_pack
.venv/bin/python tools/resource_pack.py --help
```

## Task 2：扩展协议生成器与资源消息

文件：

- 修改 `protocol/generate.py`
- 修改 `protocol/messages.json`
- 重新生成 `protocol/generated/*` 和 `protocol/golden/golden_slots.json`
- 修改 `tests/test_protocol_generator.py`
- 修改 `tests/test_protocol_python.py`
- 修改 `tests/unit/test_protocol_c.c`

步骤：

1. 先添加失败测试：定长 bytes 缺长度、非法十六进制样例、载荷超限和非零尾部填充被拒绝。
2. 为 schema 增加 `bytes` + `length` 字段，Python struct 生成 `Ns`，黄金 JSON 保留十六进制字符串。
3. 追加 `ResourceState`、`ResourceError`、`RESOURCE_BEGIN/CHUNK/FINISH/ABORT`、`GET_RESOURCE_STATUS` 和 `RESOURCE_STATUS`。
4. 确认 `RESOURCE_CHUNK` 为 256 字节，其中 data 固定 238 字节。
5. 运行生成器两次并使用 `--check`，确保输出确定。

验证：

```bash
.venv/bin/python protocol/generate.py
.venv/bin/python protocol/generate.py --check
.venv/bin/python -m unittest tests.test_protocol_generator tests.test_protocol_python
cmake --build build/host --target test_protocol_c
ctest --test-dir build/host --output-on-failure -R protocol_c
```

## Task 3：实现 STM32 纯 C 资源 parser、CRC32 和 RLE

文件：

- 新建 `firmware/stm32/App/Storage/resource_crc32.h`
- 新建 `firmware/stm32/App/Storage/resource_crc32.c`
- 新建 `firmware/stm32/App/Storage/resource_format.h`
- 新建 `firmware/stm32/App/Storage/resource_format.c`
- 新建 `tests/unit/test_resource_format.c`
- 修改 `tests/CMakeLists.txt`

步骤：

1. 用标准向量固定 IEEE CRC32 实现，并与 Python `binascii.crc32` 交叉校验。
2. 实现无未对齐指针转换的小端 parser，所有范围检查在取数前完成。
3. 实现 RAW1 和有界 RLE1 解码器，要求输入恰好耗尽且输出恰好 1024 字节。
4. 使用 Python 打包器生成的小型黄金 fixture 验证 C/Python 包头、目录、帧 CRC 和解码完全一致。
5. 模糊变异偏移、长度、数量和 RLE 控制字节，确认不越界。

验证：

```bash
cmake --build build/host --target test_resource_format
ctest --test-dir build/host --output-on-failure -R resource_format
```

## Task 4：扩展 W25Q 驱动的非阻塞写入原语

文件：

- 修改 `firmware/stm32/Drivers/Storage/w25q.h`
- 修改 `firmware/stm32/Drivers/Storage/w25q.c`
- 新建 `firmware/stm32/App/Storage/storage_flash.h`
- 新建 `firmware/stm32/App/Storage/w25q_storage_adapter.h`
- 新建 `firmware/stm32/App/Storage/w25q_storage_adapter.c`

步骤：

1. 将当前逐字节 `READ` 改为有界 burst read，保持地址和容量检查。
2. 新增状态寄存器读取、写使能、4 KiB 扇区擦除启动、不跨 256 字节页编程启动和 Busy 查询。
3. 驱动只发起操作，不循环等待 WIP 清除；超时由 App 状态机计时。
4. 检查 WEL 和写保护状态，不自动清除未知芯片保护位。
5. 通过 `storage_flash_ops_t` 适配器隔离 HAL，使 `StorageService` 可在主机内存 Flash 上测试。

验证：

```bash
AIROBOT_ARM_GCC_ROOT=/home/orange/.cache/airobot-toolchain/root cmake --build build/stm32-live
```

## Task 5：实现 A/B StorageService 和掉电恢复

文件：

- 新建 `firmware/stm32/App/Storage/storage_service.h`
- 新建 `firmware/stm32/App/Storage/storage_service.c`
- 新建 `tests/support/fake_storage_flash.h`
- 新建 `tests/unit/test_storage_service.c`
- 修改 `tests/CMakeLists.txt`

步骤：

1. 先在内存 Flash 假实现中固定 journal、Bank A/B、Bank 头、commit marker 和回绕代数选择测试。
2. 实现 `BOOT_SCAN -> BOOT_VERIFY -> IDLE` 和 `ERASING -> READY -> RECEIVING -> VERIFYING -> COMMITTING -> IDLE`。
3. 每个 tick 最多发起或检查一次 Flash 操作；写入块按页边界再分割。
4. 实现严格顺序偏移、最后一块幂等重试、60 秒超时、显式 abort 和 SPI 链路丢失 abort。
5. `FINISH` 后回读完整包并验证包 parser/CRC，先写 Bank commit marker，后写 journal。
6. 在每一个 Flash 操作和 marker/journal 字节边界注入重启，验证未提交新 Bank 从不覆盖旧 Bank。
7. journal 写入失败仅报告警告，Bank 头仍能在下次启动恢复。

验证：

```bash
cmake --build build/host --target test_storage_service
ctest --test-dir build/host --output-on-failure -R storage_service
```

## Task 6：实现资源表情 Provider 并接入 UI

文件：

- 新建 `firmware/stm32/App/Storage/face_resource_provider.h`
- 新建 `firmware/stm32/App/Storage/face_resource_provider.c`
- 新建 `tests/unit/test_face_resource_provider.c`
- 修改 `firmware/stm32/App/Ui/ui_service.h`
- 修改 `firmware/stm32/App/Ui/ui_service.c`
- 修改 `tests/CMakeLists.txt`

步骤：

1. 用可注入读取函数加载已验证 Bank 的片段/帧表，不让 UI 直接访问 W25Q。
2. 按现有 xorshift 随机源和权重选片段，按帧间隔推进；缺失表情返回“使用内置”。
3. 只在 OLED 未 flush 时解码到现有 1024 字节缓冲；失败时立即重绘内置帧，不 flush 部分解码帧。
4. 在资源帧上先清理左上/右上覆盖区，再绘制链路点和状态文字。
5. ESTOP/FAULT 强制使用固件内置安全画面，不请求资源帧。
6. 运行期帧损坏标记 Bank 不健康，只在另一 Bank 已完成本次启动校验时才切换。

验证：

```bash
cmake --build build/host --target test_face_resource_provider
ctest --test-dir build/host --output-on-failure -R 'face_resource|face_animator|status_label'
```

## Task 7：集成 STM32 协议路由、存储锁和主循环

文件：

- 修改 `firmware/stm32/App/app.c`
- 修改 `firmware/stm32/App/app.h`
- 修改 `firmware/stm32/CMakeLists.txt`
- 修改 `tests/unit/test_robot_core.c` 或新建独立的存储锁策略测试

步骤：

1. 在 app 初始化中注册 W25Q adapter、`StorageService` 和 `FaceResourceProvider`，W25Q 不可用时保持内置表情。
2. 添加资源 begin/chunk/finish/abort/status handler，继续使用现有 replay cache 和 ACK/NACK。
3. 存储更新只允许 IDLE/ESTOP 且 `active_command_id=0`。更新活动期间拒绝 MANUAL/AI/MOVE 和 EC11 进入运动模式。
4. STOP、心跳、FAULT 和现有 operator-confirmed clear 保持；clear 不解锁存储锁。
5. `app_process()` 每次调用一次 storage/provider tick，不在 handler 中做长擦除或全包 CRC。
6. `FLASH_INFO` 继续兼容，另用 `RESOURCE_STATUS` 报告活动 Bank、代数、进度和错误。

验证：

```bash
cmake --build build/host
ctest --test-dir build/host --output-on-failure
AIROBOT_ARM_GCC_ROOT=/home/orange/.cache/airobot-toolchain/root cmake --build build/stm32-live
.venv/bin/python tools/check_firmware_size.py build/stm32-live/firmware/stm32/desktop_robot.elf
```

## Task 8：同步 STM32 模拟器和 ESP32 DeviceService

文件：

- 修改 `tools/stm_simulator.py`
- 修改 `tests/test_stm_simulator.py`
- 修改 `firmware/esp32/services/device_service.py`
- 修改 `tests/test_esp_services.py`

步骤：

1. 模拟器使用有界 `bytearray` 模拟非活动 Bank，处理顺序块、幂等重试、finish CRC 和资源状态。
2. 在模拟更新活动期间拒绝运动模式/MOVE，STOP 仍使机器人进入 ESTOP。
3. DeviceService 新增 begin/chunk/finish/abort/status 方法，验证 chunk 长度、填充和状态事件。
4. 模拟 SPI 中断时更新丢弃，链路恢复不自动续传旧 update ID。

验证：

```bash
.venv/bin/python -m unittest tests.test_stm_simulator tests.test_esp_services
```

## Task 9：实现 ESP32 ResourceService 和受认证 HTTP API

文件：

- 新建 `firmware/esp32/services/resource_service.py`
- 修改 `firmware/esp32/main.py`
- 修改 `firmware/esp32/services/web_service.py`
- 新建 `tests/test_resource_service.py`
- 修改 `tests/test_web_service.py`

步骤：

1. `ResourceService` 限制单会话，生成不重复 update ID，将最多 4096 字节 HTTP 块拆成 238 字节 SPI 块。
2. 每个 SPI 块单独等 ACK，保存 `next_offset`；取消、错误和完成都清理当地会话。
3. 动态路由解析只接受十进制 update ID/偏移和固定路径深度，拒绝空块、超 4096 字节、错误 content type 和非法偏移。
4. 所有资源路由需要会话，变更路由需要 CSRF；错误转换为有界 JSON，不包含 body 或凭据。
5. 为 HTTP 掉线但 SPI 仍健康的场景保留会话，根据 STM32 `next_offset` 续传；ESP 重启不恢复会话。
6. 集成 `main.py`，不改密钥、网络和现有聊天服务边界。

验证：

```bash
.venv/bin/python -m unittest tests.test_resource_service tests.test_web_service
.venv/bin/python tools/build_esp_bundle.py --output build/esp32-bundle
```

## Task 10：实现命令行上传器

文件：

- 新建 `tools/upload_resources.py`
- 新建 `tests/test_upload_resources.py`

步骤：

1. 上传前复用 `resource_format` 验证 `.arp`，不将损坏包发送到设备。
2. 使用 Python 标准库 HTTP 客户端，交互 `getpass` 登录，从登录响应取得 Cookie/CSRF；不提供密码命令行参数。
3. 以 4096 字节分块上传，以等待擦除、上传百分比、校验和激活显示进度。
4. HTTP 错误后重查状态；同 update ID 仍活跃时从 `next_offset` 续传，设备会话丢失时明确提示重新开始。
5. 使用本地假 HTTP 服务测试 Cookie/CSRF、分块边界、续传、失败和输出脱敏。

验证：

```bash
.venv/bin/python -m unittest tests.test_upload_resources
.venv/bin/python tools/upload_resources.py --help
```

## Task 11：添加样例表情资源

文件：

- 新建 `assets/expressions/manifest.json`
- 新建 `assets/expressions/<expression>/<clip>-<frame>.png`
- 新建 `tests/fixtures/resources/` 中的最小测试输入
- 修改 `.gitignore` 忽略 `*.arp` 和上传临时产物

步骤：

1. 为六种普通表情提供至少一个片段，并让至少两种表情含多个权重片段以验证随机选择。
2. 保留右上模式文字和左上链路点的视觉空间；固件仍会清理覆盖区。
3. 将清单打包两次并比较哈希，再用 `inspect/verify` 检查。
4. 不提交生成的 `.arp`；仓库只保留可编辑源资产和打包路径。

## Task 12：全量回归、资源预算和提交

验证命令：

```bash
.venv/bin/python protocol/generate.py --check
.venv/bin/python -m unittest discover -s tests
cmake --build build/host
ctest --test-dir build/host --output-on-failure
AIROBOT_ARM_GCC_ROOT=/home/orange/.cache/airobot-toolchain/root cmake --build build/stm32-live
.venv/bin/python tools/check_firmware_size.py build/stm32-live/firmware/stm32/desktop_robot.elf
.venv/bin/python tools/check_firmware_isa.py build/stm32-live/firmware/stm32/desktop_robot.elf
.venv/bin/python tools/build_esp_bundle.py --output build/esp32-bundle
git diff --check
```

步骤：

1. 记录 STM32 text/data/bss 增量，确认新增固定 RAM 不超 1536 字节且总体仍在 20 KiB/64 KiB 内。
2. 检查 ESP32 bundle 文件数和字节数，确认 N16R8 文件系统余量充足。
3. 扫描差异中的密码、API Key、Cookie、CSRF、`/config` 和生成 `.arp`，不让凭据或产物进入提交。
4. 按逻辑层创建小型 Conventional Commits，每次推送 `main`，最终保持工作树干净。

建议提交：

- `feat(resources): define expression pack format`
- `feat(protocol): add resource update messages`
- `feat(stm32): add power-safe resource storage`
- `feat(stm32): render W25Q expression resources`
- `feat(esp32): add chunked resource uploads`
- `feat(resources): add sample OLED expressions`

## Task 13：真机无运动验收

前置条件：

- STM32/W25Q 3.3 V 逻辑供电稳定。
- 电机 5 V 物理断开，轮子不参与本验收。
- ESP32 `/config` 继续保留，资源上传工具不回显密码。

步骤：

1. 刷写 STM32 固件，部署 ESP32 文件时保留 `/config`；正式 ESP 服务启动后再复位 STM32。
2. 读取资源状态，确认 W25Q 容量、活动 Bank 和降级状态。
3. 使用命令行上传样例包，验证擦除、写入、校验、提交和 OLED 六种表情。
4. 上传第二代包后，在下一次测试中分别对擦除、写入和校验阶段做受控复位，验证旧 Bank 恢复。
5. 上传损坏包并模拟 W25Q 不可用，确认旧 Bank 或内置表情回退。
6. 记录心跳、rx_errors、资源状态、OLED 观察和是否意外复位；不发送任何运动命令。

真机步骤在代码/主机测试完成后另行取得用户对稳定供电和电机 5 V 断开的确认，不因本计划批准而自动开始。
