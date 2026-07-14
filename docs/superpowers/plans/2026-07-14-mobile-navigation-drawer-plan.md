# 手机侧边导航实施计划

- 日期：2026-07-14
- 设计依据：`docs/superpowers/specs/2026-07-14-desktop-robot-design.md`
- 范围：`web/` 静态界面、前端测试、ESP32 bundle 与真机部署

## Task 1：定义可达的移动导航结构

文件：

- 修改 `web/index.html`
- 新增 `web/assets/icons/menu.svg`
- 新增 `web/assets/icons/x.svg`
- 修改 `tests/test_esp_bundle.py`

步骤：

1. 先添加失败测试，要求顶栏菜单按钮、抽屉标识、关闭按钮和遮罩存在。
2. 复用现有四个 `data-view` 按钮和退出按钮，不复制页面或设置表单。
3. 菜单按钮声明 `aria-controls` 和 `aria-expanded`，抽屉提供明确标签。

## Task 2：实现抽屉交互和首次设置跳转

文件：

- 修改 `web/app.js`
- 修改 `tests/test_web_service.py` 或前端静态契约测试

步骤：

1. 提取统一的视图切换函数。
2. 实现菜单、关闭按钮、遮罩和 ESC 关闭；打开时聚焦首个导航项，关闭后焦点返回菜单按钮。
3. 选择导航项后关闭抽屉，桌面端行为保持不变。
4. 登录后若处于配置热点且家庭 Wi-Fi SSID 为空，自动切换到设置页。

## Task 3：实现手机安全区布局

文件：

- 修改 `web/styles.css`

步骤：

1. 桌面端继续显示常驻左侧栏并隐藏抽屉控制。
2. 小于等于 980 px 时使用固定左侧抽屉和遮罩，不再把导航放在第三行底部。
3. 页面高度使用 `100vh` 回退和 `100dvh`，抽屉使用 `env(safe-area-inset-*)`。
4. 手机抽屉显示图标与文字，退出操作保持可见。

## Task 4：截图、部署和真机验收

验证：

```bash
python -m unittest discover -s tests
node --check web/app.js
```

使用 Playwright 检查 390x844 和 412x915：

1. 登录后顶栏菜单按钮可见。
2. 抽屉完全位于视口内，设置项和退出项可见且不重叠。
3. 点击设置后 Wi-Fi、API Key 和保存按钮均可见。
4. 桌面 1440x900 的常驻侧栏不回归。
5. 部署 ESP32 bundle，手机连接真实热点完成同样操作。

## 提交

计划与实现分开提交。截图输出和临时服务器文件不进入 Git。
