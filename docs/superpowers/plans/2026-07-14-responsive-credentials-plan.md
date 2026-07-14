# 响应式本地凭据实施计划

- 日期：2026-07-14
- 设计依据：`docs/superpowers/specs/2026-07-14-desktop-robot-design.md`
- 范围：ESP32 密码派生、登录并发控制、热点口令持久化和真机部署

## Task 1：异步密码派生

文件：

- 修改 `firmware/esp32/core/security.py`
- 修改 `tests/test_esp_services.py`

步骤：

1. 先添加同步/异步 PBKDF2 结果一致和计算期间事件循环持续调度的失败测试。
2. 保留现有同步函数以兼容已有调用和记录格式。
3. 新增异步派生、创建记录和验证函数，每 16 轮让出事件循环。
4. 新密码默认迭代次数改为 2000；验证始终使用记录自身的迭代次数。

## Task 2：异步登录和并发保护

文件：

- 修改 `firmware/esp32/services/config_service.py`
- 修改 `firmware/esp32/services/web_service.py`
- 修改 `tests/test_web_service.py`

步骤：

1. 增加异步设置和验证管理密码接口。
2. 登录路径等待异步派生，不阻塞 SPI、HTTP 和状态广播任务。
3. 同一时间只允许一个密码计算；并发请求返回 409。
4. 既有 20000 轮记录验证成功后，以当前密码重新保存为 2000 轮记录。

## Task 3：热点口令持久化

文件：

- 修改 `firmware/esp32/services/config_service.py`
- 修改 `firmware/esp32/services/network_service.py`
- 修改 `tests/test_esp_services.py`

步骤：

1. 在 `/config/secrets.json` 增加 `setup_ap_password`，不加入公开配置视图。
2. 缺失时生成一次符合 WPA2 长度要求的随机口令并原子保存。
3. NetworkService 始终从 ConfigService 获取口令，普通重启不再变化。
4. 通过本地串口配置用户指定的热点和管理密码，不在命令输出、源码或 Git 中记录明文。

## Task 4：部署和真机验收

验证：

```bash
python -m unittest discover -s tests
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

真机步骤：

1. 部署 ESP32 bundle，保留 `/config`。
2. 通过不回显明文的串口脚本写入热点口令和 2000 轮管理密码。
3. 重启 ESP32 和 STM32，手机连接热点并完成登录。
4. 登录计算期间确认 SPI 心跳持续，STM32 不进入 ESTOP，`rx_errors=0`。
5. 再次重启，确认热点口令保持不变并可重新登录。

## 提交

计划与实现分开提交。实现提交不得包含设备凭据、临时探针、构建产物或 `/config` 内容。
