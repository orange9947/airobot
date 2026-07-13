"""DeepSeek Chat Completions adapter."""

try:
    import json
except ImportError:
    import ujson as json


class DeepSeekProvider:
    def __init__(self, client, config, api_key):
        self.client = client
        self.config = config
        self.api_key = api_key

    def _headers(self):
        return {"Authorization": "Bearer " + self.api_key}

    def _url(self):
        return self.config["base_url"].rstrip("/") + "/chat/completions"

    @staticmethod
    def _tools(tools):
        return [
            {
                "type": "function",
                "function": {
                    "name": tool["name"],
                    "description": tool["description"],
                    "parameters": tool["parameters"],
                },
            }
            for tool in tools
        ]

    def _parse(self, response, messages):
        message = response["choices"][0]["message"]
        calls = []
        for call in message.get("tool_calls", []) or []:
            function = call.get("function", {})
            try:
                arguments = json.loads(function.get("arguments", "{}"))
            except ValueError:
                arguments = {}
            calls.append({"id": call.get("id", ""), "name": function.get("name", ""), "arguments": arguments})
        return {
            "text": message.get("content") or "",
            "tool_calls": calls,
            "continuation": {"messages": messages + [message]},
        }

    async def create_turn(self, messages, tools):
        body = {
            "model": self.config["model"],
            "messages": messages,
            "tools": self._tools(tools),
            "stream": False,
            "max_tokens": self.config.get("max_output_tokens", 256),
        }
        response = await self.client.post_json(
            self._url(), body, self._headers(), self.config.get("timeout_s", 60)
        )
        return self._parse(response, messages)

    async def submit_tool_results(self, continuation, results, _messages, tools):
        messages = list(continuation["messages"])
        for result in results:
            messages.append(
                {
                    "role": "tool",
                    "tool_call_id": result["call_id"],
                    "content": json.dumps(result["output"]),
                }
            )
        body = {
            "model": self.config["model"],
            "messages": messages,
            "tools": self._tools(tools),
            "stream": False,
            "max_tokens": self.config.get("max_output_tokens", 256),
        }
        response = await self.client.post_json(
            self._url(), body, self._headers(), self.config.get("timeout_s", 60)
        )
        return self._parse(response, messages)
