(function () {
  "use strict";

  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => Array.from(document.querySelectorAll(selector));
  const demoMode = new URLSearchParams(location.search).get("demo") === "1";
  const mobileNavigation = window.matchMedia("(max-width: 980px)");
  const state = {
    csrf: "",
    config: null,
    status: null,
    events: [],
    websocket: null,
    authenticated: demoMode,
    busy: false,
    clearContextBusy: false,
  };

  const stateNames = {
    boot: "BOOT",
    self_test: "SELF TEST",
    idle: "IDLE",
    manual: "MANUAL",
    ai: "AI",
    estop: "ESTOP",
    fault: "FAULT",
  };

  const demoConfig = {
    schema: 1,
    active_provider: "deepseek",
    providers: {
      deepseek: { base_url: "https://api.deepseek.com", model: "deepseek-chat", timeout_s: 60, max_output_tokens: 256 },
      openai: { base_url: "https://api.openai.com", model: "gpt-5-mini", timeout_s: 60, max_output_tokens: 256 },
    },
    wifi: { ssid: "Robot-Lab", password_configured: true },
    motion: { soft_rate_sps: 400, accel_sps2: 600, hold_ms: 200 },
    keys_configured: { deepseek: true, openai: false },
  };

  const demoStatus = {
    connected: true,
    state: 3,
    state_name: "manual",
    selected_mode: 3,
    degraded_flags: 0,
    fault_code: 0,
    active_command_id: 0,
    rx_errors: 0,
    flash: { jedec_id: 0xEF4014, capacity_bytes: 1048576, status: 0 },
    network: { mode: "station", ip: "192.168.1.86", setup_ssid: "" },
    provider: "deepseek",
    model: "deepseek-chat",
  };

  function escapeHtml(value) {
    return String(value)
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;")
      .replaceAll("'", "&#039;");
  }

  function nowLabel() {
    return new Intl.DateTimeFormat("zh-CN", {
      hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false,
    }).format(new Date());
  }

  function toast(message, error) {
    const element = document.createElement("div");
    element.className = "toast" + (error ? " error" : "");
    element.textContent = message;
    $("#toast-region").append(element);
    setTimeout(() => element.remove(), 3200);
  }

  function addEvent(kind, detail) {
    state.events.unshift({ time: nowLabel(), kind, detail: String(detail) });
    state.events = state.events.slice(0, 40);
    renderEvents();
  }

  function renderEvents() {
    const recent = state.events.slice(0, 6);
    $("#event-table").innerHTML = recent.length
      ? recent.map((item) => `<div class="event-row"><time>${escapeHtml(item.time)}</time><strong>${escapeHtml(item.kind)}</strong><span>${escapeHtml(item.detail)}</span></div>`).join("")
      : '<div class="empty-state">暂无事件</div>';
    $("#diagnostic-log").innerHTML = state.events.length
      ? state.events.map((item) => `<div class="diagnostic-entry"><time>${escapeHtml(item.time)}</time><strong>${escapeHtml(item.kind)}</strong><span>${escapeHtml(item.detail)}</span></div>`).join("")
      : '<div class="empty-state">暂无事件</div>';
    $("#event-count").textContent = `${state.events.length} EVENTS`;
  }

  async function api(path, options) {
    if (demoMode) return demoApi(path, options || {});
    const request = Object.assign({ method: "GET", headers: {} }, options || {});
    request.credentials = "same-origin";
    if (request.body && typeof request.body !== "string") {
      request.headers["Content-Type"] = "application/json";
      request.body = JSON.stringify(request.body);
    }
    if (!/^(GET|HEAD)$/i.test(request.method) && state.csrf) {
      request.headers["X-CSRF-Token"] = state.csrf;
    }
    const response = await fetch(path, request);
    let payload = {};
    try { payload = await response.json(); } catch (_error) { /* Empty response. */ }
    if (!response.ok) {
      if (response.status === 401) showAuth(false);
      throw new Error(payload.error || `请求失败 (${response.status})`);
    }
    return payload;
  }

  async function demoApi(path, options) {
    await new Promise((resolve) => setTimeout(resolve, 120));
    const method = options.method || "GET";
    if (path === "/api/v1/bootstrap") return { setup_required: false, network: demoStatus.network };
    if (path === "/api/v1/status") return Object.assign({}, demoStatus);
    if (path === "/api/v1/config" && method === "GET") return JSON.parse(JSON.stringify(demoConfig));
    if (path === "/api/v1/config" && method === "PUT") {
      Object.assign(demoConfig, options.body);
      demoStatus.provider = demoConfig.active_provider;
      demoStatus.model = demoConfig.providers[demoConfig.active_provider].model;
      return { ok: true };
    }
    if (path === "/api/v1/secrets") {
      demoConfig.keys_configured[options.body.provider] = Boolean(options.body.api_key);
      return { ok: true };
    }
    if (path === "/api/v1/mode") {
      demoStatus.state_name = options.body.mode;
      demoStatus.selected_mode = { idle: 2, manual: 3, ai: 4 }[options.body.mode];
      demoStatus.state = { idle: 2, manual: 3, ai: 4 }[options.body.mode];
      return { ok: true, command_id: Date.now() & 0xFFFF };
    }
    if (path === "/api/v1/motion") return { ok: true, left_steps: 80, right_steps: 80 };
    if (path === "/api/v1/stop") return { ok: true, command_id: Date.now() & 0xFFFF };
    if (path === "/api/v1/chat") {
      const text = /停|stop/i.test(options.body.message)
        ? "已向 STM32 下发停止指令。"
        : "指令已理解。机器人控制动作会先经过参数校验，再由 STM32 执行。";
      return { text, provider: demoConfig.active_provider };
    }
    return { ok: true, csrf: "demo" };
  }

  function showAuth(setupRequired) {
    if (demoMode) return;
    $("#auth-title").textContent = setupRequired ? "设置管理密码" : "登录机器人";
    $("#auth-eyebrow").textContent = setupRequired ? "FIRST BOOT" : "LOCAL DEVICE";
    $("#auth-submit-label").textContent = setupRequired ? "完成初始化" : "进入控制台";
    $("#auth-password").autocomplete = setupRequired ? "new-password" : "current-password";
    $("#auth-layer").classList.remove("hidden");
    $("#auth-password").focus();
  }

  function hideAuth() {
    $("#auth-layer").classList.add("hidden");
    $("#auth-error").textContent = "";
    $("#auth-password").value = "";
  }

  function syncNavigationMode() {
    const sidebar = $("#primary-sidebar");
    if (mobileNavigation.matches) {
      const isOpen = sidebar.classList.contains("open");
      sidebar.setAttribute("aria-hidden", isOpen ? "false" : "true");
      sidebar.inert = !isOpen;
    } else {
      sidebar.setAttribute("aria-hidden", "false");
      sidebar.inert = false;
      sidebar.classList.remove("open");
      $("#nav-backdrop").classList.remove("open");
      $("#menu-button").setAttribute("aria-expanded", "false");
      document.body.classList.remove("drawer-open");
    }
  }

  function openNavigation() {
    if (!mobileNavigation.matches) return;
    $("#primary-sidebar").classList.add("open");
    $("#primary-sidebar").setAttribute("aria-hidden", "false");
    $("#primary-sidebar").inert = false;
    $("#nav-backdrop").classList.add("open");
    $("#menu-button").setAttribute("aria-expanded", "true");
    document.body.classList.add("drawer-open");
    $("#primary-sidebar [data-view]").focus();
  }

  function closeNavigation(returnFocus) {
    const sidebar = $("#primary-sidebar");
    const wasOpen = sidebar.classList.contains("open");
    sidebar.classList.remove("open");
    $("#nav-backdrop").classList.remove("open");
    $("#menu-button").setAttribute("aria-expanded", "false");
    document.body.classList.remove("drawer-open");
    if (wasOpen && returnFocus !== false) $("#menu-button").focus();
    if (mobileNavigation.matches) {
      sidebar.setAttribute("aria-hidden", "true");
      sidebar.inert = true;
    }
  }

  function switchView(viewName) {
    $$("[data-view]").forEach((item) => item.classList.toggle("active", item.dataset.view === viewName));
    $$(".view").forEach((view) => view.classList.toggle("active", view.id === `view-${viewName}`));
    closeNavigation(true);
  }

  function openClearChatDialog() {
    if (state.clearContextBusy || state.busy) return;
    $("#clear-chat-error").textContent = "";
    $("#clear-chat-layer").classList.remove("hidden");
    $("#clear-chat-layer").setAttribute("aria-hidden", "false");
    $("#clear-chat-cancel").focus();
  }

  function closeClearChatDialog() {
    if (state.clearContextBusy) return;
    $("#clear-chat-layer").classList.add("hidden");
    $("#clear-chat-layer").setAttribute("aria-hidden", "true");
    $("#clear-chat-error").textContent = "";
    $("#clear-chat-context").focus();
  }

  async function clearChatContext() {
    if (state.clearContextBusy) return;
    const cancelButton = $("#clear-chat-cancel");
    const confirmButton = $("#clear-chat-confirm");
    state.clearContextBusy = true;
    cancelButton.disabled = true;
    confirmButton.disabled = true;
    $("#clear-chat-error").textContent = "";
    let cleared = false;
    try {
      await api("/api/v1/chat", { method: "DELETE", body: {} });
      $("#chat-stream").innerHTML = "";
      appendMessage("assistant", "新的对话已开始。");
      cleared = true;
    } catch (error) {
      $("#clear-chat-error").textContent = error.message;
      toast(error.message, true);
    } finally {
      state.clearContextBusy = false;
      cancelButton.disabled = false;
      confirmButton.disabled = false;
    }
    if (cleared) {
      closeClearChatDialog();
      toast("上下文已清除");
    }
  }

  function selectedModeName(status) {
    return { 2: "idle", 3: "manual", 4: "ai" }[status.selected_mode] || status.state_name || "idle";
  }

  function expressionForStatus(status) {
    if (status.state_name === "estop" || status.state_name === "fault") return status.state_name;
    if (status.state_name === "ai") return "thinking";
    if (status.state_name === "idle") return "sleepy";
    return "neutral";
  }

  function formatBytes(bytes) {
    if (!bytes) return "未检测";
    if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(bytes % 1048576 ? 1 : 0)} MiB`;
    return `${Math.round(bytes / 1024)} KiB`;
  }

  function renderStatus(status) {
    state.status = status;
    const connected = Boolean(status.connected);
    const stateName = status.state_name || "unknown";
    const stateLabel = stateNames[stateName] || stateName.toUpperCase();
    const network = status.network || {};
    const modeName = selectedModeName(status);
    const manualEnabled = connected && modeName === "manual" && !["estop", "fault"].includes(stateName);

    $("#state-code").textContent = stateLabel;
    $("#oled-label").textContent = stateLabel;
    $("#oled-face").dataset.expression = expressionForStatus(status);
    $("#spi-status").textContent = connected ? "在线" : "断开";
    $("#wifi-status").textContent = network.mode === "station" ? `已连接 · ${network.ip}` : network.mode === "access_point" ? `配置热点 · ${network.ip}` : "离线";
    $("#model-status").textContent = status.model || "未配置";
    $("#active-task").textContent = status.active_command_id ? `#${status.active_command_id}` : "无";
    $("#fault-code").textContent = String(status.fault_code || 0);
    $("#metric-state").textContent = stateLabel;
    $("#metric-rx").textContent = String(status.rx_errors || 0);
    $("#metric-flash").textContent = status.flash && status.flash.status === 0 ? formatBytes(status.flash.capacity_bytes) : "未检测";
    $("#metric-ip").textContent = network.ip || "0.0.0.0";
    $("#device-address").textContent = network.ip ? `http://${network.ip}` : "本地设备";
    $("#link-state").classList.toggle("online", connected);
    $("#link-state").classList.toggle("fault", !connected);
    $("#link-state span").textContent = connected ? "设备在线" : "设备离线";
    $("#motion-lock").textContent = manualEnabled ? "可执行有限步运动" : `${stateNames[modeName] || modeName.toUpperCase()} 模式已锁定`;
    $$("[data-direction]").forEach((button) => { button.disabled = !manualEnabled; });
    $$("[data-mode]").forEach((button) => button.classList.toggle("active", button.dataset.mode === modeName));
    $("#chat-provider").textContent = status.provider === "openai" ? "OpenAI" : "DeepSeek";
    $("#chat-model").textContent = status.model || "未配置";
  }

  async function refreshStatus(quiet) {
    if (!state.authenticated) return;
    try {
      const status = await api("/api/v1/status");
      renderStatus(status);
      if (!quiet) addEvent("STATUS", "设备状态已刷新");
      return status;
    } catch (error) {
      $("#link-state").classList.add("offline");
      $("#link-state span").textContent = "连接失败";
      if (!quiet) toast(error.message, true);
    }
  }

  function fillProviderFields() {
    if (!state.config) return;
    const name = $("#provider-select").value;
    const provider = state.config.providers[name];
    $("#provider-model").value = provider.model || "";
    $("#provider-url").value = provider.base_url || "";
    const configured = state.config.keys_configured && state.config.keys_configured[name];
    $("#key-state").textContent = `${name === "openai" ? "OpenAI" : "DeepSeek"} 密钥${configured ? "已配置" : "未配置"}`;
    $("#provider-key").value = "";
  }

  function fillSettings(config) {
    state.config = config;
    $("#provider-select").value = config.active_provider;
    $("#wifi-ssid").value = config.wifi.ssid || "";
    $("#wifi-password").value = "";
    $("#motion-rate").value = config.motion.soft_rate_sps;
    $("#motion-accel").value = config.motion.accel_sps2;
    $("#motion-hold").value = config.motion.hold_ms;
    fillProviderFields();
    updateStepEstimate();
  }

  async function loadConfig() {
    const config = await api("/api/v1/config");
    fillSettings(config);
    return config;
  }

  function connectEvents() {
    if (demoMode || !state.authenticated || state.websocket) return;
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(`${scheme}//${location.host}/api/v1/events`);
    state.websocket = socket;
    socket.onmessage = (event) => {
      try {
        const payload = JSON.parse(event.data);
        if (payload.type === "tool") addEvent("AI TOOL", payload.result.name);
        else if (payload.type === "chat") addEvent("MODEL", "对话完成");
        else if (payload.type === "device") addEvent("STM32", `消息 ${payload.message_type}`);
        if (payload.status) renderStatus(Object.assign({}, state.status || {}, payload.status));
      } catch (_error) { /* Ignore malformed event frames. */ }
    };
    socket.onclose = () => {
      state.websocket = null;
      if (state.authenticated) setTimeout(connectEvents, 1800);
    };
  }

  function updateStepEstimate() {
    const speed = Number($("#speed-input").value);
    const duration = Number($("#duration-input").value);
    const rate = state.config ? Number(state.config.motion.soft_rate_sps) : 400;
    $("#speed-output").textContent = `${speed}%`;
    $("#duration-output").textContent = `${duration} ms`;
    $("#step-estimate").textContent = `${Math.max(1, Math.floor(rate * speed / 100 * duration / 1000))} 步`;
  }

  async function sendMode(mode) {
    try {
      await api("/api/v1/mode", { method: "POST", body: { mode } });
      addEvent("MODE", `切换到 ${stateNames[mode] || mode.toUpperCase()}`);
      await refreshStatus(true);
    } catch (error) { toast(error.message, true); }
  }

  async function sendMotion(direction) {
    if (state.busy) return;
    state.busy = true;
    try {
      const body = {
        direction,
        speed_percent: Number($("#speed-input").value),
        duration_ms: Number($("#duration-input").value),
      };
      await api("/api/v1/motion", { method: "POST", body });
      addEvent("MOTION", `${direction} · ${body.speed_percent}% · ${body.duration_ms}ms`);
    } catch (error) { toast(error.message, true); }
    finally { state.busy = false; }
  }

  async function stopRobot() {
    try {
      await api("/api/v1/stop", { method: "POST", body: {} });
      addEvent("STOP", "停止指令已确认");
      toast("机器人已停止");
    } catch (error) { toast(error.message, true); }
  }

  function appendMessage(role, text) {
    const item = document.createElement("div");
    item.className = `message ${role === "assistant" ? "robot" : role}`;
    item.innerHTML = `<span>${role === "user" ? "YOU" : "AI"}</span><p>${escapeHtml(text)}</p>`;
    $("#chat-stream").append(item);
    $("#chat-stream").scrollTop = $("#chat-stream").scrollHeight;
  }

  function bindUi() {
    $$("[data-view]").forEach((button) => button.addEventListener("click", () => switchView(button.dataset.view)));
    $("#menu-button").addEventListener("click", openNavigation);
    $("#menu-close").addEventListener("click", () => closeNavigation(true));
    $("#nav-backdrop").addEventListener("click", () => closeNavigation(true));
    document.addEventListener("keydown", (event) => {
      const clearDialogOpen = !$("#clear-chat-layer").classList.contains("hidden");
      if (event.key === "Escape" && clearDialogOpen) {
        closeClearChatDialog();
        return;
      }
      if (event.key === "Escape") closeNavigation(true);
      if (event.key === "Tab" && clearDialogOpen) {
        const first = $("#clear-chat-cancel");
        const last = $("#clear-chat-confirm");
        if (event.shiftKey && document.activeElement === first) {
          event.preventDefault();
          last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault();
          first.focus();
        }
      }
    });
    if (mobileNavigation.addEventListener) mobileNavigation.addEventListener("change", syncNavigationMode);
    else mobileNavigation.addListener(syncNavigationMode);
    syncNavigationMode();
    $$("[data-mode]").forEach((button) => button.addEventListener("click", () => sendMode(button.dataset.mode)));
    $$("[data-direction]").forEach((button) => button.addEventListener("click", () => sendMotion(button.dataset.direction)));
    $("#motion-stop").addEventListener("click", stopRobot);
    $("#global-stop").addEventListener("click", stopRobot);
    $("#refresh-button").addEventListener("click", () => refreshStatus(false));
    $("#speed-input").addEventListener("input", updateStepEstimate);
    $("#duration-input").addEventListener("input", updateStepEstimate);
    $("#clear-events").addEventListener("click", () => { state.events = []; renderEvents(); });
    $("#provider-select").addEventListener("change", fillProviderFields);
    $("#clear-chat-context").addEventListener("click", openClearChatDialog);
    $("#clear-chat-cancel").addEventListener("click", closeClearChatDialog);
    $("#clear-chat-confirm").addEventListener("click", clearChatContext);
    $("#clear-chat-layer").addEventListener("click", (event) => {
      if (event.target === event.currentTarget) closeClearChatDialog();
    });

    $("#auth-form").addEventListener("submit", async (event) => {
      event.preventDefault();
      try {
        const result = await api("/api/v1/session/login", { method: "POST", body: { password: $("#auth-password").value } });
        state.csrf = result.csrf;
        state.authenticated = true;
        hideAuth();
        const [config, status] = await Promise.all([loadConfig(), refreshStatus(true)]);
        if (status && status.network && status.network.mode === "access_point" && config.wifi && !config.wifi.ssid) {
          switchView("settings");
        }
        connectEvents();
        addEvent("SESSION", "控制台已连接");
      } catch (error) { $("#auth-error").textContent = error.message; }
    });

    $("#logout-button").addEventListener("click", async () => {
      try { await api("/api/v1/session/logout", { method: "POST", body: {} }); } catch (_error) { /* Local cleanup still applies. */ }
      state.authenticated = false;
      state.csrf = "";
      if (state.websocket) state.websocket.close();
      showAuth(false);
    });

    $("#chat-form").addEventListener("submit", async (event) => {
      event.preventDefault();
      const input = $("#chat-input");
      const message = input.value.trim();
      if (!message || state.busy) return;
      appendMessage("user", message);
      input.value = "";
      state.busy = true;
      const button = $("#chat-form button");
      button.disabled = true;
      $("#clear-chat-context").disabled = true;
      try {
        const result = await api("/api/v1/chat", { method: "POST", body: { message } });
        appendMessage("assistant", result.text || "模型未返回文本");
      } catch (error) {
        appendMessage("assistant", `请求失败：${error.message}`);
      } finally {
        state.busy = false;
        button.disabled = false;
        $("#clear-chat-context").disabled = false;
        input.focus();
      }
    });

    $("#save-key").addEventListener("click", async () => {
      const apiKey = $("#provider-key").value.trim();
      if (!apiKey) return toast("请输入新的 API Key", true);
      try {
        await api("/api/v1/secrets", { method: "PUT", body: { provider: $("#provider-select").value, api_key: apiKey } });
        await loadConfig();
        toast("API Key 已更新");
      } catch (error) { toast(error.message, true); }
    });

    $("#settings-form").addEventListener("submit", async (event) => {
      event.preventDefault();
      const selected = $("#provider-select").value;
      const providers = JSON.parse(JSON.stringify(state.config.providers));
      providers[selected].model = $("#provider-model").value.trim();
      providers[selected].base_url = $("#provider-url").value.trim().replace(/\/$/, "");
      const wifi = { ssid: $("#wifi-ssid").value.trim(), password: $("#wifi-password").value };
      const motion = {
        soft_rate_sps: Number($("#motion-rate").value),
        accel_sps2: Number($("#motion-accel").value),
        hold_ms: Number($("#motion-hold").value),
      };
      try {
        const result = await api("/api/v1/config", { method: "PUT", body: { active_provider: selected, providers, wifi, motion } });
        await loadConfig();
        toast(result.restart_required ? "设置已保存，网络修改将在重启后生效" : "设置已保存");
      } catch (error) { toast(error.message, true); }
    });
  }

  async function bootstrap() {
    bindUi();
    renderEvents();
    if (demoMode) {
      $("#auth-layer").classList.add("hidden");
      state.config = JSON.parse(JSON.stringify(demoConfig));
      fillSettings(state.config);
      renderStatus(Object.assign({}, demoStatus));
      addEvent("DEMO", "交互预览已启动");
      appendMessage("assistant", "本地控制链路已就绪。可切换模式、测试运动控制或发送指令。\n");
      return;
    }
    try {
      const initial = await api("/api/v1/bootstrap");
      showAuth(initial.setup_required);
    } catch (error) {
      showAuth(false);
      $("#auth-error").textContent = error.message;
    }
  }

  setInterval(() => refreshStatus(true), 1000);
  bootstrap();
}());
