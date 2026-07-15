import asyncio
import json
import tempfile
import unittest

from firmware.esp32.providers.deepseek_provider import DeepSeekProvider
from firmware.esp32.providers.openai_provider import OpenAIProvider
from firmware.esp32.services.config_service import ConfigService
from firmware.esp32.services.http_client import AsyncJsonClient, HttpClientError, parse_url
from firmware.esp32.services.llm_service import LlmService
from firmware.esp32.tools.robot_tools import TOOL_SCHEMAS, RobotTools


class FakeHttpClient:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    async def post_json(self, url, payload, headers=None, timeout_s=60, response_parser=None):
        self.requests.append((url, payload, headers, timeout_s, response_parser))
        response = self.responses.pop(0)
        if response_parser is not None and isinstance(response, tuple):
            text, response_headers = response
            return response_parser(text, response_headers)
        return response


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


class FailingExpressionDevice(FakeDevice):
    async def set_expression(self, expression):
        self.calls.append(("expression", expression))
        raise RuntimeError("STM32 rejected command: 5")


class LlmServiceTests(unittest.TestCase):
    @staticmethod
    def _sse(*events):
        blocks = []
        for event in events:
            if isinstance(event, str):
                blocks.append(event)
            else:
                blocks.append("data: " + json.dumps(event, separators=(",", ":")))
        return "\n\n".join(blocks) + "\n\n"

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
            first_payload = client.requests[0][1]
            self.assertNotIn("max_output_tokens", first_payload)
            self.assertEqual(set(first_payload), {"model", "input", "tools"})
            second_turn = await provider.submit_tool_results(
                turn["continuation"],
                [{"call_id": "call-1", "output": {"ok": True}}],
                [],
                TOOL_SCHEMAS,
            )
            self.assertEqual(second_turn["tool_calls"][0]["name"], "robot_set_expression")
            second_payload = client.requests[1][1]
            self.assertNotIn("previous_response_id", second_payload)
            self.assertNotIn("max_output_tokens", second_payload)
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
            self.assertNotIn("max_output_tokens", third_payload)
            self.assertEqual(third_payload["input"][:3], messages + first_output)
            self.assertIn(second_output[0], third_payload["input"])
            self.assertEqual(third_payload["input"][-1]["call_id"], "call-2")

        asyncio.run(scenario())

    def test_openai_response_parser_accepts_json(self):
        response = OpenAIProvider._decode_response(
            '{"id":"resp-json","output_text":"Hello","output":[]}',
            {"content-type": "application/json; charset=utf-8"},
        )
        self.assertEqual(response["output_text"], "Hello")

    def test_openai_sse_reconstructs_tool_call_and_followup_text(self):
        async def scenario():
            reasoning = {"type": "reasoning", "id": "reasoning-1", "summary": []}
            function_call = {
                "type": "function_call",
                "call_id": "call-1",
                "name": "robot_stop",
                "arguments": "{}",
            }
            message = {
                "type": "message",
                "id": "message-1",
                "content": [{"type": "output_text", "text": "Done."}],
            }
            first_body = self._sse(
                ": keep-alive",
                {"type": "response.output_text.delta", "delta": "ignored"},
                {"type": "response.output_item.done", "output_index": 1, "item": function_call},
                {"type": "response.output_item.done", "output_index": 0, "item": reasoning},
                'data: {"type":"response.completed",\ndata: "response":{"id":"resp-1","output":[]}}',
                "data: [DONE]",
            )
            second_body = self._sse(
                {"type": "response.output_item.done", "output_index": 0, "item": message},
                {"type": "response.completed", "response": {"id": "resp-2", "output": []}},
            )
            client = FakeHttpClient([
                (first_body, {"content-type": "text/event-stream; charset=utf-8"}),
                (second_body, {"content-type": "text/event-stream"}),
            ])
            provider = OpenAIProvider(
                client,
                {"base_url": "https://api.openai.com", "model": "test-model", "timeout_s": 10},
                "secret",
            )

            turn = await provider.create_turn([{"role": "user", "content": "stop"}], TOOL_SCHEMAS)
            self.assertEqual(turn["tool_calls"][0]["name"], "robot_stop")
            self.assertEqual(turn["continuation"]["input"][-2:], [reasoning, function_call])
            self.assertIsNotNone(client.requests[0][4])

            final = await provider.submit_tool_results(
                turn["continuation"],
                [{"call_id": "call-1", "output": {"ok": True}}],
                [],
                TOOL_SCHEMAS,
            )
            self.assertEqual(final["text"], "Done.")
            self.assertEqual(final["continuation"]["input"][-1], message)

        asyncio.run(scenario())

    def test_openai_sse_rejects_invalid_terminal_shapes(self):
        valid_item = {"type": "message", "content": []}
        cases = {
            "malformed event JSON": "data: {bad json}\n\n",
            "missing completion": self._sse(
                {"type": "response.output_item.done", "output_index": 0, "item": valid_item}
            ),
            "failed response": self._sse(
                {"type": "response.failed", "response": {"error": {"message": "failed"}}}
            ),
            "duplicate output index": self._sse(
                {"type": "response.output_item.done", "output_index": 0, "item": valid_item},
                {"type": "response.output_item.done", "output_index": 0, "item": valid_item},
                {"type": "response.completed", "response": {"output": []}},
            ),
            "negative output index": self._sse(
                {"type": "response.output_item.done", "output_index": -1, "item": valid_item},
                {"type": "response.completed", "response": {"output": []}},
            ),
            "oversized output index": self._sse(
                {"type": "response.output_item.done", "output_index": 16, "item": valid_item},
                {"type": "response.completed", "response": {"output": []}},
            ),
            "boolean output index": self._sse(
                {"type": "response.output_item.done", "output_index": True, "item": valid_item},
                {"type": "response.completed", "response": {"output": []}},
            ),
            "non-object item": self._sse(
                {"type": "response.output_item.done", "output_index": 0, "item": []},
                {"type": "response.completed", "response": {"output": []}},
            ),
            "duplicate completion": self._sse(
                {"type": "response.completed", "response": {"output": []}},
                {"type": "response.completed", "response": {"output": []}},
            ),
            "non-list terminal output": self._sse(
                {"type": "response.completed", "response": {"output": {}}}
            ),
            "non-contiguous output": self._sse(
                {"type": "response.output_item.done", "output_index": 1, "item": valid_item},
                {"type": "response.completed", "response": {"output": []}},
            ),
        }
        for name, body in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    OpenAIProvider._decode_response(body, {"content-type": "text/event-stream"})

        with self.assertRaises(ValueError):
            OpenAIProvider._decode_response("plain text", {"content-type": "text/plain"})

    def test_openai_incomplete_sse_never_executes_tool(self):
        async def scenario():
            with tempfile.TemporaryDirectory() as root:
                config = ConfigService(root)
                config.load()
                config.set_provider_key("openai", "key")
                config.config["active_provider"] = "openai"
                config.config["providers"]["openai"]["model"] = "test-model"
                function_call = {
                    "type": "function_call",
                    "call_id": "call-unsafe",
                    "name": "robot_set_expression",
                    "arguments": '{"expression":"happy"}',
                }
                body = self._sse({
                    "type": "response.output_item.done",
                    "output_index": 0,
                    "item": function_call,
                })
                device = FakeDevice()
                service = LlmService(
                    config,
                    device,
                    FakeHttpClient([(body, {"content-type": "text/event-stream"})]),
                )
                with self.assertRaises(ValueError):
                    await service.chat("smile")
                self.assertEqual(device.calls, [])

        asyncio.run(scenario())

    def test_http_response_parser_errors_are_normalized(self):
        client = AsyncJsonClient()

        def fail_parser(_text, _headers):
            raise ValueError("private parser detail")

        with self.assertRaises(HttpClientError) as caught:
            client._decode_body("x" * 600, 200, {"content-type": "text/plain"}, fail_parser)
        self.assertEqual(str(caught.exception), "invalid response payload")
        self.assertEqual(len(caught.exception.body), 512)

        with self.assertRaisesRegex(HttpClientError, "invalid JSON response"):
            client._decode_body("not json", 200, {"content-type": "application/json"})

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
            self.assertEqual(client.requests[0][1]["max_tokens"], 64)
            final = await provider.submit_tool_results(
                turn["continuation"],
                [{"call_id": "call-2", "output": {"ok": True}}],
                [],
                TOOL_SCHEMAS,
            )
            self.assertEqual(final["text"], "Done.")
            self.assertEqual(client.requests[1][1]["messages"][-1]["role"], "tool")
            self.assertEqual(client.requests[1][1]["max_tokens"], 64)

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

    def test_tool_device_failure_is_returned_to_model(self):
        async def scenario():
            with tempfile.TemporaryDirectory() as root:
                config = ConfigService(root)
                config.load()
                config.set_provider_key("deepseek", "key")
                first_call = {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [{
                        "id": "call-failed",
                        "type": "function",
                        "function": {
                            "name": "robot_set_expression",
                            "arguments": "{\"expression\":\"happy\"}",
                        },
                    }],
                }
                client = FakeHttpClient([
                    {"choices": [{"message": first_call}]},
                    {"choices": [{"message": {"role": "assistant", "content": "The expression could not be changed."}}]},
                ])
                service = LlmService(config, FailingExpressionDevice(), client)
                result = await service.chat("smile")
                self.assertIn("could not", result["text"])
                tool_message = client.requests[1][1]["messages"][-1]
                self.assertEqual(tool_message["role"], "tool")
                self.assertIn('"ok": false', tool_message["content"])
                self.assertIn("STM32 rejected", tool_message["content"])

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
