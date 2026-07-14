"""OpenAI Responses API adapter."""

try:
    import json
except ImportError:
    import ujson as json


class OpenAIProvider:
    def __init__(self, client, config, api_key):
        self.client = client
        self.config = config
        self.api_key = api_key

    def _headers(self):
        return {"Authorization": "Bearer " + self.api_key}

    def _url(self):
        return self.config["base_url"].rstrip("/") + "/v1/responses"

    def _parse(self, response, request_input):
        text_parts = []
        tool_calls = []
        if response.get("output_text"):
            text_parts.append(response["output_text"])
        for item in response.get("output", []):
            if item.get("type") == "function_call":
                try:
                    arguments = json.loads(item.get("arguments", "{}"))
                except ValueError:
                    arguments = {}
                tool_calls.append({"id": item.get("call_id", ""), "name": item.get("name", ""), "arguments": arguments})
            elif item.get("type") == "message":
                for content in item.get("content", []):
                    if content.get("type") == "output_text":
                        text_parts.append(content.get("text", ""))
        output_items = response.get("output", []) or []
        return {
            "text": "\n".join(part for part in text_parts if part),
            "tool_calls": tool_calls,
            "continuation": {"input": list(request_input) + list(output_items)},
        }

    async def create_turn(self, messages, tools):
        body = {
            "model": self.config["model"],
            "input": messages,
            "tools": tools,
            "max_output_tokens": self.config.get("max_output_tokens", 256),
        }
        response = await self.client.post_json(
            self._url(), body, self._headers(), self.config.get("timeout_s", 60)
        )
        return self._parse(response, messages)

    async def submit_tool_results(self, continuation, results, _messages, tools):
        tool_outputs = [
            {"type": "function_call_output", "call_id": result["call_id"], "output": json.dumps(result["output"])}
            for result in results
        ]
        request_input = list(continuation["input"]) + tool_outputs
        body = {
            "model": self.config["model"],
            "input": request_input,
            "tools": tools,
            "max_output_tokens": self.config.get("max_output_tokens", 256),
        }
        response = await self.client.post_json(
            self._url(), body, self._headers(), self.config.get("timeout_s", 60)
        )
        return self._parse(response, request_input)
