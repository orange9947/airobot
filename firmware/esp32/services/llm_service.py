"""Provider-independent conversation and robot-tool orchestration."""

from firmware.esp32.providers.deepseek_provider import DeepSeekProvider
from firmware.esp32.providers.openai_provider import OpenAIProvider
from firmware.esp32.tools.robot_tools import TOOL_SCHEMAS, RobotTools

SYSTEM_PROMPT = (
    "You control a small differential-drive desktop robot through the provided tools. "
    "Use only those tools for physical actions, obey their bounds, and report tool failures clearly. "
    "Never claim an action succeeded until its tool result says it succeeded."
)


class LlmService:
    def __init__(self, config, device, http_client):
        self.config = config
        self.device = device
        self.http_client = http_client
        self.tools = RobotTools(device, config)
        self.history = []
        self.history_provider = None
        self.listeners = []

    def add_listener(self, listener):
        self.listeners.append(listener)

    def _emit(self, event):
        for listener in tuple(self.listeners):
            try:
                listener(event)
            except Exception:
                pass

    def _provider(self):
        name = self.config.config["active_provider"]
        provider_config = self.config.config["providers"][name]
        key = self.config.provider_key(name)
        if not key:
            raise RuntimeError("{} API key is not configured".format(name))
        if not provider_config.get("model"):
            raise RuntimeError("{} model is not configured".format(name))
        if name == "openai":
            return OpenAIProvider(self.http_client, provider_config, key)
        return DeepSeekProvider(self.http_client, provider_config, key)

    async def chat(self, prompt):
        prompt = (prompt or "").strip()
        if not prompt or len(prompt) > 2000:
            raise ValueError("message must contain 1-2000 characters")
        active_provider = self.config.config["active_provider"]
        if self.history_provider != active_provider:
            self.history = []
            self.history_provider = active_provider
        provider = self._provider()
        self.history.append({"role": "user", "content": prompt})
        messages = [{"role": "system", "content": SYSTEM_PROMPT}] + self.history
        turn = await provider.create_turn(messages, TOOL_SCHEMAS)
        for _ in range(3):
            if not turn["tool_calls"]:
                text = turn.get("text") or ""
                self.history.append({"role": "assistant", "content": text})
                self.history = self.history[-20:]
                response = {"text": text, "provider": self.config.config["active_provider"]}
                self._emit({"type": "chat", "response": response})
                return response
            results = []
            for call in turn["tool_calls"]:
                output = await self.tools.execute(call)
                result = {"call_id": call["id"], "name": call["name"], "output": output}
                results.append(result)
                self._emit({"type": "tool", "result": result})
            turn = await provider.submit_tool_results(
                turn["continuation"], results, self.history, TOOL_SCHEMAS
            )
        raise RuntimeError("model exceeded the tool-call limit")

    def clear(self):
        self.history = []
