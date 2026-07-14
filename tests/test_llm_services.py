import asyncio
import tempfile
import unittest

from firmware.esp32.providers.deepseek_provider import DeepSeekProvider
from firmware.esp32.providers.openai_provider import OpenAIProvider
from firmware.esp32.services.config_service import ConfigService
from firmware.esp32.services.http_client import parse_url
from firmware.esp32.services.llm_service import LlmService
from firmware.esp32.tools.robot_tools import TOOL_SCHEMAS, RobotTools


class FakeHttpClient:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    async def post_json(self, url, payload, headers=None, timeout_s=60):
        self.requests.append((url, payload, headers, timeout_s))
        return self.responses.pop(0)


class FakeDevice:
    def __init__(self):
        self.calls = []

    async def stop(self):
        self.calls.append(("stop",))
        return 1

    async def set_expression(self, expression):
        self.calls.append(("expression", expression))
        return 2

    async def set_mode(self, mode):
        self.calls.append(("mode", mode))
        return 3

    async def move(self, left, right, rate, accel, timeout):
        self.calls.append(("move", left, right, rate, accel, timeout))
        return {"ok": True, "left_steps": left, "right_steps": right}


class LlmServiceTests(unittest.TestCase):
    def test_parse_url(self):
        self.assertEqual(
            parse_url("https://api.example.com:8443/v1/test"),
            ("https", "api.example.com", 8443, "/v1/test"),
        )

    def test_openai_tool_call_and_result_shapes(self):
        async def scenario():
            messages = [{"role": "user", "content": "stop"}]
            first_output = [
                {"type": "reasoning", "id": "reasoning-1", "summary": []},
                {"type": "function_call", "call_id": "call-1", "name": "robot_stop", "arguments": "{}"},
            ]
            second_output = [
                {"type": "function_call", "call_id": "call-2", "name": "robot_set_expression", "arguments": "{\"expression\":\"happy\"}"},
            ]
            client = FakeHttpClient(
                [
                    {"id": "resp-1", "output": first_output},
                    {"id": "resp-2", "output": second_output},
                    {"id": "resp-3", "output_text": "Stopped and smiling."},
                ]
            )
            provider = OpenAIProvider(
                client,
                {"base_url": "https://api.openai.com", "model": "test-model", "timeout_s": 10, "max_output_tokens": 64},
                "secret",
            )
            turn = await provider.create_turn(messages, TOOL_SCHEMAS)
            self.assertEqual(turn["tool_calls"][0]["name"], "robot_stop")
            second_turn = await provider.submit_tool_results(
                turn["continuation"],
                [{"call_id": "call-1", "output": {"ok": True}}],
                [],
                TOOL_SCHEMAS,
            )
            self.assertEqual(second_turn["tool_calls"][0]["name"], "robot_set_expression")
            second_payload = client.requests[1][1]
            self.assertNotIn("previous_response_id", second_payload)
            self.assertEqual(second_payload["input"][:3], messages + first_output)
            self.assertEqual(second_payload["input"][-1]["type"], "function_call_output")
            self.assertEqual(second_payload["input"][-1]["call_id"], "call-1")

            final = await provider.submit_tool_results(
                second_turn["continuation"],
                [{"call_id": "call-2", "output": {"ok": True}}],
                [],
                TOOL_SCHEMAS,
            )
            self.assertEqual(final["text"], "Stopped and smiling.")
            third_payload = client.requests[2][1]
            self.assertNotIn("previous_response_id", third_payload)
            self.assertEqual(third_payload["input"][:3], messages + first_output)
            self.assertIn(second_output[0], third_payload["input"])
            self.assertEqual(third_payload["input"][-1]["call_id"], "call-2")

        asyncio.run(scenario())

    def test_deepseek_tool_call_and_result_shapes(self):
        async def scenario():
            assistant_call = {
                "role": "assistant",
                "content": None,
                "tool_calls": [{"id": "call-2", "type": "function", "function": {"name": "robot_set_expression", "arguments": "{\"expression\":\"happy\"}"}}],
            }
            client = FakeHttpClient(
                [
                    {"choices": [{"message": assistant_call}]},
                    {"choices": [{"message": {"role": "assistant", "content": "Done."}}]},
                ]
            )
            provider = DeepSeekProvider(
                client,
                {"base_url": "https://api.deepseek.com", "model": "deepseek-chat", "timeout_s": 10, "max_output_tokens": 64},
                "secret",
            )
            turn = await provider.create_turn([{"role": "user", "content": "smile"}], TOOL_SCHEMAS)
            self.assertEqual(turn["tool_calls"][0]["arguments"]["expression"], "happy")
            final = await provider.submit_tool_results(
                turn["continuation"],
                [{"call_id": "call-2", "output": {"ok": True}}],
                [],
                TOOL_SCHEMAS,
            )
            self.assertEqual(final["text"], "Done.")
            self.assertEqual(client.requests[1][1]["messages"][-1]["role"], "tool")

        asyncio.run(scenario())

    def test_robot_move_is_bounded_and_translated(self):
        async def scenario():
            with tempfile.TemporaryDirectory() as root:
                config = ConfigService(root)
                config.load()
                device = FakeDevice()
                tools = RobotTools(device, config)
                result = await tools.execute(
                    {"name": "robot_move", "arguments": {"direction": "left", "speed_percent": 50, "duration_ms": 500}}
                )
                self.assertTrue(result["ok"])
                call = device.calls[-1]
                self.assertEqual(call[0], "move")
                self.assertLess(call[1], 0)
                self.assertGreater(call[2], 0)
                self.assertEqual(call[5], 500)

        asyncio.run(scenario())

    def test_llm_service_rejects_missing_key(self):
        async def scenario():
            with tempfile.TemporaryDirectory() as root:
                config = ConfigService(root)
                config.load()
                service = LlmService(config, FakeDevice(), FakeHttpClient([]))
                with self.assertRaisesRegex(RuntimeError, "API key"):
                    await service.chat("hello")

        asyncio.run(scenario())

    def test_provider_switch_starts_a_new_history(self):
        async def scenario():
            with tempfile.TemporaryDirectory() as root:
                config = ConfigService(root)
                config.load()
                config.set_provider_key("deepseek", "key")
                config.set_provider_key("openai", "key")
                config.config["providers"]["openai"]["model"] = "test-model"
                client = FakeHttpClient([
                    {"choices": [{"message": {"role": "assistant", "content": "one"}}]},
                    {"id": "resp", "output_text": "two"},
                ])
                service = LlmService(config, FakeDevice(), client)
                await service.chat("first")
                config.config["active_provider"] = "openai"
                await service.chat("second")
                openai_input = client.requests[1][1]["input"]
                self.assertEqual([item["role"] for item in openai_input], ["system", "user"])
                self.assertEqual(openai_input[-1]["content"], "second")

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
