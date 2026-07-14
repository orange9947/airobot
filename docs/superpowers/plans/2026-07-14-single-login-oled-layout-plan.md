# 单次登录与 OLED 状态布局实施计划

- 日期：2026-07-14
- 设计依据：`docs/superpowers/specs/2026-07-14-single-login-oled-layout-design.md`
- 范围：ESP32 Web 登录响应、网页认证状态机、STM32 OLED 状态标签、自动化和真机部署

## Task 1：让登录响应携带首屏数据

文件：

- 修改 `firmware/esp32/services/web_service.py`
- 修改 `tests/test_web_service.py`

步骤：

1. 先扩展登录测试，要求成功响应包含 `csrf`、脱敏 `config` 和当前 `status`。
2. 验证 JSON 不包含 Wi-Fi 密码、API Key、密码记录或会话令牌。
3. 在创建 HttpOnly Cookie 后，从现有 `public_view()` 和 `_status()` 组装响应，不新增认证存储。
4. 保持错误密码、并发认证和首次设置密码行为不变。

## Task 2：前端直接使用登录响应

文件：

- 修改 `web/app.js`
- 修改 `web/index.html`
- 修改 `tests/test_esp_bundle.py`
- 更新 `/tmp` Playwright 登录验收脚本

步骤：

1. 静态 HTML 默认禁用密码框和提交按钮，脚本绑定并完成 bootstrap 后再启用。
2. 让 `loadAuthenticatedConsole()` 接受可选的登录首屏数据。
3. 正常登录时直接填充配置、渲染状态、连接事件流并关闭登录层。
4. 仅在已有会话的“重新连接”路径请求 `/config` 和 `/status`。
5. 浏览器用延迟脚本加载复现首次点击，断言未就绪表单不可提交且没有页面导航。
6. 浏览器断言一次密码提交后登录层关闭、登录接口只调用一次，且不立即请求配置/状态。
7. 保留错误密码、超时、401 会话失效和加载重试测试。

## Task 3：抽取并测试 OLED 右上角标签布局

文件：

- 新增 `firmware/stm32/App/Ui/status_label.h`
- 新增 `firmware/stm32/App/Ui/status_label.c`
- 新增 `tests/unit/test_status_label.c`
- 修改 `tests/CMakeLists.txt`
- 修改 `firmware/stm32/CMakeLists.txt`

步骤：

1. 先写主机 C 测试，固定 `AI`、`MAN`、`IDLE`、`STOP`、`ERR` 的文字和右对齐坐标。
2. 实现纯布局助手：3x5 字体宽度为 `4 * 字符数 - 1`，`y=2`，右边距 2 像素。
3. BOOT、SELF_TEST 和普通待机状态沿用 `IDLE` 标签，避免改变现有显示语义。
4. 将助手加入主机测试和 STM32 交叉构建。

## Task 4：接入 OLED 渲染

文件：

- 修改 `firmware/stm32/App/Ui/ui_service.c`

步骤：

1. 删除底部 `IDLE/MAN/AI/STOP/ERR` 绘制。
2. 先绘制安全或普通表情，再绘制左上角链路点和右上角状态标签。
3. 保持安全状态优先级、随机动画时序和分页刷新逻辑不变。
4. 交叉编译并检查 Flash/RAM 大小。

## Task 5：完整验证、提交与真机部署

验证命令：

```bash
.venv/bin/python -m unittest discover -s tests
cmake --build build/host
ctest --test-dir build/host --output-on-failure
node --check web/app.js
AIROBOT_ARM_GCC_ROOT=/home/orange/.cache/airobot-toolchain/root cmake --build build/stm32-live
```

验收顺序：

1. 运行手机和桌面 Playwright 登录流程，确认单次提交、无导航和错误状态。
2. 提交并推送实现，不包含 `/config`、凭据或临时脚本。
3. 烧录 STM32，部署 ESP32 bundle，并保留现有配置。
4. 真机不发送运动指令；确认 OLED 标签位于右上角且表情区域完整。
5. 由手机使用一次密码登录控制台；诊断完成后恢复 ESP32 正式服务并最后复位 STM32。
